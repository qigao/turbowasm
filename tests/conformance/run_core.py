#!/usr/bin/env python3
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

SUMMARY_RE = re.compile(
    r"CONFORMANCE pass=(\d+) fail=(\d+) unsupported=(\d+) total=(\d+)"
)
MIR_REPLAY_RE = re.compile(
    r"MIR_REPLAY compiled=(\d+) interpret_only=(\d+) cold=(\d+) calls=(\d+)"
)
RESULT_RE = re.compile(
    r"^RESULT line=(\d+) command=([^ ]+) outcome=(pass|fail|unsupported|mixed|none)$",
    re.MULTILINE,
)

TRAP_MAP = [
    ("unreachable", 1),
    ("call stack exhausted", 2),
    ("integer divide by zero", 3),
    ("integer overflow", 4),
    ("invalid conversion to integer", 9),
    ("unaligned atomic", 11),
    ("expected shared memory", 12),
    ("out of bounds memory access", 5),
    ("out of bounds table access", 6),
    ("undefined element", 6),
    ("uninitialized element", 7),
    ("indirect call type mismatch", 8),
    ("null reference", 10),
    ("null function reference", 10),
    ("out of bounds array access", 14),
    ("cast failure", 15),
    ("null array reference", 10),
    ("null structure reference", 10),
    ("null i31 reference", 10),
    ("indirect call", 8),
    ("cast", 15),
    ("out of bounds", 5),
]


def hex_utf8(text):
    return text.encode("utf-8").hex()


def safe_reason(text):
    return str(text).replace("\t", " ").replace("\r", " ").replace("\n", " ")


def encode_value(value, expected=False):
    value_type = value.get("type")
    raw = value.get("value")

    if value_type in ("i32", "i64", "f32", "f64"):
        if not isinstance(raw, str):
            return None, f"{value_type} value is not a string"
        if (
            expected
            and value_type in ("f32", "f64")
            and raw in ("nan:canonical", "nan:arithmetic")
        ):
            return f"{value_type}:{raw}", None
        if value_type in ("i32", "i64") and re.fullmatch(r"-?[0-9]+", raw):
            width = 32 if value_type == "i32" else 64
            number = int(raw)
            if number < -(1 << (width - 1)) or number >= 1 << width:
                return None, f"{value_type} value outside bit width"
            return f"{value_type}:{number % (1 << width)}", None
        if not raw.isdigit():
            return None, f"{value_type} non-decimal encoding unsupported"
        return f"{value_type}:{raw}", None

    if value_type == "funcref":
        if raw == "null":
            return "funcref:null", None
        if expected and raw in ("0", None):
            # WABT's script expected (ref.func) pattern is encoded via
            # Const::set_funcref(), which writes the sentinel payload 0.
            # It means any non-null function reference, not function index 0.
            return "funcref:nonnull", None
        return None, "funcref non-null argument/payload unsupported"

    if value_type == "externref":
        if expected and raw is None:
            return "externref:nonnull", None
        if raw == "null":
            return "externref:null", None
        if isinstance(raw, str) and raw.isdigit():
            return f"externref:{raw}", None
        return None, "externref payload encoding unsupported"

    if value_type == "refnull" and expected:
        return "ref:null", None
    if value_type == "nullref":
        return "gcref:null", None
    if value_type == "anyref" and isinstance(raw, str) and raw.isdigit():
        return f"hostref:{raw}", None
    if value_type in ("anyref", "eqref", "i31ref", "structref", "arrayref"):
        if raw == "null":
            return "gcref:null", None
        if expected and raw is None:
            return f"gcref:{value_type}", None
        return None, f"{value_type} payload encoding unsupported"

    if value_type in ("nullfuncref", "nullexternref", "nullexnref", "exnref") and raw in ("null", None):
        carrier = {"nullfuncref": "funcref", "nullexternref": "externref"}.get(value_type, "exnref")
        return f"{carrier}:null", None

    if value_type == "v128":
        lane_type = value.get("lane_type")
        lanes = value.get("value")
        lane_counts = {
            "i8": 16,
            "i16": 8,
            "i32": 4,
            "i64": 2,
            "f32": 4,
            "f64": 2,
        }
        expected_count = lane_counts.get(lane_type)
        if expected_count is None:
            return None, f"v128 lane type {lane_type!r} unsupported"
        if not isinstance(lanes, list) or len(lanes) != expected_count:
            return None, "v128 lane count mismatch"

        encoded_lanes = []
        for lane in lanes:
            if not isinstance(lane, str):
                return None, "v128 lane value is not a string"
            if (
                expected
                and lane_type in ("f32", "f64")
                and lane in ("nan:canonical", "nan:arithmetic")
            ):
                encoded_lanes.append(lane)
                continue
            if lane_type.startswith("i") and re.fullmatch(r"-?[0-9]+", lane):
                width = int(lane_type[1:])
                number = int(lane)
                if number < -(1 << (width - 1)) or number >= 1 << width:
                    return None, f"v128 {lane_type} lane outside bit width"
                encoded_lanes.append(str(number % (1 << width)))
                continue
            if not lane.isdigit():
                return None, f"v128 {lane_type} non-decimal lane unsupported"
            encoded_lanes.append(lane)

        return (
            f"v128:{lane_type}:" + ";".join(encoded_lanes),
            None,
        )

    if value_type == "exnref":
        return None, "exnref non-null/reference payload unsupported"

    return None, f"value type {value_type!r} unsupported"


