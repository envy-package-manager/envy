"""Functional tests for engine programmatic fetch (fetch functions).

Tests FETCH = function(ctx) ... end syntax with envy.fetch() and envy.commit_fetch().
"""

import os
from pathlib import Path
import unittest

from . import test_config
from .env import EnvyTestCase

# Inline test file contents
TEST_FILES = {
    "simple.lua": "-- Simple test script for lua_util tests\nexpected_value = 42\n",
    "print_single.lua": 'print("hello")\n',
    "print_multiple.lua": 'print("a", "b", "c")\n',
}


class TestEngineProgrammaticFetch(EnvyTestCase):
    """Tests for programmatic fetch phase (fetch functions)."""

    def setUp(self):
        super().setUp()
        self.test_files_dir = self.make_temp_dir("test_files_dir")
        self.trace_flag = ["--trace"] if os.environ.get("ENVY_TEST_TRACE") else []

        # Write test files to temp directory
        for name, content in TEST_FILES.items():
            (self.test_files_dir / name).write_text(content, encoding="utf-8")

    def lua_path(self, filename: str) -> str:
        """Get Lua-safe path for a test file."""
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

    def test_fetch_single_string(self):
        """envy.fetch("url") returns scalar string basename."""
        spec_content = f"""IDENTITY = "local.prog_fetch_single@v1"

function FETCH(tmp_dir, options)
  local file = envy.fetch("{self.lua_path("simple.lua")}", {{dest = tmp_dir}})

  -- Verify return is scalar string, not array
  if type(file) ~= "string" then
    error("Expected string return, got " .. type(file))
  end

  envy.commit_fetch(file)
end
"""
        spec_path = self.cache_root / "prog_fetch_single.lua"
        spec_path.write_text(spec_content, encoding="utf-8")

        result = self.run_spec("local.prog_fetch_single@v1", spec_path)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertIn("local.prog_fetch_single@v1", result.stderr)

    def test_fetch_string_array(self):
        """envy.fetch({"url1", "url2"}) returns array of basenames."""
        spec_content = f"""IDENTITY = "local.prog_fetch_array@v1"

function FETCH(tmp_dir, options)
  local files = envy.fetch({{
    "{self.lua_path("simple.lua")}",
    "{self.lua_path("print_single.lua")}"
  }}, {{dest = tmp_dir}})

  -- Verify return is array
  if type(files) ~= "table" then
    error("Expected table return, got " .. type(files))
  end
  if #files ~= 2 then
    error("Expected 2 files, got " .. #files)
  end

  envy.commit_fetch(files)
end
"""
        spec_path = self.cache_root / "prog_fetch_array.lua"
        spec_path.write_text(spec_content, encoding="utf-8")

        result = self.run_spec("local.prog_fetch_array@v1", spec_path)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

    def test_fetch_single_table(self):
        """envy.fetch({url="..."}) returns scalar basename."""
        spec_content = f"""IDENTITY = "local.prog_fetch_table@v1"

function FETCH(tmp_dir, options)
  local file = envy.fetch({{source = "{self.lua_path("simple.lua")}"}}, {{dest = tmp_dir}})

  -- Verify return is scalar string
  if type(file) ~= "string" then
    error("Expected string return, got " .. type(file))
  end

  envy.commit_fetch(file)
end
"""
        spec_path = self.cache_root / "prog_fetch_table.lua"
        spec_path.write_text(spec_content, encoding="utf-8")

        result = self.run_spec("local.prog_fetch_table@v1", spec_path)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

    def test_fetch_table_array(self):
        """envy.fetch({{url="..."}, {...}}) returns array."""
        spec_content = f"""IDENTITY = "local.prog_fetch_table_array@v1"

function FETCH(tmp_dir, options)
  local files = envy.fetch({{
    {{source = "{self.lua_path("simple.lua")}"}},
    {{source = "{self.lua_path("print_single.lua")}"}}
  }}, {{dest = tmp_dir}})

  -- Verify return is array
  if type(files) ~= "table" or #files ~= 2 then
    error("Expected array of 2 files")
  end

  envy.commit_fetch(files)
end
"""
        spec_path = self.cache_root / "prog_fetch_table_array.lua"
        spec_path.write_text(spec_content, encoding="utf-8")

        result = self.run_spec("local.prog_fetch_table_array@v1", spec_path)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

    def test_fetch_single_table_destination(self):
        """envy.fetch({source="...", dest="..."}) uses dest as basename."""
        spec_content = f"""IDENTITY = "local.prog_fetch_dest@v1"

function FETCH(tmp_dir, options)
  local file = envy.fetch({{
    source = "{self.lua_path("simple.lua")}",
    dest ="renamed.dat"
  }}, {{dest = tmp_dir}})

  if file ~= "renamed.dat" then
    error("Expected basename 'renamed.dat', got '" .. file .. "'")
  end

  -- Verify file exists under the new name
  local f = io.open(tmp_dir .. "/renamed.dat", "r")
  if not f then error("renamed.dat not in tmp_dir") end
  f:close()

  envy.commit_fetch(file)
end
"""
        spec_path = self.cache_root / "prog_fetch_dest.lua"
        spec_path.write_text(spec_content, encoding="utf-8")

        result = self.run_spec("local.prog_fetch_dest@v1", spec_path)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

    def test_fetch_table_array_destination(self):
        """envy.fetch({{source=..., dest=...}, ...}) returns overridden basenames."""
        spec_content = f"""IDENTITY = "local.prog_fetch_dest_array@v1"

function FETCH(tmp_dir, options)
  local files = envy.fetch({{
    {{source = "{self.lua_path("simple.lua")}", dest ="alpha.dat"}},
    {{source = "{self.lua_path("print_single.lua")}", dest ="beta.dat"}}
  }}, {{dest = tmp_dir}})

  if type(files) ~= "table" or #files ~= 2 then
    error("Expected array of 2 files")
  end
  if files[1] ~= "alpha.dat" then
    error("Expected 'alpha.dat', got '" .. files[1] .. "'")
  end
  if files[2] ~= "beta.dat" then
    error("Expected 'beta.dat', got '" .. files[2] .. "'")
  end

  envy.commit_fetch(files)
end
"""
        spec_path = self.cache_root / "prog_fetch_dest_array.lua"
        spec_path.write_text(spec_content, encoding="utf-8")

        result = self.run_spec("local.prog_fetch_dest_array@v1", spec_path)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

    def test_fetch_function_returns_table_with_destination(self):
        """FETCH function returning {source=..., dest=...} uses dest."""
        spec_content = f"""IDENTITY = "local.prog_return_dest@v1"

FETCH = function(tmp_dir, options)
  return {{
    source = "{self.lua_path("simple.lua")}",
    dest ="output.dat"
  }}
end

function INSTALL(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  local f = io.open(fetch_dir .. "/output.dat", "r")
  if not f then error("output.dat not found in fetch_dir") end
  f:close()
end
"""
        spec_path = self.cache_root / "prog_return_dest.lua"
        spec_path.write_text(spec_content, encoding="utf-8")

        result = self.run_spec("local.prog_return_dest@v1", spec_path)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

    def test_commit_fetch_scalar_string(self):
        """envy.commit_fetch("file.tar.gz") moves file to fetch_dir."""
        spec_content = f"""IDENTITY = "local.prog_commit_scalar@v1"

function FETCH(tmp_dir, options)
  local file = envy.fetch("{self.lua_path("simple.lua")}", {{dest = tmp_dir}})

  -- File should be in tmp before commit
  local tmp_path = tmp_dir .. "/" .. file
  local f = io.open(tmp_path, "r")
  if not f then
    error("File not in tmp: " .. tmp_path)
  end
  f:close()

  envy.commit_fetch(file)
end
"""
        spec_path = self.cache_root / "prog_commit_scalar.lua"
        spec_path.write_text(spec_content, encoding="utf-8")

        result = self.run_spec("local.prog_commit_scalar@v1", spec_path)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

    def test_commit_fetch_with_sha256(self):
        """envy.commit_fetch({filename="...", sha256="..."}) verifies hash."""
        # Compute hash dynamically
        test_file = self.test_files_dir / "simple.lua"
        file_hash = self.get_file_hash(test_file)

        spec_content = f"""IDENTITY = "local.prog_commit_sha256@v1"

function FETCH(tmp_dir, options)
  local file = envy.fetch("{self.lua_path("simple.lua")}", {{dest = tmp_dir}})

  -- Commit with SHA256 verification
  envy.commit_fetch({{
    filename = file,
    sha256 = "{file_hash}"
  }})
end
"""
        spec_path = self.cache_root / "prog_commit_sha256.lua"
        spec_path.write_text(spec_content, encoding="utf-8")

        result = self.run_spec(
            "local.prog_commit_sha256@v1", spec_path, "--trace", "--verbose"
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        # Verify SHA256 verification happened (requires --trace to see the log)
        self.assertIn("sha256", result.stderr.lower())

    def test_commit_fetch_sha256_mismatch(self):
        """Wrong SHA256 in commit_fetch causes verification error."""
        spec_content = f"""IDENTITY = "local.prog_commit_bad_sha256@v1"

function FETCH(tmp_dir, options)
  local file = envy.fetch("{self.lua_path("simple.lua")}", {{dest = tmp_dir}})

  -- Commit with wrong SHA256
  envy.commit_fetch({{
    filename = file,
    sha256 = "0000000000000000000000000000000000000000000000000000000000000000"
  }})
end
"""
        spec_path = self.cache_root / "prog_commit_bad_sha256.lua"
        spec_path.write_text(spec_content, encoding="utf-8")

        result = self.run_spec("local.prog_commit_bad_sha256@v1", spec_path)

        self.assertNotEqual(result.returncode, 0, "Expected SHA256 mismatch to fail")
        self.assertIn("sha256", result.stderr.lower())

    def test_commit_fetch_array(self):
        """envy.commit_fetch({"file1", "file2"}) commits multiple files."""
        spec_content = f"""IDENTITY = "local.prog_commit_array@v1"

function FETCH(tmp_dir, options)
  local files = envy.fetch({{
    "{self.lua_path("simple.lua")}",
    "{self.lua_path("print_single.lua")}"
  }}, {{dest = tmp_dir}})

  envy.commit_fetch(files)
end
"""
        spec_path = self.cache_root / "prog_commit_array.lua"
        spec_path.write_text(spec_content, encoding="utf-8")

        result = self.run_spec("local.prog_commit_array@v1", spec_path)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

    def test_commit_fetch_missing_file(self):
        """Trying to commit file not in tmp_dir fails with clear error."""
        spec_content = """IDENTITY = "local.prog_commit_missing@v1"

function FETCH(tmp_dir, options)
  -- Try to commit file that was never fetched
  envy.commit_fetch("nonexistent.tar.gz")
end
"""
        spec_path = self.cache_root / "prog_commit_missing.lua"
        spec_path.write_text(spec_content, encoding="utf-8")

        result = self.run_spec("local.prog_commit_missing@v1", spec_path)

        self.assertNotEqual(result.returncode, 0, "Expected missing file error")
        stderr_lower = result.stderr.lower()
        self.assertTrue(
            "not found" in stderr_lower or "missing" in stderr_lower,
            f"Expected file not found error: {result.stderr}",
        )

    def test_selective_commit(self):
        """Fetch 3 files, commit 2, verify tmp cleanup removes uncommitted."""
        spec_content = f"""IDENTITY = "local.prog_selective_commit@v1"

function FETCH(tmp_dir, options)
  local files = envy.fetch({{
    "{self.lua_path("simple.lua")}",
    "{self.lua_path("print_single.lua")}",
    "{self.lua_path("print_multiple.lua")}"
  }}, {{dest = tmp_dir}})

  -- Only commit first 2
  envy.commit_fetch({{files[1], files[2]}})

  -- Third file should still be in tmp here but will be cleaned up
end
"""
        spec_path = self.cache_root / "prog_selective_commit.lua"
        spec_path.write_text(spec_content, encoding="utf-8")

        result = self.run_spec("local.prog_selective_commit@v1", spec_path)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

    def test_ctx_identity(self):
        """IDENTITY contains spec identity."""
        spec_content = f"""IDENTITY = "local.prog_ctx_identity@v1"

function FETCH(tmp_dir, options)
  if IDENTITY ~= "local.prog_ctx_identity@v1" then
    error("Expected identity 'local.prog_ctx_identity@v1', got: " .. IDENTITY)
  end

  -- Fetch something so phase completes successfully
  local file = envy.fetch("{self.lua_path("simple.lua")}", {{dest = tmp_dir}})
  envy.commit_fetch(file)
end
"""
        spec_path = self.cache_root / "prog_ctx_identity.lua"
        spec_path.write_text(spec_content, encoding="utf-8")

        result = self.run_spec("local.prog_ctx_identity@v1", spec_path)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

    def test_ctx_options(self):
        """opts is accessible as a table (empty when no options passed)."""
        spec_content = f"""IDENTITY = "local.prog_ctx_options@v1"

function FETCH(tmp_dir, options)
  -- Verify options exists and is a table
  if type(options) ~= "table" then
    error("Expected options to be table, got: " .. type(options))
  end

  local file = envy.fetch("{self.lua_path("simple.lua")}", {{dest = tmp_dir}})
  envy.commit_fetch(file)
end
"""
        spec_path = self.cache_root / "prog_ctx_options.lua"
        spec_path.write_text(spec_content, encoding="utf-8")

        result = self.run_spec("local.prog_ctx_options@v1", spec_path)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

    def test_ctx_options_empty(self):
        """opts exists as empty table when no options specified."""
        spec_content = f"""IDENTITY = "local.prog_ctx_options_empty@v1"

function FETCH(tmp_dir, options)
  if type(options) ~= "table" then
    error("Expected options to be table, got: " .. type(options))
  end

  -- Check it's empty
  local count = 0
  for k, v in pairs(options) do
    count = count + 1
  end

  if count ~= 0 then
    error("Expected empty options, got " .. count .. " entries")
  end

  local file = envy.fetch("{self.lua_path("simple.lua")}", {{dest = tmp_dir}})
  envy.commit_fetch(file)
end
"""
        spec_path = self.cache_root / "prog_ctx_options_empty.lua"
        spec_path.write_text(spec_content, encoding="utf-8")

        result = self.run_spec("local.prog_ctx_options_empty@v1", spec_path)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

    def test_multiple_serial_fetches(self):
        """Multiple envy.fetch() calls execute serially, files accumulate."""
        spec_content = f"""IDENTITY = "local.prog_serial_fetches@v1"

function FETCH(tmp_dir, options)
  -- First fetch
  local file1 = envy.fetch("{self.lua_path("simple.lua")}", {{dest = tmp_dir}})

  -- Verify file1 exists in tmp
  local f = io.open(tmp_dir .. "/" .. file1, "r")
  if not f then error("file1 not in tmp after first fetch") end
  f:close()

  -- Second fetch
  local file2 = envy.fetch("{self.lua_path("print_single.lua")}", {{dest = tmp_dir}})

  -- Verify both files exist
  f = io.open(tmp_dir .. "/" .. file1, "r")
  if not f then error("file1 disappeared after second fetch") end
  f:close()

  f = io.open(tmp_dir .. "/" .. file2, "r")
  if not f then error("file2 not in tmp after second fetch") end
  f:close()

  -- Third fetch
  local file3 = envy.fetch("{self.lua_path("print_multiple.lua")}", {{dest = tmp_dir}})

  -- Commit all
  envy.commit_fetch({{file1, file2, file3}})
end
"""
        spec_path = self.cache_root / "prog_serial_fetches.lua"
        spec_path.write_text(spec_content, encoding="utf-8")

        result = self.run_spec("local.prog_serial_fetches@v1", spec_path)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

    def test_fetch_function_error_propagation(self):
        """Lua errors in fetch function propagate with spec identity."""
        spec_content = """IDENTITY = "local.prog_error_prop@v1"

function FETCH(tmp_dir, options)
  error("Intentional test error")
end
"""
        spec_path = self.cache_root / "prog_error_prop.lua"
        spec_path.write_text(spec_content, encoding="utf-8")

        result = self.run_spec("local.prog_error_prop@v1", spec_path)

        self.assertNotEqual(result.returncode, 0, "Expected error to cause failure")
        # Verify error mentions spec identity
        self.assertIn("local.prog_error_prop@v1", result.stderr)
        self.assertIn("Intentional test error", result.stderr)

    def test_fetch_function_returns_string(self):
        """FETCH = function(ctx) return "url" end (declarative string shorthand)."""
        spec_content = f"""IDENTITY = "local.prog_return_string@v1"

FETCH = function(ctx)
  return "{self.lua_path("simple.lua")}"
end
"""
        spec_path = self.cache_root / "prog_return_string.lua"
        spec_path.write_text(spec_content, encoding="utf-8")

        result = self.run_spec("local.prog_return_string@v1", spec_path)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertIn("local.prog_return_string@v1", result.stderr)

    def test_fetch_function_returns_table_single(self):
        """FETCH = function(ctx) return {source="...", sha256="..."} end."""
        # Get hash of test file
        test_file = self.test_files_dir / "simple.lua"
        file_hash = self.get_file_hash(test_file)

        spec_content = f"""IDENTITY = "local.prog_return_table@v1"

FETCH = function(ctx)
  return {{source = "{self.lua_path("simple.lua")}", sha256 = "{file_hash}"}}
end
"""
        spec_path = self.cache_root / "prog_return_table.lua"
        spec_path.write_text(spec_content, encoding="utf-8")

        result = self.run_spec("local.prog_return_table@v1", spec_path)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

    def test_fetch_function_returns_table_array(self):
        """FETCH = function(ctx) return {{source="..."}, {source="..."}} end."""
        spec_content = f"""IDENTITY = "local.prog_return_array@v1"

FETCH = function(ctx)
  return {{
    {{source = "{self.lua_path("simple.lua")}"}},
    {{source = "{self.lua_path("print_single.lua")}"}}
  }}
end
"""
        spec_path = self.cache_root / "prog_return_array.lua"
        spec_path.write_text(spec_content, encoding="utf-8")

        result = self.run_spec("local.prog_return_array@v1", spec_path)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

    def test_fetch_function_returns_string_array(self):
        """FETCH = function(ctx) return {"url1", "url2"} end."""
        spec_content = f"""IDENTITY = "local.prog_return_str_array@v1"

FETCH = function(ctx)
  return {{
    "{self.lua_path("simple.lua")}",
    "{self.lua_path("print_single.lua")}"
  }}
end
"""
        spec_path = self.cache_root / "prog_return_str_array.lua"
        spec_path.write_text(spec_content, encoding="utf-8")

        result = self.run_spec("local.prog_return_str_array@v1", spec_path)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

    def test_fetch_function_returns_with_options_templating(self):
        """FETCH = function(ctx, opts) return with opts templating."""
        # The manifest entry passes no options, so the defaults apply.
        spec_content = f"""IDENTITY = "local.prog_options_template@v1"

function FETCH(tmp_dir, options)
  local opts = options or {{}}
  local filename = opts.filename or "simple.lua"
  return "{self.test_files_dir.as_posix()}/" .. filename
end
"""
        spec_path = self.cache_root / "prog_options_template.lua"
        spec_path.write_text(spec_content, encoding="utf-8")

        result = self.run_spec("local.prog_options_template@v1", spec_path)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

    def test_fetch_function_mixed_imperative_and_declarative(self):
        """FETCH = function(ctx) calls envy.fetch() and returns table (mixed mode)."""
        spec_content = f"""IDENTITY = "local.prog_mixed_mode@v1"

function FETCH(tmp_dir, options)
  -- Imperative: fetch and commit one file
  local file1 = envy.fetch("{self.lua_path("simple.lua")}", {{dest = tmp_dir}})
  envy.commit_fetch(file1)

  -- Declarative: return spec for more files
  return {{
    "{self.lua_path("print_single.lua")}",
    "{self.lua_path("print_multiple.lua")}"
  }}
end
"""
        spec_path = self.cache_root / "prog_mixed_mode.lua"
        spec_path.write_text(spec_content, encoding="utf-8")

        result = self.run_spec("local.prog_mixed_mode@v1", spec_path)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

    def test_fetch_function_returns_nil(self):
        """FETCH = function(ctx) with explicit return nil (imperative mode)."""
        spec_content = f"""IDENTITY = "local.prog_return_nil@v1"

function FETCH(tmp_dir, options)
  local file = envy.fetch("{self.lua_path("simple.lua")}", {{dest = tmp_dir}})
  envy.commit_fetch(file)
  return nil
end
"""
        spec_path = self.cache_root / "prog_return_nil.lua"
        spec_path.write_text(spec_content, encoding="utf-8")

        result = self.run_spec("local.prog_return_nil@v1", spec_path)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

    def test_fetch_function_returns_invalid_type(self):
        """FETCH = function(ctx) returns number (error)."""
        spec_content = """IDENTITY = "local.prog_return_invalid@v1"

FETCH = function(ctx)
  return 42
end
"""
        spec_path = self.cache_root / "prog_return_invalid.lua"
        spec_path.write_text(spec_content, encoding="utf-8")

        result = self.run_spec("local.prog_return_invalid@v1", spec_path)

        self.assertNotEqual(
            result.returncode, 0, "Expected error for invalid return type"
        )
        self.assertIn("must return nil, string, or table", result.stderr)


if __name__ == "__main__":
    unittest.main()
