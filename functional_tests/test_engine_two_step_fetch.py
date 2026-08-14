"""Functional tests for two-step fetch pattern (fetch → commit).

Tests the security gating pattern where envy.fetch() downloads to tmp
and envy.commit_fetch() moves files to fetch_dir with SHA256 verification.
"""

import os
from pathlib import Path
import unittest

from . import test_config
from .env import EnvyTestCase

# Simple test file content for fetch tests
SIMPLE_LUA_CONTENT = """-- Simple test script for lua_util tests
expected_value = 42
"""


class TestEngineTwoStepFetch(EnvyTestCase):
    """Tests for two-step fetch pattern (ungated fetch → gated commit)."""

    def setUp(self):
        super().setUp()
        self.test_files_dir = self.make_temp_dir("test_files_dir")
        self.trace_flag = ["--trace"] if os.environ.get("ENVY_TEST_TRACE") else []

        # Write test file to temp directory
        self.simple_lua = self.test_files_dir / "simple.lua"
        self.simple_lua.write_text(SIMPLE_LUA_CONTENT, encoding="utf-8")

    @staticmethod
    def lua_path(path: Path) -> str:
        """Convert path to Lua-safe string (forward slashes work on all platforms)."""
        return path.as_posix()

    def get_file_hash(self, filepath):
        """Get SHA256 hash of file using envy hash command."""
        result = test_config.run(
            [str(self.envy), "hash", str(filepath)],
            capture_output=True,
            text=True,
            check=True,
        )
        return result.stdout.strip().split("  ", 1)[0]

    def test_two_step_with_sha256(self):
        """Fetch → inspect → commit with SHA256 verification."""
        # Get actual SHA256 of test file
        test_file = self.simple_lua
        expected_hash = self.get_file_hash(test_file)

        spec_content = f"""IDENTITY = "local.two_step_sha256@v1"

function FETCH(tmp_dir, options)
  -- Step 1: Download to tmp (ungated)
  local file = envy.fetch("{self.lua_path(test_file)}", {{dest = tmp_dir}})

  -- At this point, file is in tmp_dir (run_dir), not fetch_dir
  -- User could inspect it, read manifest, fetch more files, etc.

  -- Step 2: Commit with SHA256 (gated)
  envy.commit_fetch({{
    filename = file,
    sha256 = "{expected_hash}"
  }})

  -- Now file is in fetch_dir with verified SHA256
end
"""
        spec_path = self.cache_root / "two_step_sha256.lua"
        spec_path.write_text(spec_content, encoding="utf-8")

        manifest = test_config.write_spec_manifest(
            self.cache_root, [("local.two_step_sha256@v1", spec_path)]
        )
        result = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                *self.trace_flag,
                "install",
                "--manifest",
                str(manifest),
            ],
            capture_output=True,
            text=True,
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertIn("local.two_step_sha256@v1", result.stderr)

    def test_fetch_then_inspect_then_commit(self):
        """Fetch manifest → read contents → fetch listed files → commit all."""
        # Create a fake manifest listing other files
        manifest_dir = self.cache_root / "manifest_files"
        manifest_dir.mkdir()

        # Create manifest file
        manifest_file = manifest_dir / "manifest.txt"
        manifest_file.write_text("file1.txt\nfile2.txt\n", encoding="utf-8")

        # Create listed files
        (manifest_dir / "file1.txt").write_text("content1", encoding="utf-8")
        (manifest_dir / "file2.txt").write_text("content2", encoding="utf-8")

        spec_content = f"""IDENTITY = "local.manifest_workflow@v1"

function FETCH(tmp_dir, options)
  -- Step 1: Fetch manifest
  local manifest_file = envy.fetch("{self.lua_path(manifest_file)}", {{dest = tmp_dir}})

  -- Step 2: Read manifest from tmp (this is the point of two-step pattern!)
  local manifest_path = tmp_dir .. "/" .. manifest_file
  local f = io.open(manifest_path, "r")
  if not f then error("Cannot read manifest from tmp") end
  local manifest_content = f:read("*all")
  f:close()

  -- Verify we can inspect files before commit
  if not manifest_content:match("file1.txt") then
    error("Manifest should list file1.txt")
  end

  -- Step 3: Fetch files listed in manifest
  local files = envy.fetch({{
    "{self.lua_path(manifest_dir / "file1.txt")}",
    "{self.lua_path(manifest_dir / "file2.txt")}"
  }}, {{dest = tmp_dir}})

  -- Step 4: Commit all files (manifest + listed files)
  envy.commit_fetch(manifest_file)
  envy.commit_fetch(files)
end
"""
        spec_path = self.cache_root / "manifest_workflow.lua"
        spec_path.write_text(spec_content, encoding="utf-8")

        manifest = test_config.write_spec_manifest(
            self.cache_root, [("local.manifest_workflow@v1", spec_path)]
        )
        result = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                *self.trace_flag,
                "install",
                "--manifest",
                str(manifest),
            ],
            capture_output=True,
            text=True,
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertIn("local.manifest_workflow@v1", result.stderr)


if __name__ == "__main__":
    unittest.main()
