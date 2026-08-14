"""Functional tests for engine phase execution.

Tests the phase lifecycle: check(), fetch(), and install() phase execution
with proper trace logging and dependency handling.
"""

import os
import subprocess
from pathlib import Path
import unittest

from . import test_config
from .env import EnvyTestCase
from .trace_parser import PkgPhase, TraceParser


class TestEnginePhases(EnvyTestCase):
    """Tests for phase execution lifecycle."""

    def setUp(self):
        super().setUp()
        self.specs_dir = self.make_temp_dir("specs_dir")

    def write_spec(self, name: str, content: str) -> Path:
        """Write spec to temp directory."""
        path = self.specs_dir / f"{name}.lua"
        path.write_text(content, encoding="utf-8")
        return path

    def get_file_hash(self, filepath):
        """Get SHA256 hash of file using envy hash command."""
        result = test_config.run(
            [str(self.envy), "hash", str(filepath)],
            capture_output=True,
            text=True,
            check=True,
        )
        return result.stdout.strip().split("  ", 1)[0]

    def test_phase_execution_check_false(self):
        """Engine executes check() and install() phases with structured trace."""
        # Minimal spec with no dependencies
        spec = """IDENTITY = "local.simple@v1"
DEPENDENCIES = {}

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic package - no cache interaction
    end,
  },
}

"""
        spec_path = self.write_spec("simple", spec)
        trace_file = self.cache_root / "trace.jsonl"
        manifest = test_config.write_spec_manifest(
            self.specs_dir, [("local.simple@v1", spec_path)]
        )

        result = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                f"--trace=file:{trace_file}",
                "install",
                "--manifest",
                str(manifest),
            ],
            capture_output=True,
            text=True,
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        # Verify phase execution via structured trace
        parser = TraceParser(trace_file)
        phase_sequence = parser.get_phase_sequence("local.simple@v1")
        self.assertIn(PkgPhase.PKG_CHECK, phase_sequence)
        self.assertIn(PkgPhase.PKG_INSTALL, phase_sequence)

    def test_fetch_function_basic(self):
        """Engine executes fetch() phase for specs with fetch function."""
        # Spec with basic fetch function
        spec = """IDENTITY = "local.fetcher@v1"
DEPENDENCIES = {}

function FETCH(tmp_dir, options)
  -- Simulates fetching by writing a test file
end

function INSTALL(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  -- Install from fetched materials
end
"""
        spec_path = self.write_spec("fetch_function_basic", spec)
        trace_file = self.cache_root / "trace.jsonl"
        manifest = test_config.write_spec_manifest(
            self.specs_dir, [("local.fetcher@v1", spec_path)]
        )

        result = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                f"--trace=file:{trace_file}",
                "install",
                "--manifest",
                str(manifest),
            ],
            capture_output=True,
            text=True,
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        # Verify fetch phase execution via structured trace
        parser = TraceParser(trace_file)
        phase_sequence = parser.get_phase_sequence("local.fetcher@v1")
        self.assertIn(PkgPhase.PKG_FETCH, phase_sequence)

        # The spec is the only package the engine resolved
        self.assertEqual(parser.registered_specs(), {"local.fetcher@v1"})

    def test_fetch_function_with_dependency(self):
        """Engine executes fetch() with dependencies available."""
        # Minimal tool spec used as dependency
        tool_spec = """IDENTITY = "local.tool@v1"
DEPENDENCIES = {}

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Install tool
    end,
  },
}

"""
        self.write_spec("tool", tool_spec)

        # Spec with fetch function that depends on another recipe
        fetcher_spec = """IDENTITY = "local.fetcher_with_dep@v1"
DEPENDENCIES = {
  { spec = "local.tool@v1", source = "tool.lua" }
}

function FETCH(tmp_dir, options)
  -- Fetch phase uses a dependency
end

function INSTALL(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  -- Install from fetched materials
end
"""
        spec_path = self.write_spec("fetch_function_with_dep", fetcher_spec)
        trace_file = self.cache_root / "trace.jsonl"
        manifest = test_config.write_spec_manifest(
            self.specs_dir, [("local.fetcher_with_dep@v1", spec_path)]
        )

        result = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                f"--trace=file:{trace_file}",
                "install",
                "--manifest",
                str(manifest),
            ],
            capture_output=True,
            text=True,
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        # Verify both specs executed
        parser = TraceParser(trace_file)
        self.assertEqual(
            parser.registered_specs(),
            {"local.fetcher_with_dep@v1", "local.tool@v1"},
        )

        # Verify dependency relationship via structured trace
        deps = parser.get_dependency_added_events("local.fetcher_with_dep@v1")
        dep_names = [d.raw.get("dependency") for d in deps]
        self.assertIn("local.tool@v1", dep_names)


if __name__ == "__main__":
    unittest.main()
