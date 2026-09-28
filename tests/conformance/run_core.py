#!/usr/bin/env python3
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile

SUMMARY_RE = re.compile(
    r"CONFORMANCE pass=(\d+) fail=(\d+) unsupported=(\d+) total=(\d+)"
)

TRAP_MAP = [
    ("unreachable", 1),
    ("call stack exhausted", 2),
    ("integer divide by zero", 3),
    ("integer overflow", 4),
    ("invalid conversion to integer", 9),
    ("out of bounds memory access", 5),
    ("out of bounds table access", 6),
    ("undefined element", 6),
    ("uninitialized element", 7),
    ("indirect call type mismatch", 8),
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
        if not raw.isdigit():
            return None, f"{value_type} non-decimal encoding unsupported"
        return f"{value_type}:{raw}", None

    if value_type == "funcref" and raw == "null":
        return "funcref:null", None

    if value_type == "v128":
        return None, "v128 manifest comparison not yet qualified"

    if value_type in ("externref", "exnref", "funcref"):
        return None, f"{value_type} non-null/reference payload unsupported"

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


def convert_json(json_path, manifest_path):
    with open(json_path, "r", encoding="utf-8") as handle:
        document = json.load(handle)

    json_dir = os.path.dirname(os.path.abspath(json_path))
    named_slots = {}
    current_slot = None
    next_slot = 0
    lines = ["TWCF1"]

    def emit_unsupported(line, reason):
        lines.append(
            f"unsupported\t{line}\t{safe_reason(reason)}"
        )

    def encode_invoke(command, action, command_type, expected=None, trap=None):
        line = command.get("line", 0)
        if action.get("type") != "invoke":
            emit_unsupported(line, "get action not yet qualified")
            return

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
                f"action\t{line}\t{slot_text}\t{field_hex}\t{args}"
            )
            return

        if command_type == "assert_return":
            encoded_expected, reason = encode_values(
                expected or [], expected=True
            )
            if encoded_expected is None:
                emit_unsupported(line, reason)
                return
            lines.append(
                f"assert_return\t{line}\t{slot_text}\t{field_hex}"
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

        raise AssertionError(command_type)

    for command in document.get("commands", []):
        command_type = command.get("type")
        line = command.get("line", 0)

        if command_type == "module":
            filename = command.get("filename")
            if not isinstance(filename, str):
                emit_unsupported(line, "module filename missing")
                continue

            path = os.path.abspath(os.path.join(json_dir, filename))
            if command.get("definition") in (True, "true"):
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

        if command_type in (
            "assert_invalid",
            "assert_malformed",
            "assert_unlinkable",
            "assert_uninstantiable",
        ):
            if command.get("module_type") != "binary":
                emit_unsupported(
                    line,
                    f"{command_type} text module not run through TurboWasm binary reader",
                )
                continue
            filename = command.get("filename")
            if not isinstance(filename, str):
                emit_unsupported(line, f"{command_type} filename missing")
                continue
            path = os.path.abspath(os.path.join(json_dir, filename))
            lines.append(f"{command_type}\t{line}\t{path}")
            continue

        emit_unsupported(line, f"command {command_type!r} unsupported")

    with open(manifest_path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("\n".join(lines))
        handle.write("\n")


def run_file(wast2json, runner, core_dir, filename, temp_root):
    wast_path = os.path.join(core_dir, filename)
    case_dir = os.path.join(
        temp_root, os.path.splitext(filename)[0].replace("/", "_")
    )
    os.makedirs(case_dir, exist_ok=True)
    json_path = os.path.join(case_dir, "case.json")
    manifest_path = os.path.join(case_dir, "case.twcf")

    convert = subprocess.run(
        [
            wast2json,
            "--enable-function-references",
            wast_path,
            "-o",
            json_path,
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    if convert.returncode != 0:
        print(f"SPEC {filename} converter_unsupported")
        print(convert.stdout)
        return 0, 0, 1, 1

    convert_json(json_path, manifest_path)

    result = subprocess.run(
        [runner, manifest_path],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    match = SUMMARY_RE.search(result.stdout)
    if match is None:
        print(
            f"SPEC {filename} runner_summary_missing "
            f"returncode={result.returncode}"
        )
        print(result.stdout)

        trace_env = os.environ.copy()
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
        return 0, 1, 0, 1

    passed, failed, unsupported, total = map(int, match.groups())
    print(
        f"SPEC {filename} pass={passed} fail={failed} "
        f"unsupported={unsupported} total={total}"
    )
    if failed or result.returncode != 0:
        print(result.stdout)

    if result.returncode != 0 and failed == 0:
        failed = 1
        total += 1

    return passed, failed, unsupported, total


def load_suite(path):
    result = []
    with open(path, "r", encoding="utf-8") as handle:
        for raw in handle:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            result.append(line)
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--wast2json", required=True)
    parser.add_argument("--runner", required=True)
    parser.add_argument("--core-dir", required=True)
    parser.add_argument("--suite", required=True)
    args = parser.parse_args()

    suite = load_suite(args.suite)
    if not suite:
        raise SystemExit("empty conformance suite")

    passed = 0
    failed = 0
    unsupported = 0
    total = 0

    with tempfile.TemporaryDirectory(prefix="turbowasm-spec-") as temp_root:
        for filename in suite:
            p, f, u, t = run_file(
                args.wast2json,
                args.runner,
                args.core_dir,
                filename,
                temp_root,
            )
            passed += p
            failed += f
            unsupported += u
            total += t

    print(
        f"CORE_CONFORMANCE pass={passed} fail={failed} "
        f"unsupported={unsupported} total={total} files={len(suite)}"
    )

    if passed == 0:
        print("no upstream assertions passed", file=sys.stderr)
        return 1
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
