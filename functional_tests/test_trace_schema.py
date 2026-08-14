"""Trace schema contract tests.

Locks the trace registry: the binary's `trace-schema` dump must match the
parser's EVENT_REGISTRY exactly, every emitted event must be registered (the
strict parser raises otherwise), seq must be strictly increasing with the
trace_start header first, and the human (stderr) and JSONL (file) sinks must
agree on per-event-type counts.
"""

import json
import unittest
from collections import Counter
from pathlib import Path

from . import test_config
from .env import EnvyTestCase
from .trace_parser import EVENT_REGISTRY, SCHEMA_VERSION, TraceParser

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


class TestTraceSchema(EnvyTestCase):
    def setUp(self):
        super().setUp()
        self.test_dir = self.make_temp_dir("test_dir")

        self.spec_path = self.test_dir / "simple.lua"
        self.spec_path.write_text(SIMPLE_SPEC, encoding="utf-8")
        self.manifest = test_config.write_spec_manifest(
            self.test_dir, [("local.simple@v1", self.spec_path)]
        )

    def _run_install(self, *trace_args: str):
        result = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                *trace_args,
                "install",
                "--manifest",
                str(self.manifest),
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        return result

    def test_binary_registry_matches_parser_registry(self):
        """trace-schema dump == EVENT_REGISTRY, field for field."""
        result = test_config.run(
            [str(self.envy), "trace-schema"], capture_output=True, text=True
        )
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        dump = json.loads(result.stdout)
        self.assertEqual(dump["schema"], SCHEMA_VERSION)
        self.assertEqual(
            dump["events"],
            EVENT_REGISTRY,
            "Binary trace-schema dump and trace_parser.EVENT_REGISTRY diverged; "
            "update functional_tests/trace_parser.py to match src/trace_events.def",
        )

    def test_emitted_events_parse_strictly(self):
        """A real run emits only registered events with valid envelopes."""
        trace_file = self.cache_root / "trace.jsonl"
        self._run_install(f"--trace=file:{trace_file}")

        parser = TraceParser(trace_file)
        events = parser.parse()  # raises on unknown events / bad envelope / header
        self.assertGreater(len(events), 1)

        # Header is the causally-first record and carries the schema version.
        self.assertEqual(events[0].event, "trace_start")
        self.assertEqual(events[0].raw["schema"], SCHEMA_VERSION)

        # seq strictly increasing and unique (parser sorts by seq; file order
        # matches because seq is assigned under the queue mutex).
        seqs = [e.seq for e in events]
        self.assertEqual(seqs, sorted(set(seqs)), "seq values must be unique/ordered")

    def test_stderr_and_file_sinks_agree(self):
        """Human and JSONL sinks receive the same events (compare type counts)."""
        trace_file = self.cache_root / "trace.jsonl"
        result = self._run_install(f"--trace=stderr,file:{trace_file}")

        parser = TraceParser(trace_file)
        file_counts = Counter(e.event for e in parser.parse())

        stderr_counts: Counter = Counter()
        for line in result.stderr.splitlines():
            # Human trace lines start with the event name; --trace decorates
            # lines with a "[ts] [TRC] " prefix, so strip it when present.
            text = line.split("] ", 2)[-1].strip()
            name = text.split(" ", 1)[0]
            if name in EVENT_REGISTRY:
                stderr_counts[name] += 1

        self.assertEqual(
            stderr_counts,
            file_counts,
            "stderr (human) and file (JSONL) sinks diverged",
        )


if __name__ == "__main__":
    unittest.main()
