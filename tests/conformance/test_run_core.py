import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
from subprocess import CompletedProcess

import run_core


class ConformanceGateTests(unittest.TestCase):
    def invoke(self, root, outcome, *extra):
        argv = ["run_core.py", "--wasm-tools", "converter",
                "--runner", "runner", "--core-dir", str(root), *extra]
        with patch.object(sys, "argv", argv), \
                patch.object(run_core, "run_file", return_value=outcome if len(outcome) == 10 else outcome + (0, 0)) as run, \
                contextlib.redirect_stdout(io.StringIO()), \
                contextlib.redirect_stderr(io.StringIO()):
            status = run_core.main()
        return status, run

    def test_full_suite_discovers_nested_files_and_rejects_unsupported(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "gc").mkdir()
            (root / "gc" / "array.wast").touch()
            (root / "i32.wast").touch()
            outcome = (2, 0, 1, 3, None, 0, 0, 0)
            status, run = self.invoke(root, outcome, "--all-core")
            self.assertEqual(status, 1)
            self.assertEqual([call.args[3] for call in run.call_args_list],
                             ["gc/array.wast", "i32.wast"])
            self.assertEqual(run.call_args.kwargs["wasm_tools"], "converter")

    def test_full_suite_passes_only_with_successful_commands(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "i32.wast").touch()
            for outcome, expected in [
                ((2, 0, 0, 2, None, 0, 0, 0), 0),
                ((2, 1, 0, 3, None, 0, 0, 0), 1),
                ((0, 0, 0, 0, None, 0, 0, 0), 1),
            ]:
                with self.subTest(outcome=outcome):
                    self.assertEqual(self.invoke(root, outcome, "--all-core")[0], expected)

    def test_selected_suite_can_request_strict_unsupported_budget(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            suite = root / "suite.txt"
            suite.write_text("i32.wast\n", encoding="utf-8")
            outcome = (1, 0, 1, 2, None, 0, 0, 0)
            status, _ = self.invoke(root, outcome, "--suite", str(suite),
                                    "--maximum-unsupported", "0")
            self.assertEqual(status, 1)

    def test_null_reference_trap_mapping(self):
        self.assertEqual(run_core.trap_kind("null reference"), 10)
        self.assertEqual(run_core.trap_kind("null function reference"), 10)

    def test_signed_integer_encodings_preserve_bits(self):
        self.assertEqual(run_core.encode_value({"type": "i32", "value": "-1"}),
                         ("i32:4294967295", None))
        self.assertEqual(run_core.encode_value({"type": "i64", "value": "-1"}),
                         ("i64:18446744073709551615", None))
        self.assertIsNone(run_core.encode_value(
            {"type": "i32", "value": "4294967296"})[0])

    def test_wasm_tools_either_preserves_complete_vector_alternatives(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "suite.json"
            manifest = Path(directory) / "suite.twcf"
            choices = [{"type": "v128", "lane_type": "i32", "value": values}
                       for values in [["1", "2", "3", "4"], ["5", "6", "7", "8"]]]
            source.write_text(json.dumps({"commands": [
                {"type": "module", "filename": "test.wasm"},
                {"type": "assert_return", "line": 7,
                 "action": {"type": "invoke", "field": "f"},
                 "expected": [{"type": "either", "values": choices}]},
            ]}), encoding="utf-8")
            self.assertEqual(run_core.convert_json(source, manifest), (0, 0))
            self.assertIn("assert_return_either\t7\t0\t66\t-\tv128:i32:1;2;3;4|v128:i32:5;6;7;8",
                          manifest.read_text(encoding="utf-8"))

    def test_text_frontend_results_are_separate_and_errors_fail_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "suite.json"
            manifest = Path(directory) / "suite.twcf"
            source.write_text(json.dumps({"commands": [
                {"type": "assert_malformed", "line": 4,
                 "module_type": "text", "filename": "bad.wat"},
            ]}), encoding="utf-8")
            for code, stderr, expected in [(1, "error: bad token", (1, 0)),
                                            (0, "", (0, 1)),
                                            (2, "tool failure", (0, 1))]:
                with self.subTest(code=code), patch.object(run_core.subprocess, "run",
                        return_value=CompletedProcess([], code, "", stderr)), \
                        contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(run_core.convert_json(source, manifest, "converter"), expected)
                    self.assertEqual(manifest.read_text(encoding="utf-8").strip(), "TWCF1")
            (Path(directory) / "module.wast").touch()
            self.assertEqual(self.invoke(Path(directory),
                (1, 0, 0, 1, None, 0, 0, 0, 0, 1), "--all-core")[0], 1)


if __name__ == "__main__":
    unittest.main()