def encode_values(values, expected=False):
    encoded = []
    for value in values or []:
        token, reason = encode_value(value, expected=expected)
        if token is None:
            return None, reason
        encoded.append(token)
    return ",".join(encoded) if encoded else "-", None


def trap_kind(text):
    lowered = text.lower()
    for pattern, kind in TRAP_MAP:
        if pattern in lowered:
            return kind
    return None


def action_slot(action, named_slots, current_slot):
    module_name = action.get("module")
    if module_name is None:
        return current_slot
    return named_slots.get(module_name)


def convert_json(json_path, manifest_path, wasm_tools=None):
    with open(json_path, "r", encoding="utf-8") as handle:
        document = json.load(handle)

    json_dir = os.path.dirname(os.path.abspath(json_path))
    named_slots = {}
    definitions = {}
    last_definition = None
    frontend_passed = 0
    frontend_failed = 0
    current_slot = None
    next_slot = 0
    lines = ["TWCF1"]

    def emit_unsupported(line, reason):
        lines.append(
            f"unsupported\t{line}\t{safe_reason(reason)}"
        )

    def encode_invoke(command, action, command_type, expected=None, trap=None):
        line = command.get("line", 0)
        if action.get("type") not in ("invoke", "get"):
            emit_unsupported(line, "unknown action type")
            return
        suffix = "_get" if action["type"] == "get" else ""

        slot = action_slot(action, named_slots, current_slot)
        if slot is None:
            emit_unsupported(line, "named/default module not available")
            return

        args, reason = encode_values(action.get("args", []), expected=False)
        if args is None:
            emit_unsupported(line, reason)
            return

        field = action.get("field")
        if not isinstance(field, str):
            emit_unsupported(line, "invoke field missing")
            return

        slot_text = str(slot)
        field_hex = hex_utf8(field)

        if command_type == "action":
            lines.append(
                f"action{suffix}\t{line}\t{slot_text}\t{field_hex}\t{args}"
            )
            return

        if command_type == "assert_return":
            either = command.get("either")
            if (either is None and len(expected or []) == 1
                    and expected[0].get("type") == "either"):
                either = expected[0].get("values")
            if either is not None:
                if not isinstance(either, list) or not either:
                    emit_unsupported(line, "either expectation is empty")
                    return
                encoded_alternatives = []
                for alternative in either:
                    token, reason = encode_value(
                        alternative, expected=True
                    )
                    if token is None:
                        emit_unsupported(line, reason)
                        return
                    encoded_alternatives.append(token)
                lines.append(
                    f"assert_return_either{suffix}\t{line}\t{slot_text}"
                    f"\t{field_hex}\t{args}\t"
                    + "|".join(encoded_alternatives)
                )
                return

            encoded_expected, reason = encode_values(
                expected or [], expected=True
            )
            if encoded_expected is None:
                emit_unsupported(line, reason)
                return
            lines.append(
                f"assert_return{suffix}\t{line}\t{slot_text}\t{field_hex}"
                f"\t{args}\t{encoded_expected}"
            )
            return

        if command_type == "assert_trap":
            if trap is None:
                emit_unsupported(line, "trap text has no TurboWasm mapping")
                return
            lines.append(
                f"assert_trap\t{line}\t{slot_text}\t{field_hex}"
                f"\t{args}\t{trap}"
            )
            return

        if command_type == "assert_exception" and not suffix:
            lines.append(f"assert_exception\t{line}\t{slot_text}\t{field_hex}\t{args}")
            return

        raise AssertionError(command_type)

    for command in document.get("commands", []):
        command_type = command.get("type")
        line = command.get("line", 0)

        if command_type in ("module", "module_definition"):
            filename = command.get("filename")
            if not isinstance(filename, str):
                emit_unsupported(line, "module filename missing")
                continue

            path = os.path.abspath(os.path.join(json_dir, filename))
            if command.get("module_type") == "text":
                if wasm_tools is None:
                    emit_unsupported(line, "quoted text module requires wasm-tools")
                    continue
                binary_path = path + ".wasm"
                parsed = subprocess.run([wasm_tools, "parse", path, "-o", binary_path],
                                        capture_output=True, text=True, check=False)
                if parsed.returncode != 0:
                    emit_unsupported(line, f"quoted module conversion failed: {parsed.stderr}")
                    continue
                path = binary_path
            if (command_type == "module_definition" or
                    command.get("definition") in (True, "true")):
                last_definition = path
                if isinstance(command.get("name"), str):
                    definitions[command["name"]] = path
                lines.append(f"module_definition\t{line}\t{path}")
                continue

            slot = next_slot
            next_slot += 1
            current_slot = slot

            name = command.get("name")
            if isinstance(name, str):
                named_slots[name] = slot

            lines.append(f"module\t{line}\t{slot}\t{path}")
            continue

        if command_type == "module_instance":
            name = command.get("module")
            path = definitions.get(name) if name is not None else last_definition
            if path is None:
                emit_unsupported(line, "module definition unavailable")
                continue
            slot = next_slot
            next_slot += 1
            current_slot = slot
            if isinstance(command.get("instance"), str):
                named_slots[command["instance"]] = slot
            lines.append(f"module\t{line}\t{slot}\t{path}")
            continue

        if command_type == "register":
            source_name = command.get("name")
            slot = (
                named_slots.get(source_name)
                if isinstance(source_name, str)
                else current_slot
            )
            alias = command.get("as")
            if slot is None or not isinstance(alias, str):
                emit_unsupported(line, "register target/name unavailable")
                continue
            lines.append(
                f"register\t{line}\t{slot}\t{hex_utf8(alias)}"
            )
            continue

        if command_type == "action":
            encode_invoke(
                command, command.get("action", {}), "action"
            )
            continue

        if command_type == "assert_return":
            encode_invoke(
                command,
                command.get("action", {}),
                "assert_return",
                expected=command.get("expected", []),
            )
            continue

        if command_type == "assert_trap":
            encode_invoke(
                command,
                command.get("action", {}),
                "assert_trap",
                trap=trap_kind(command.get("text", "")),
            )
            continue

        if command_type == "assert_exhaustion":
            encode_invoke(
                command,
                command.get("action", {}),
                "assert_trap",
                trap=2,
            )
            continue

        if command_type == "assert_exception":
            encode_invoke(command, command.get("action", {}), "assert_exception")
            continue

        if command_type in (
            "assert_invalid",
            "assert_malformed",
            "assert_unlinkable",
            "assert_uninstantiable",
        ):
            filename = command.get("filename")
            if not isinstance(filename, str):
                emit_unsupported(line, f"{command_type} filename missing")
                continue
            path = os.path.abspath(os.path.join(json_dir, filename))
            if command.get("module_type") != "binary":
                if wasm_tools is None:
                    emit_unsupported(line, f"{command_type} text module requires a text frontend")
                    continue
                binary_path = path + ".wasm"
                parsed = subprocess.run([wasm_tools, "parse", path, "-o", binary_path],
                                        capture_output=True, text=True, check=False)
                if parsed.returncode != 0:
                    if command_type == "assert_malformed" and parsed.returncode == 1 and "error:" in parsed.stderr:
                        frontend_passed += 1
                    else:
                        frontend_failed += 1
                        print(f"TEXT_FRONTEND_FAILURE line={line} command={command_type} {parsed.stderr}")
                    continue
                if command_type == "assert_malformed":
                    frontend_failed += 1
                    print(f"TEXT_FRONTEND_FAILURE line={line} malformed text accepted")
                    continue
                path = binary_path
            lines.append(f"{command_type}\t{line}\t{path}")
            continue

        emit_unsupported(line, f"command {command_type!r} unsupported")

    with open(manifest_path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("\n".join(lines))
        handle.write("\n")
    return frontend_passed, frontend_failed



class _WastList:
    __slots__ = ("items", "start", "end", "line")

    def __init__(self, items, start, end, line):
        self.items = items
        self.start = start
        self.end = end
        self.line = line


def _wast_head(node):
    if not isinstance(node, _WastList) or not node.items:
        return None
    head = node.items[0]
    return head if isinstance(head, str) else None


def _wast_string(item):
    if (
        isinstance(item, tuple)
        and len(item) == 2
        and item[0] == "string"
    ):
        return item[1]
    return None


def _parse_wast_script(text):
    length = len(text)
    index = 0
    line = 1

    def skip_space():
        nonlocal index, line
        while index < length:
            ch = text[index]
            if ch.isspace():
                if ch == "\n":
                    line += 1
                index += 1
                continue
            if text.startswith(";;", index):
                index += 2
                while index < length and text[index] != "\n":
                    index += 1
                continue
            if text.startswith("(;", index):
                depth = 1
                index += 2
                while index < length and depth:
                    if text.startswith("(;", index):
                        depth += 1
                        index += 2
                    elif text.startswith(";)", index):
                        depth -= 1
                        index += 2
                    else:
                        if text[index] == "\n":
                            line += 1
                        index += 1
                if depth:
                    raise ValueError("unterminated block comment")
                continue
            break

    def parse_string():
        nonlocal index, line
        data = bytearray()
        index += 1
        while index < length:
            ch = text[index]
            if ch == '"':
                index += 1
                return ("string", data.decode("utf-8"))
            if ch == "\n":
                raise ValueError("newline in WAT string")
            if ch != "\\":
                data.extend(ch.encode("utf-8"))
                index += 1
                continue

            index += 1
            if index >= length:
                raise ValueError("unterminated WAT string escape")
            esc = text[index]
            if (
                index + 1 < length
                and esc in "0123456789abcdefABCDEF"
                and text[index + 1] in "0123456789abcdefABCDEF"
            ):
                data.append(int(text[index:index + 2], 16))
                index += 2
                continue
            escapes = {
                "n": b"\n",
                "r": b"\r",
                "t": b"\t",
                '"': b'"',
                "'": b"'",
                "\\": b"\\",
            }
            if esc not in escapes:
                raise ValueError(f"unsupported WAT string escape \\{esc}")
            data.extend(escapes[esc])
            index += 1
        raise ValueError("unterminated WAT string")

    def parse_atom():
        nonlocal index
        start = index
        while (
            index < length
            and not text[index].isspace()
            and text[index] not in "()"
        ):
            index += 1
        if start == index:
            raise ValueError(f"unexpected WAST character {text[index]!r}")
        return text[start:index]

    def parse_item():
        nonlocal index, line
        skip_space()
        if index >= length:
            raise ValueError("unexpected end of WAST")
        if text[index] == '"':
            return parse_string()
        if text[index] != "(":
            return parse_atom()

        start = index
        start_line = line
        index += 1
        items = []
        while True:
            skip_space()
            if index >= length:
                raise ValueError("unterminated WAST list")
            if text[index] == ")":
                index += 1
                return _WastList(items, start, index, start_line)
            items.append(parse_item())

    forms = []
    while True:
        skip_space()
        if index >= length:
            break
        forms.append(parse_item())
    return forms


def _thread_const_value(node):
    head = _wast_head(node)
    if head not in ("i32.const", "i64.const"):
        raise ValueError(f"thread harness constant {head!r} unsupported")
    if len(node.items) != 2 or not isinstance(node.items[1], str):
        raise ValueError(f"malformed {head}")
    bits = 32 if head == "i32.const" else 64
    value = int(node.items[1], 0) & ((1 << bits) - 1)
    return {"type": head[:3], "value": str(value)}


def _thread_module_name(node):
    if _wast_head(node) != "module" or len(node.items) < 2:
        return None
    name = node.items[1]
    return name if isinstance(name, str) and name.startswith("$") else None


def _compile_thread_module(
    node,
    source,
    case_dir,
    wast2json,
    wast2json_flags,
    counter,
):
    module_id = counter[0]
    counter[0] += 1
    source_path = os.path.join(case_dir, f"thread-module-{module_id}.wast")
    json_path = os.path.join(case_dir, f"thread-module-{module_id}.json")
    with open(source_path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(source[node.start:node.end])
        handle.write("\n")

    args = [wast2json, "--enable-function-references"]
    args.extend(wast2json_flags or [])
    args.extend([source_path, "-o", json_path])
    completed = subprocess.run(
        args,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"thread module conversion failed at line {node.line}:\n"
            f"{completed.stdout}"
        )

    with open(json_path, "r", encoding="utf-8") as handle:
        document = json.load(handle)
    commands = document.get("commands", [])
    module_command = next(
        (command for command in commands if command.get("type") == "module"),
        None,
    )
    if module_command is None:
        raise RuntimeError(
            f"thread module conversion produced no module at line {node.line}"
        )
    filename = module_command.get("filename")
    if not isinstance(filename, str):
        raise RuntimeError(
            f"thread module conversion omitted filename at line {node.line}"
        )
    return os.path.abspath(os.path.join(os.path.dirname(json_path), filename))


def _convert_threads_wast(
    wast_path,
    manifest_path,
    case_dir,
    wast2json,
    wast2json_flags,
):
    with open(wast_path, "r", encoding="utf-8") as handle:
        source = handle.read()
    forms = _parse_wast_script(source)
    module_counter = [0]
    manifest_counter = [0]

    def convert_forms(nodes, path, initial_named_slots=None):
        named_slots = dict(initial_named_slots or {})
        current_slot = None
        next_slot = (
            max(named_slots.values()) + 1 if named_slots else 0
        )
        lines = ["TWCF2"]

        def emit_unsupported(line, reason):
            lines.append(
                f"unsupported\t{line}\t{safe_reason(reason)}"
            )

        def slot_for_action(items, start_index):
            nonlocal current_slot
            index = start_index
            slot = current_slot
            if (
                index < len(items)
                and isinstance(items[index], str)
                and items[index].startswith("$")
            ):
                slot = named_slots.get(items[index])
                index += 1
            return slot, index

        def encode_invoke_node(node):
            if _wast_head(node) != "invoke":
                raise ValueError("expected invoke action")
            slot, index = slot_for_action(node.items, 1)
            if slot is None:
                raise ValueError("named/default module not available")
            if index >= len(node.items):
                raise ValueError("invoke field missing")
            field = _wast_string(node.items[index])
            if field is None:
                raise ValueError("invoke field is not a string")
            index += 1
            args = []
            for value_node in node.items[index:]:
                args.append(_thread_const_value(value_node))
            encoded, reason = encode_values(args, expected=False)
            if encoded is None:
                raise ValueError(reason)
            return slot, hex_utf8(field), encoded

        for node in nodes:
            if not isinstance(node, _WastList):
                emit_unsupported(0, "top-level atom unsupported")
                continue
            command = _wast_head(node)
            line = node.line

            if command == "module":
                try:
                    compiled = _compile_thread_module(
                        node,
                        source,
                        case_dir,
                        wast2json,
                        wast2json_flags,
                        module_counter,
                    )
                except (OSError, ValueError, RuntimeError, json.JSONDecodeError) as error:
                    emit_unsupported(line, error)
                    continue
                slot = next_slot
                next_slot += 1
                current_slot = slot
                name = _thread_module_name(node)
                if name is not None:
                    named_slots[name] = slot
                lines.append(f"module\t{line}\t{slot}\t{compiled}")
                continue

            if command == "register":
                if len(node.items) < 2:
                    emit_unsupported(line, "register name missing")
                    continue
                alias = _wast_string(node.items[1])
                if alias is None:
                    emit_unsupported(line, "register name is not a string")
                    continue
                if len(node.items) >= 3:
                    target = node.items[2]
                    slot = (
                        named_slots.get(target)
                        if isinstance(target, str)
                        else None
                    )
                else:
                    slot = current_slot
                if slot is None:
                    emit_unsupported(line, "register target unavailable")
                    continue
                lines.append(
                    f"register\t{line}\t{slot}\t{hex_utf8(alias)}"
                )
                continue

            if command == "invoke":
                try:
                    slot, field, args = encode_invoke_node(node)
                except ValueError as error:
                    emit_unsupported(line, error)
                    continue
                lines.append(
                    f"action\t{line}\t{slot}\t{field}\t{args}"
                )
                continue

            if command == "assert_return":
                if len(node.items) < 2 or not isinstance(
                    node.items[1], _WastList
                ):
                    emit_unsupported(line, "assert_return action missing")
                    continue
                try:
                    slot, field, args = encode_invoke_node(node.items[1])
                    expected_nodes = node.items[2:]
                    if (
                        len(expected_nodes) == 1
                        and _wast_head(expected_nodes[0]) == "either"
                    ):
                        alternatives = []
                        for alternative in expected_nodes[0].items[1:]:
                            token, reason = encode_value(
                                _thread_const_value(alternative),
                                expected=True,
                            )
                            if token is None:
                                raise ValueError(reason)
                            alternatives.append(token)
                        if not alternatives:
                            raise ValueError("either expectation is empty")
                        lines.append(
                            f"assert_return_either\t{line}\t{slot}\t{field}"
                            f"\t{args}\t" + "|".join(alternatives)
                        )
                    else:
                        expected = [
                            _thread_const_value(item)
                            for item in expected_nodes
                        ]
                        encoded, reason = encode_values(
                            expected, expected=True
                        )
                        if encoded is None:
                            raise ValueError(reason)
                        lines.append(
                            f"assert_return\t{line}\t{slot}\t{field}"
                            f"\t{args}\t{encoded}"
                        )
                except ValueError as error:
                    emit_unsupported(line, error)
                continue

            if command == "assert_unlinkable":
                if len(node.items) < 2 or not isinstance(
                    node.items[1], _WastList
                ):
                    emit_unsupported(line, "assert_unlinkable module missing")
                    continue
                try:
                    compiled = _compile_thread_module(
                        node.items[1],
                        source,
                        case_dir,
                        wast2json,
                        wast2json_flags,
                        module_counter,
                    )
                except (OSError, ValueError, RuntimeError, json.JSONDecodeError) as error:
                    emit_unsupported(line, error)
                    continue
                lines.append(f"assert_unlinkable\t{line}\t{compiled}")
                continue

            if command == "thread":
                if len(node.items) < 2 or not isinstance(node.items[1], str):
                    emit_unsupported(line, "thread name missing")
                    continue
                thread_name = node.items[1]
                body_start = 2
                shared = []
                if (
                    body_start < len(node.items)
                    and _wast_head(node.items[body_start]) == "shared"
                ):
                    shared_node = node.items[body_start]
                    body_start += 1
                    for entry in shared_node.items[1:]:
                        if _wast_head(entry) != "module" or len(entry.items) != 2:
                            emit_unsupported(
                                line, "malformed shared module reference"
                            )
                            shared = None
                            break
                        module_name = entry.items[1]
                        if (
                            not isinstance(module_name, str)
                            or module_name not in named_slots
                        ):
                            emit_unsupported(
                                line,
                                f"shared module {module_name!r} unavailable",
                            )
                            shared = None
                            break
                        shared.append((module_name, named_slots[module_name]))
                if shared is None:
                    continue

                child_named = {}
                bindings = []
                for child_slot, (module_name, parent_slot) in enumerate(shared):
                    child_named[module_name] = child_slot
                    bindings.append(f"{child_slot}:{parent_slot}")

                child_nodes = [
                    item
                    for item in node.items[body_start:]
                    if isinstance(item, _WastList)
                ]
                child_id = manifest_counter[0]
                manifest_counter[0] += 1
                child_path = os.path.join(
                    case_dir, f"thread-{child_id}.twcf"
                )
                convert_forms(child_nodes, child_path, child_named)
                binding_text = ",".join(bindings) if bindings else "-"
                lines.append(
                    f"thread\t{line}\t{hex_utf8(thread_name)}"
                    f"\t{binding_text}\t{child_path}"
                )
                continue

            if command == "wait":
                if len(node.items) != 2 or not isinstance(node.items[1], str):
                    emit_unsupported(line, "wait thread name missing")
                    continue
                lines.append(
                    f"wait\t{line}\t{hex_utf8(node.items[1])}"
                )
                continue

            emit_unsupported(
                line, f"thread script command {command!r} unsupported"
            )

        with open(path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write("\n".join(lines))
            handle.write("\n")

    convert_forms(forms, manifest_path)


def _wast_uses_thread_commands(wast_path):
    with open(wast_path, "r", encoding="utf-8") as handle:
        forms = _parse_wast_script(handle.read())
    return any(
        _wast_head(form) in ("thread", "wait")
        for form in forms
        if isinstance(form, _WastList)
    )

def run_file(
    wast2json,
    runner,
    core_dir,
    filename,
    temp_root,
    compare_runner=None,
    wast2json_flags=None,
    wasm_tools=None,
):
    wast_path = os.path.join(core_dir, filename)
    frontend_passed = frontend_failed = 0
    case_dir = os.path.join(
        temp_root, os.path.splitext(filename)[0].replace("/", "_")
    )
    os.makedirs(case_dir, exist_ok=True)
    json_path = os.path.join(case_dir, "case.json")
    manifest_path = os.path.join(case_dir, "case.twcf")

    uses_thread_commands = False
    if "--enable-threads" in (wast2json_flags or []):
        try:
            uses_thread_commands = _wast_uses_thread_commands(wast_path)
        except (OSError, ValueError) as error:
            print(f"SPEC {filename} converter_unsupported")
            print(error)
            return 0, 0, 1, 1, None, 0, 0, 0, 0, 0

    if uses_thread_commands:
        try:
            _convert_threads_wast(
                wast_path,
                manifest_path,
                case_dir,
                wast2json,
                wast2json_flags,
            )
        except (OSError, ValueError, RuntimeError, json.JSONDecodeError) as error:
            print(f"SPEC {filename} converter_unsupported")
            print(error)
            return 0, 0, 1, 1, None, 0, 0, 0, 0, 0
    else:
        if wasm_tools is not None:
            convert_args = [wasm_tools, "json-from-wast", wast_path,
                            "--wasm-dir", case_dir, "-o", json_path]
        else:
            convert_args = [wast2json, "--enable-function-references"]
            convert_args.extend(wast2json_flags or [])
            convert_args.extend([wast_path, "-o", json_path])

        convert = subprocess.run(
            convert_args,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
        if convert.returncode != 0:
            print(f"SPEC {filename} converter_unsupported")
            print(convert.stdout)
            return 0, 0, 1, 1, None, 0, 0, 0, 0, 0

        frontend_passed, frontend_failed = convert_json(json_path, manifest_path, wasm_tools=wasm_tools)

    run_env = os.environ.copy()
    if wasm_tools is not None:
        run_env["TURBOWASM_SPEC_GC_STORE"] = "1"
    if compare_runner is not None:
        run_env["TURBOWASM_SPEC_RESULT_TRACE"] = "1"

    result = subprocess.run(
        [runner, manifest_path],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
        env=run_env,
    )
    match = SUMMARY_RE.search(result.stdout)
    if match is None:
        print(
            f"SPEC {filename} runner_summary_missing "
            f"returncode={result.returncode}"
        )
        print(result.stdout)

        trace_env = run_env.copy()
        trace_env["TURBOWASM_SPEC_TRACE"] = "1"
        traced = subprocess.run(
            [runner, manifest_path],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
            env=trace_env,
        )
        trace_lines = traced.stdout.splitlines()
        print(
            f"SPEC {filename} traced_returncode={traced.returncode} "
            f"trace_tail={min(80, len(trace_lines))}"
        )
        for trace_line in trace_lines[-80:]:
            print(trace_line)
        return 0, 1, 0, 1, None, 0, 0, 1, 0, 0

    passed, failed, unsupported, total = map(int, match.groups())
    mir_match = MIR_REPLAY_RE.search(result.stdout)
    mir_stats = (
        tuple(map(int, mir_match.groups()))
        if mir_match is not None
        else None
    )
    differential_files = 0
    differential_commands = 0
    differential_mismatches = 0

    if compare_runner is not None:
        compared = subprocess.run(
            [compare_runner, manifest_path],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
            env=run_env,
        )
        compared_summary = SUMMARY_RE.search(compared.stdout)
        primary_trace = RESULT_RE.findall(result.stdout)
        compared_trace = RESULT_RE.findall(compared.stdout)

        differential_files = 1
        differential_commands = max(
            len(primary_trace), len(compared_trace)
        )
        trace_length = differential_commands
        for index in range(trace_length):
            left = (
                primary_trace[index]
                if index < len(primary_trace)
                else None
            )
            right = (
                compared_trace[index]
                if index < len(compared_trace)
                else None
            )
            if left != right:
                differential_mismatches += 1
                if differential_mismatches == 1:
                    print(
                        f"DIFFERENTIAL_FAIL {filename} index={index} "
                        f"interpreter={left!r} mir={right!r}"
                    )

        if compared_summary is None:
            differential_mismatches += 1
            print(
                f"DIFFERENTIAL_FAIL {filename} "
                "MIR runner summary missing"
            )
        else:
            compared_counts = tuple(
                map(int, compared_summary.groups())
            )
            primary_counts = (
                passed, failed, unsupported, total
            )
            if compared_counts != primary_counts:
                differential_mismatches += 1
                print(
                    f"DIFFERENTIAL_FAIL {filename} "
                    f"interpreter_counts={primary_counts} "
                    f"mir_counts={compared_counts}"
                )

        if compared.returncode != 0:
            differential_mismatches += 1
            print(
                f"DIFFERENTIAL_FAIL {filename} "
                f"MIR returncode={compared.returncode}"
            )

        compared_mir = MIR_REPLAY_RE.search(compared.stdout)
        if compared_mir is not None:
            mir_stats = tuple(map(int, compared_mir.groups()))
            compiled, interpret_only, cold, calls = mir_stats
            print(f"SPEC_MIR {filename} compiled={compiled} interpret_only={interpret_only} cold={cold} calls={calls}")
            for admission in compared.stdout.splitlines():
                if admission.startswith("MIR_ADMISSION "):
                    print(f"SPEC_MIR {filename} {admission}")

        if differential_mismatches:
            failed += differential_mismatches
            total += differential_mismatches

    print(
        f"SPEC {filename} pass={passed} fail={failed} "
        f"unsupported={unsupported} total={total}"
    )

    if unsupported:
        first_unsupported = next(
            (
                line
                for line in result.stdout.splitlines()
                if line.startswith("FIRST_UNSUPPORTED ")
            ),
            None,
        )
        if first_unsupported is not None:
            print(f"SPEC_UNSUPPORTED_DETAIL {filename} {first_unsupported}")

        first_runtime_unsupported = next(
            (
                line
                for line in result.stdout.splitlines()
                if line.startswith("FIRST_RUNTIME_UNSUPPORTED ")
            ),
            None,
        )
        if first_runtime_unsupported is not None:
            print(
                f"SPEC_RUNTIME_UNSUPPORTED_DETAIL {filename} "
                f"{first_runtime_unsupported}"
            )

    if failed or result.returncode != 0:
        print(result.stdout)

    if result.returncode != 0 and failed == 0:
        failed = 1
        total += 1

    return (
        passed,
        failed,
        unsupported,
        total,
        mir_stats,
        differential_files,
        differential_commands,
        differential_mismatches,
        frontend_passed,
        frontend_failed,
    )


def load_suite(path):
    result = []
    seen = set()
    with open(path, "r", encoding="utf-8") as handle:
        for raw in handle:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            if line in seen:
                raise ValueError(f"duplicate conformance suite entry: {line}")
            seen.add(line)
            result.append(line)
    return result


def main():
    parser = argparse.ArgumentParser()
    converter = parser.add_mutually_exclusive_group(required=True)
    converter.add_argument("--wast2json")
    converter.add_argument("--wasm-tools")
    parser.add_argument("--runner", required=True)
    parser.add_argument("--compare-runner")
    parser.add_argument(
        "--wast2json-flag",
        action="append",
        default=[],
        help="Additional feature flag passed to wast2json; may repeat",
    )
    parser.add_argument(
        "--minimum-passes",
        type=int,
        default=1,
        help="Minimum aggregate upstream passes required for success",
    )
    parser.add_argument("--core-dir", required=True)
    selection = parser.add_mutually_exclusive_group(required=True)
    selection.add_argument("--suite")
    selection.add_argument("--all-core", action="store_true",
                           help="Run every .wast recursively; unsupported defaults to zero")
    parser.add_argument("--maximum-unsupported", type=int,
                        help="Fail when unsupported commands exceed this limit")
    args = parser.parse_args()

    if args.minimum_passes < 0:
        parser.error("minimum passes must be non-negative")
    if args.maximum_unsupported is not None and args.maximum_unsupported < 0:
        parser.error("maximum unsupported must be non-negative")
    if args.wasm_tools and args.wast2json_flag:
        parser.error("--wast2json-flag requires --wast2json")
    if args.all_core:
        root = Path(args.core_dir)
        suite = sorted(path.relative_to(root).as_posix()
                       for path in root.rglob("*.wast"))
        if args.maximum_unsupported is None:
            args.maximum_unsupported = 0
    else:
        suite = load_suite(args.suite)
    if not suite:
        raise SystemExit("empty conformance suite")

    frontend_passed = frontend_failed = 0
    passed = 0
    failed = 0
    unsupported = 0
    total = 0
    mir_compiled = 0
    mir_interpret_only = 0
    mir_cold = 0
    mir_calls = 0
    mir_files = 0
    differential_files = 0
    differential_commands = 0
    differential_mismatches = 0

    with tempfile.TemporaryDirectory(prefix="turbowasm-spec-") as temp_root:
        for filename in suite:
            (
                p, f, u, t, mir,
                diff_files, diff_commands, diff_mismatches, text_passed, text_failed,
            ) = run_file(
                args.wast2json,
                args.runner,
                args.core_dir,
                filename,
                temp_root,
                compare_runner=args.compare_runner,
                wast2json_flags=args.wast2json_flag,
                wasm_tools=args.wasm_tools,
            )
            frontend_passed += text_passed
            frontend_failed += text_failed
            passed += p
            failed += f
            unsupported += u
            total += t
            if mir is not None:
                compiled, interpret_only, cold, calls = mir
                mir_compiled += compiled
                mir_interpret_only += interpret_only
                mir_cold += cold
                mir_calls += calls
                mir_files += 1
            differential_files += diff_files
            differential_commands += diff_commands
            differential_mismatches += diff_mismatches

    print(
        f"CORE_CONFORMANCE pass={passed} fail={failed} "
        f"unsupported={unsupported} total={total} files={len(suite)}"
    )

    if args.wasm_tools:
        print(f"TEXT_FRONTEND_CONFORMANCE pass={frontend_passed} fail={frontend_failed} "
              "provider=wasm-tools (excluded from TurboWasm binary counts)")

    if mir_files:
        print(
            f"MIR_REPLAY compiled={mir_compiled} "
            f"interpret_only={mir_interpret_only} "
            f"cold={mir_cold} calls={mir_calls} files={mir_files}"
        )

    if args.compare_runner is not None:
        print(
            f"DIFFERENTIAL_REPLAY files={differential_files} "
            f"commands={differential_commands} "
            f"mismatches={differential_mismatches}"
        )

    if (args.maximum_unsupported is not None and
            unsupported > args.maximum_unsupported):
        print(f"unsupported commands {unsupported} exceed maximum "
              f"{args.maximum_unsupported}", file=sys.stderr)
        return 1
    if passed < args.minimum_passes:
        print(
            f"upstream passes {passed} below minimum {args.minimum_passes}",
            file=sys.stderr,
        )
        return 1
    return 1 if failed or frontend_failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
