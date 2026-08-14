"""Smoke tests for structured trace output.

Tests the trace infrastructure works correctly with different output modes:
- Human-readable stderr output
- Structured JSONL file output
- Multiple outputs simultaneously
"""

import json
import subprocess
import sys
import unittest
from pathlib import Path

from . import test_config
from .env import EnvyTestCase
from .trace_parser import TraceParser

SIMPLE_SPEC = """IDENTITY = "local.simple@v1"
DEPENDENCIES = {}

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
    end,
  },
}
"""


class TestStructuredTrace(EnvyTestCase):
    """Smoke tests for structured logging infrastructure."""

    def setUp(self):
        super().setUp()
        self.test_dir = self.make_temp_dir("test_dir")

        # Write spec to temp directory
        self.spec_path = self.test_dir / "simple.lua"
        self.spec_path.write_text(SIMPLE_SPEC, encoding="utf-8")
        self.manifest = test_config.write_spec_manifest(
            self.test_dir, [("local.simple@v1", self.spec_path)]
        )

    def test_trace_stderr_human_readable(self):
        """Verify --trace=stderr produces human-readable output."""
        result = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                "--trace=stderr",
                "install",
                "--manifest",
                str(self.manifest),
            ],
            capture_output=True,
            text=True,
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        # Verify human-readable trace output in stderr
        # Should NOT be JSON - should be human-readable text
        stderr_lines = result.stderr.strip().split("\n")
        self.assertGreater(len(stderr_lines), 0, "Expected trace output in stderr")

        # Check that it's NOT JSON (human-readable format)
        # Human-readable lines typically start with timestamps or log levels
        # JSON lines would start with '{'
        non_json_lines = [
            line for line in stderr_lines if not line.strip().startswith("{")
        ]
        self.assertGreater(
            len(non_json_lines), 0, "Expected human-readable (non-JSON) output"
        )

    def test_trace_file_jsonl_output(self):
        """Verify --trace=file:<path> produces valid JSONL."""
        trace_file = self.cache_root / "trace.jsonl"

        result = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                f"--trace=file:{trace_file}",
                "install",
                "--manifest",
                str(self.manifest),
            ],
            capture_output=True,
            text=True,
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertTrue(trace_file.exists(), "Trace file should be created")

        # Verify JSONL format
        parser = TraceParser(trace_file)
        events = parser.parse()

        self.assertGreater(len(events), 0, "Expected trace events in file")

        # Verify each line is valid JSON
        with open(trace_file) as f:
            for line_num, line in enumerate(f, 1):
                line = line.strip()
                if not line:
                    continue
                try:
                    event = json.loads(line)
                    # Verify required fields
                    self.assertIn("ts", event, f"Line {line_num}: Missing 'ts' field")
                    self.assertIn(
                        "event", event, f"Line {line_num}: Missing 'event' field"
                    )
                except json.JSONDecodeError as e:
                    self.fail(f"Line {line_num} is not valid JSON: {e}\nLine: {line}")

    @unittest.skipIf(
        sys.platform == "win32",
        "Non-ASCII trace path is passed via argv, which envy receives ANSI-mangled on "
        "Windows (no wmain); Windows Unicode argv is tracked separately",
    )
    def test_trace_file_non_ascii_path(self):
        """A trace-file path with non-ASCII characters must open and write correctly.

        Exercises the Unicode-safe file open (util_open_file / _wfopen on Windows)
        rather than a narrow fopen that would mangle the path.
        """
        trace_dir = self.cache_root / "trace-café-你好"
        trace_dir.mkdir(parents=True)
        trace_file = trace_dir / "trace-日本語.jsonl"

        result = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                f"--trace=file:{trace_file}",
                "install",
                "--manifest",
                str(self.manifest),
            ],
            capture_output=True,
            text=True,
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertTrue(
            trace_file.exists(), "Trace file at non-ASCII path should be created"
        )

        parser = TraceParser(trace_file)
        self.assertGreater(len(parser.parse()), 0, "Expected trace events in file")

    def test_trace_multiple_outputs_simultaneously(self):
        """Verify --trace=stderr,file:<path> works simultaneously."""
        trace_file = self.cache_root / "trace.jsonl"

        result = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                f"--trace=stderr,file:{trace_file}",
                "install",
                "--manifest",
                str(self.manifest),
            ],
            capture_output=True,
            text=True,
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        # Verify stderr has human-readable output
        stderr_lines = result.stderr.strip().split("\n")
        self.assertGreater(len(stderr_lines), 0, "Expected trace output in stderr")

        # Verify file has JSONL output
        self.assertTrue(trace_file.exists(), "Trace file should be created")
        parser = TraceParser(trace_file)
        events = parser.parse()
        self.assertGreater(len(events), 0, "Expected trace events in file")

    def test_trace_file_contains_expected_events(self):
        """Verify trace file contains expected event types."""
        trace_file = self.cache_root / "trace.jsonl"

        result = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                f"--trace=file:{trace_file}",
                "install",
                "--manifest",
                str(self.manifest),
            ],
            capture_output=True,
            text=True,
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        parser = TraceParser(trace_file)
        events = parser.parse()

        # Verify we have expected event types
        event_types = set(e.event for e in events)

        # Should have at least these core event types
        expected_types = {
            "spec_registered",
            "phase_start",
            "phase_complete",
        }

        for expected_type in expected_types:
            self.assertIn(
                expected_type, event_types, f"Expected '{expected_type}' event in trace"
            )

    def test_trace_disabled_by_default(self):
        """Verify trace is not output when --trace flag not provided."""
        trace_file = self.cache_root / "trace.jsonl"

        # Run WITHOUT --trace flag
        result = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                "install",
                "--manifest",
                str(self.manifest),
            ],
            capture_output=True,
            text=True,
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        # Trace file should NOT exist
        self.assertFalse(
            trace_file.exists(), "Trace file should not be created without --trace flag"
        )

        # Stderr should not contain trace events (may have debug/info/warn/error, but not structured trace)
        # This is harder to verify precisely, but we can check there's no JSON-like output
        if result.stderr:
            stderr_lines = [
                line.strip() for line in result.stderr.split("\n") if line.strip()
            ]
            json_lines = [line for line in stderr_lines if line.startswith("{")]
            self.assertEqual(
                len(json_lines),
                0,
                "Should not have JSON trace output without --trace flag",
            )


if __name__ == "__main__":
    unittest.main()
