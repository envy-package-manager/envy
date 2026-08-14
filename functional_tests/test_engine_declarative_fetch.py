"""Functional tests for engine declarative fetch.

Tests declarative fetch syntax: FETCH = "url", FETCH = {url, sha256},
FETCH = [{...}, ...], and basic error handling (collision, bad SHA256).
"""

import os
from pathlib import Path
import unittest

from . import test_config
from .env import EnvyTestCase

# Inline test files for declarative fetch tests
TEST_FILES = {
    "simple.lua": "-- Simple test script for lua_util tests\nexpected_value = 42\n",
    "print_single.lua": 'print("hello")\n',
    "print_multiple.lua": 'print("a", "b", "c")\n',
}


class TestEngineDeclarativeFetch(EnvyTestCase):
    """Tests for declarative fetch phase (package fetching)."""

    # Some cases clone real github repos; relax the watchdog for network latency.
    envy_watchdog_timeout = 60

    def setUp(self):
        super().setUp()
        self.test_files_dir = self.make_temp_dir("test_files_dir")
        # Enable trace for all tests if ENVY_TEST_TRACE is set
        self.trace_flag = ["--trace"] if os.environ.get("ENVY_TEST_TRACE") else []

        # Write inline test files to temp directory
        for name, content in TEST_FILES.items():
            (self.test_files_dir / name).write_text(content, encoding="utf-8")

    def lua_path(self, filename: str) -> str:
        """Get Lua-safe path to test file."""
        return (self.test_files_dir / filename).as_posix()

    def get_file_hash(self, filepath):
        """Get SHA256 hash of file using envy hash command."""
        result = test_config.run(
            [str(self.envy), "hash", str(filepath)],
            capture_output=True,
            text=True,
            check=True,
        )
        return result.stdout.strip().split("  ", 1)[0]

    def run_spec(self, identity, spec_path, *flags):
        """Install one local spec through a generated manifest."""
        manifest = test_config.write_spec_manifest(
            self.cache_root, [(identity, spec_path)]
        )
        return test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                *(flags or self.trace_flag),
                "install",
                "--manifest",
                str(manifest),
            ],
            capture_output=True,
            text=True,
        )

    def assert_sole_package(self, result, identity):
        """Exactly one package outcome, and it belongs to `identity`.

        Requires --trace on stderr; pkg_outcome is emitted once per package.
        """
        outcomes = [ln for ln in result.stderr.splitlines() if "pkg_outcome" in ln]
        self.assertEqual(len(outcomes), 1, f"Expected one package: {result.stderr}")
        self.assertIn(f"spec={identity}", outcomes[0])

    def test_declarative_fetch_string(self):
        """Spec with declarative fetch (string format) downloads file."""
        # Create spec with inline content
        spec_content = f"""-- Test declarative fetch with string format
IDENTITY = "local.fetch_string@v1"

-- String format: simple path, no verification
FETCH = "{self.lua_path("simple.lua")}"
"""
        spec_path = self.cache_root / "fetch_string.lua"
        spec_path.write_text(spec_content, encoding="utf-8")

        result = self.run_spec(
            "local.fetch_string@v1", spec_path, "--trace", "--verbose"
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assert_sole_package(result, "local.fetch_string@v1")

        # Verify fetch phase executed
        stderr_lower = result.stderr.lower()
        self.assertIn(
            "fetch", stderr_lower, f"Expected fetch phase log: {result.stderr}"
        )

    def test_declarative_fetch_single_table(self):
        """Spec with declarative fetch (single table with sha256) downloads and verifies."""
        # Compute hash dynamically
        simple_hash = self.get_file_hash(self.test_files_dir / "simple.lua")

        # Create spec with computed hash
        spec_content = f"""-- Test declarative fetch with single table format and SHA256 verification
IDENTITY = "local.fetch_single@v1"

-- Single table format with optional sha256
FETCH = {{
  source = "{self.lua_path("simple.lua")}",
  sha256 = "{simple_hash}"
}}
"""
        modified_spec = self.cache_root / "fetch_single.lua"
        modified_spec.write_text(spec_content)

        result = self.run_spec(
            "local.fetch_single@v1", modified_spec, "--trace", "--verbose"
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assert_sole_package(result, "local.fetch_single@v1")

        # Verify SHA256 verification occurred
        stderr_lower = result.stderr.lower()
        self.assertIn(
            "sha256", stderr_lower, f"Expected SHA256 verification log: {result.stderr}"
        )

    def test_declarative_fetch_array(self):
        """Spec with declarative fetch (array format) downloads multiple files concurrently."""
        # Compute hashes dynamically
        simple_hash = self.get_file_hash(self.test_files_dir / "simple.lua")
        print_single_hash = self.get_file_hash(self.test_files_dir / "print_single.lua")

        # Create spec with computed hashes
        spec_content = f"""-- Test declarative fetch with array format (concurrent downloads)
IDENTITY = "local.fetch_array@v1"

-- Array format: multiple files with optional sha256
FETCH = {{
  {{
    source = "{self.lua_path("simple.lua")}",
    sha256 = "{simple_hash}"
  }},
  {{
    source = "{self.lua_path("print_single.lua")}",
    sha256 = "{print_single_hash}"
  }},
  {{
    source = "{self.lua_path("print_multiple.lua")}"
    -- No sha256 - should still work (permissive mode)
  }}
}}
"""
        modified_spec = self.cache_root / "fetch_array.lua"
        modified_spec.write_text(spec_content)

        result = self.run_spec(
            "local.fetch_array@v1", modified_spec, "--trace", "--verbose"
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assert_sole_package(result, "local.fetch_array@v1")

        # Verify multiple files were downloaded
        stderr_lower = result.stderr.lower()
        self.assertIn(
            "downloading", stderr_lower, f"Expected download log: {result.stderr}"
        )
        # The log should mention "3 file(s)" or similar
        self.assertTrue(
            "3" in result.stderr or "file" in stderr_lower,
            f"Expected multiple file download log: {result.stderr}",
        )

    def test_declarative_fetch_collision(self):
        """Spec with duplicate filenames fails with collision error."""
        # Create two different files with same basename in different subdirs
        subdir1 = self.test_files_dir / "lua"
        subdir2 = self.test_files_dir / "specs"
        subdir1.mkdir()
        subdir2.mkdir()
        (subdir1 / "simple.lua").write_text("-- lua version\n", encoding="utf-8")
        (subdir2 / "simple.lua").write_text("-- specs version\n", encoding="utf-8")

        spec_content = f"""-- Test declarative fetch with filename collision (should error)
IDENTITY = "local.fetch_collision@v1"

-- Both URLs have the same basename "simple.lua" - should error
FETCH = {{
  {{ source = "{(subdir1 / "simple.lua").as_posix()}" }},
  {{ source = "{(subdir2 / "simple.lua").as_posix()}" }}  -- Different file, same basename
}}
"""
        spec_path = self.cache_root / "fetch_collision.lua"
        spec_path.write_text(spec_content, encoding="utf-8")

        result = self.run_spec("local.fetch_collision@v1", spec_path)

        self.assertNotEqual(
            result.returncode, 0, "Expected filename collision to cause failure"
        )
        self.assertIn(
            "collision",
            result.stderr.lower(),
            f"Expected collision error, got: {result.stderr}",
        )
        self.assertIn(
            "simple.lua",
            result.stderr,
            f"Expected filename in error, got: {result.stderr}",
        )

    def test_declarative_fetch_bad_sha256(self):
        """Spec with wrong SHA256 fails verification."""
        spec_content = f"""-- Test declarative fetch with wrong SHA256 (should fail verification)
IDENTITY = "local.fetch_bad_sha256@v1"

-- Wrong sha256 - should fail after download
FETCH = {{
  source = "{self.lua_path("simple.lua")}",
  sha256 = "0000000000000000000000000000000000000000000000000000000000000000"
}}
"""
        spec_path = self.cache_root / "fetch_bad_sha256.lua"
        spec_path.write_text(spec_content, encoding="utf-8")

        result = self.run_spec("local.fetch_bad_sha256@v1", spec_path)

        self.assertNotEqual(
            result.returncode, 0, "Expected SHA256 mismatch to cause failure"
        )
        self.assertIn(
            "sha256",
            result.stderr.lower(),
            f"Expected SHA256 error, got: {result.stderr}",
        )

    def test_declarative_fetch_destination_override(self):
        """Spec with destination field uses it instead of URL-derived basename."""
        simple_hash = self.get_file_hash(self.test_files_dir / "simple.lua")

        spec_content = f"""-- Test declarative fetch with dest override
IDENTITY = "local.fetch_dest_override@v1"

FETCH = {{
  source = "{self.lua_path("simple.lua")}",
  dest ="custom_name.dat",
  sha256 = "{simple_hash}"
}}

function INSTALL(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  -- Verify file exists under the overridden destination name
  local f = io.open(fetch_dir .. "/custom_name.dat", "r")
  if not f then
    error("File not found as custom_name.dat in fetch_dir")
  end
  f:close()

  -- Verify file does NOT exist under the original URL basename
  local g = io.open(fetch_dir .. "/simple.lua", "r")
  if g then
    g:close()
    error("File should NOT exist as simple.lua (dest override failed)")
  end
end
"""
        spec_path = self.cache_root / "fetch_dest_override.lua"
        spec_path.write_text(spec_content, encoding="utf-8")

        result = self.run_spec(
            "local.fetch_dest_override@v1", spec_path, "--trace", "--verbose"
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

    def test_declarative_fetch_destination_array(self):
        """dest field works in array-of-tables format."""
        spec_content = f"""-- Test dest override in array format
IDENTITY = "local.fetch_dest_array@v1"

FETCH = {{
  {{ source = "{self.lua_path("simple.lua")}", dest ="first.dat" }},
  {{ source = "{self.lua_path("print_single.lua")}", dest ="second.dat" }}
}}

function INSTALL(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  local f = io.open(fetch_dir .. "/first.dat", "r")
  if not f then error("first.dat not found in fetch_dir") end
  f:close()

  f = io.open(fetch_dir .. "/second.dat", "r")
  if not f then error("second.dat not found in fetch_dir") end
  f:close()
end
"""
        spec_path = self.cache_root / "fetch_dest_array.lua"
        spec_path.write_text(spec_content, encoding="utf-8")

        result = self.run_spec(
            "local.fetch_dest_array@v1", spec_path, "--trace", "--verbose"
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

    def test_declarative_fetch_destination_resolves_collision(self):
        """dest field can resolve what would otherwise be a filename collision."""
        subdir1 = self.test_files_dir / "a"
        subdir2 = self.test_files_dir / "b"
        subdir1.mkdir()
        subdir2.mkdir()
        (subdir1 / "data.txt").write_text("version a\n", encoding="utf-8")
        (subdir2 / "data.txt").write_text("version b\n", encoding="utf-8")

        spec_content = f"""-- dest resolves collision between same-basename files
IDENTITY = "local.fetch_dest_collision_fix@v1"

FETCH = {{
  {{ source = "{(subdir1 / "data.txt").as_posix()}", dest ="data_a.txt" }},
  {{ source = "{(subdir2 / "data.txt").as_posix()}", dest ="data_b.txt" }}
}}

function INSTALL(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  local f = io.open(fetch_dir .. "/data_a.txt", "r")
  if not f then error("data_a.txt not found") end
  local content_a = f:read("*a")
  f:close()

  f = io.open(fetch_dir .. "/data_b.txt", "r")
  if not f then error("data_b.txt not found") end
  local content_b = f:read("*a")
  f:close()

  if content_a == content_b then
    error("Files should have different content")
  end
end
"""
        spec_path = self.cache_root / "fetch_dest_collision_fix.lua"
        spec_path.write_text(spec_content, encoding="utf-8")

        result = self.run_spec("local.fetch_dest_collision_fix@v1", spec_path)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

    def test_declarative_fetch_destination_collision(self):
        """Duplicate dest values still trigger collision error."""
        spec_content = f"""-- Duplicate dest values should collide
IDENTITY = "local.fetch_dest_dup@v1"

FETCH = {{
  {{ source = "{self.lua_path("simple.lua")}", dest ="same.dat" }},
  {{ source = "{self.lua_path("print_single.lua")}", dest ="same.dat" }}
}}
"""
        spec_path = self.cache_root / "fetch_dest_dup.lua"
        spec_path.write_text(spec_content, encoding="utf-8")

        result = self.run_spec("local.fetch_dest_dup@v1", spec_path)

        self.assertNotEqual(
            result.returncode, 0, "Expected collision error for duplicate dest"
        )
        self.assertIn("collision", result.stderr.lower())
        self.assertIn("same.dat", result.stderr)

    def test_declarative_fetch_string_array(self):
        """Spec with FETCH = {\"url1\", \"url2\", \"url3\"} downloads all files."""
        # Create spec with array of strings (no SHA256)
        spec_content = f"""-- Test declarative fetch with string array
IDENTITY = "local.fetch_string_array@v1"

-- Array of strings (no SHA256 verification)
FETCH = {{
  "{self.lua_path("simple.lua")}",
  "{self.lua_path("print_single.lua")}",
  "{self.lua_path("print_multiple.lua")}"
}}
"""
        spec_path = self.cache_root / "fetch_string_array.lua"
        spec_path.write_text(spec_content, encoding="utf-8")

        result = self.run_spec(
            "local.fetch_string_array@v1", spec_path, "--trace", "--verbose"
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assert_sole_package(result, "local.fetch_string_array@v1")

        # Verify downloading log mentions 3 files
        stderr_lower = result.stderr.lower()
        self.assertIn(
            "downloading", stderr_lower, f"Expected download log: {result.stderr}"
        )
        self.assertIn(
            "3", result.stderr, f"Expected 3 files mentioned: {result.stderr}"
        )

    def test_declarative_fetch_git(self):
        """Spec with git fetch downloads repository."""
        spec_content = """-- Test declarative fetch with git repository
IDENTITY = "local.fetch_git_test@v1"

FETCH = {
    source = "https://github.com/ninja-build/ninja.git",
    ref = "v1.13.2"
}

function INSTALL(install_dir, stage_dir, fetch_dir, tmp_dir, options)
    -- Verify the fetched git repo is available in stage_dir/ninja.git/
    local readme = stage_dir .. "/ninja.git/README.md"
    local f = io.open(readme, "r")
    if not f then
        error("Could not find README.md at: " .. readme)
    end
    f:close()

    -- Verify .git directory is present (kept for packages that need it)
    -- Check by opening a file that must exist in a git repo
    local git_head = stage_dir .. "/ninja.git/.git/HEAD"
    local g = io.open(git_head, "r")
    if not g then
        error(".git directory should be present at: " .. stage_dir .. "/ninja.git/.git (tried to open HEAD file)")
    end
    g:close()
end
"""
        spec_path = self.cache_root / "fetch_git_test.lua"
        spec_path.write_text(spec_content, encoding="utf-8")

        result = self.run_spec("local.fetch_git_test@v1", spec_path)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertIn("local.fetch_git_test@v1", result.stderr)

    def test_declarative_git_in_stage_not_fetch(self):
        """Git repos must be cloned to stage_dir, NOT fetch_dir."""
        # This is a cache-managed package (no check verb) that verifies git placement
        spec_content = """-- Test that git repos go to stage_dir
IDENTITY = "local.git_location_test@v1"

FETCH = {
    source = "https://github.com/ninja-build/ninja.git",
    ref = "v1.13.2"
}

function INSTALL(install_dir, stage_dir, fetch_dir, tmp_dir, options)
    -- Verify git repo is in stage_dir
    local stage_readme = stage_dir .. "/ninja.git/README.md"
    local f = io.open(stage_readme, "r")
    if not f then
        error("Git repo not found in stage_dir at: " .. stage_readme)
    end
    f:close()

    -- Verify git repo is NOT in fetch_dir
    local fetch_readme = fetch_dir .. "/ninja.git/README.md"
    local g = io.open(fetch_readme, "r")
    if g then
        g:close()
        error("Git repo should NOT be in fetch_dir, found at: " .. fetch_readme)
    end

end
"""
        spec_path = self.cache_root / "git_location_test.lua"
        spec_path.write_text(spec_content, encoding="utf-8")

        result = self.run_spec("local.git_location_test@v1", spec_path)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

    def test_declarative_git_no_fetch_complete_marker(self):
        """Git fetches should NOT create fetch completion marker (not cacheable)."""
        # This is a cache-managed package (no check verb) that verifies fetch marker behavior
        spec_content = """-- Test git fetch completion marker
IDENTITY = "local.git_no_cache@v1"

FETCH = {
    source = "https://github.com/ninja-build/ninja.git",
    ref = "v1.13.2"
}

function INSTALL(install_dir, stage_dir, fetch_dir, tmp_dir, options)
    local readme = stage_dir .. "/ninja.git/README.md"
    local f = io.open(readme, "r")
    if not f then
        error("Git repo not found")
    end
    f:close()
end
"""
        spec_path = self.cache_root / "git_no_cache.lua"
        spec_path.write_text(spec_content, encoding="utf-8")

        result = self.run_spec(
            "local.git_no_cache@v1", spec_path, "--trace", "--verbose"
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        # Verify trace log shows skipping fetch completion
        self.assertIn("skipping fetch completion marker", result.stderr.lower())

        # Verify fetch completion marker does NOT exist
        pkg_dir = self.cache_root / "packages" / "local.git_no_cache@v1"
        fetch_complete_files = list(pkg_dir.rglob("fetch/envy-complete"))
        self.assertEqual(
            len(fetch_complete_files),
            0,
            f"fetch/envy-complete should not exist for git repos, found: {fetch_complete_files}",
        )


if __name__ == "__main__":
    unittest.main()
