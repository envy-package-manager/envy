"""Functional tests for envy.loadenv() function.

Tests loading Lua files into sandboxed environments at various scopes:
- Manifest global scope
- Spec global scope
- Phase functions
"""

import unittest
from pathlib import Path

from . import test_config
from .env import EnvyTestCase


class TestLoadenvBasic(EnvyTestCase):
    """Tests for basic envy.loadenv() functionality."""

    def setUp(self):
        super().setUp()
        self.test_dir = self.make_temp_dir("test_dir")

    @staticmethod
    def lua_path(path: Path) -> str:
        return path.as_posix()

    def create_helper_file(self, name: str, content: str) -> Path:
        helper_path = self.test_dir / name
        helper_path.parent.mkdir(parents=True, exist_ok=True)
        helper_path.write_text(content, encoding="utf-8")
        return helper_path

    def run_sync(self, manifest: Path, install_all: bool = True):
        cmd = [
            str(self.envy),
            "--cache-root",
            str(self.cache_root),
            "install" if install_all else "sync",
            "--manifest",
            str(manifest),
        ]
        return test_config.run(
            cmd,
            cwd=self.project_root,
            capture_output=True,
            text=True,
        )

    def test_loadenv_in_manifest_global_scope(self):
        """envy.loadenv() works at manifest global scope."""
        # Create helper file with PACKAGES
        helper_content = """PACKAGES = {
    -- Empty packages list from helper
}
"""
        self.create_helper_file("helper.lua", helper_content)

        # Create manifest that loads helper
        manifest_content = f"""-- @envy bin-dir "tools"
local helper = envy.loadenv("helper")
PACKAGES = helper.PACKAGES or {{}}
"""
        manifest_path = self.test_dir / "envy.lua"
        manifest_path.write_text(manifest_content, encoding="utf-8")

        result = self.run_sync(manifest=manifest_path)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

    def test_loadenv_in_spec_global_scope(self):
        """envy.loadenv() works at spec global scope."""
        # Create helper file
        helper_content = """HELPER_VALUE = "from_helper"
"""
        self.create_helper_file("helper.lua", helper_content)

        # Create spec that uses loadenv at global scope
        spec_content = """IDENTITY = "local.loadenv-test@v1"
DEPENDENCIES = {}

local helper = envy.loadenv("helper")
LOADED_VALUE = helper.HELPER_VALUE

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
        spec_path = self.test_dir / "spec.lua"
        spec_path.write_text(spec_content, encoding="utf-8")

        manifest_content = f"""-- @envy bin-dir "tools"
PACKAGES = {{
    {{ spec = "local.loadenv-test@v1", source = "{self.lua_path(spec_path)}" }},
}}
"""
        manifest_path = self.test_dir / "envy.lua"
        manifest_path.write_text(manifest_content, encoding="utf-8")

        result = self.run_sync(manifest=manifest_path)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

    def test_loadenv_in_phase_function(self):
        """envy.loadenv() works inside phase functions."""
        # Create helper file
        helper_content = """TOOL_VERSION = "1.2.3"
"""
        self.create_helper_file("version.lua", helper_content)

        # Create spec that uses loadenv in INSTALL phase
        spec_content = """IDENTITY = "local.phase-loadenv@v1"
DEPENDENCIES = {}

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options)
      local ver = envy.loadenv("version")
      return ver.TOOL_VERSION == "1.2.3"
    end,
    INSTALL = function(pkg_dir, options)
    end,
  },
}

"""
        spec_path = self.test_dir / "spec.lua"
        spec_path.write_text(spec_content, encoding="utf-8")

        manifest_content = f"""-- @envy bin-dir "tools"
PACKAGES = {{
    {{ spec = "local.phase-loadenv@v1", source = "{self.lua_path(spec_path)}" }},
}}
"""
        manifest_path = self.test_dir / "envy.lua"
        manifest_path.write_text(manifest_content, encoding="utf-8")

        result = self.run_sync(manifest=manifest_path)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

    def test_loadenv_sandboxing(self):
        """envy.loadenv() captures globals in returned table (sandboxed)."""
        # Create helper that sets multiple globals
        helper_content = """FOO = "foo_value"
BAR = 42
BAZ = {nested = true}
"""
        self.create_helper_file("globals.lua", helper_content)

        # Create spec that verifies sandbox captures globals
        spec_content = """IDENTITY = "local.sandbox-test@v1"
DEPENDENCIES = {}

local env = envy.loadenv("globals")

-- Verify globals are in the returned table
assert(env.FOO == "foo_value", "FOO not captured")
assert(env.BAR == 42, "BAR not captured")
assert(env.BAZ.nested == true, "BAZ not captured")

-- Verify globals did NOT leak into our _G
assert(FOO == nil, "FOO leaked to _G")
assert(BAR == nil, "BAR leaked to _G")
assert(BAZ == nil, "BAZ leaked to _G")

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
        spec_path = self.test_dir / "spec.lua"
        spec_path.write_text(spec_content, encoding="utf-8")

        manifest_content = f"""-- @envy bin-dir "tools"
PACKAGES = {{
    {{ spec = "local.sandbox-test@v1", source = "{self.lua_path(spec_path)}" }},
}}
"""
        manifest_path = self.test_dir / "envy.lua"
        manifest_path.write_text(manifest_content, encoding="utf-8")

        result = self.run_sync(manifest=manifest_path)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

    def test_loadenv_path_resolution_relative(self):
        """envy.loadenv() resolves paths relative to the caller's file."""
        # Create helper in subdirectory
        subdir = self.test_dir / "subdir"
        subdir.mkdir(parents=True, exist_ok=True)

        helper_content = """SUBDIR_VALUE = "in_subdir"
"""
        (subdir / "helper.lua").write_text(helper_content, encoding="utf-8")

        # Create spec in test_dir that loads from subdir
        spec_content = """IDENTITY = "local.relative-path@v1"
DEPENDENCIES = {}

local helper = envy.loadenv("subdir.helper")
assert(helper.SUBDIR_VALUE == "in_subdir", "Failed to load from subdir")

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
        spec_path = self.test_dir / "spec.lua"
        spec_path.write_text(spec_content, encoding="utf-8")

        manifest_content = f"""-- @envy bin-dir "tools"
PACKAGES = {{
    {{ spec = "local.relative-path@v1", source = "{self.lua_path(spec_path)}" }},
}}
"""
        manifest_path = self.test_dir / "envy.lua"
        manifest_path.write_text(manifest_content, encoding="utf-8")

        result = self.run_sync(manifest=manifest_path)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

    def test_loadenv_with_extend(self):
        """envy.extend() works with tables from envy.loadenv()."""
        # Create helper with packages
        helper_content = """HELPER_PACKAGES = {
    -- This would normally have package entries
}
"""
        self.create_helper_file("packages.lua", helper_content)

        # Create manifest using loadenv and extend
        manifest_content = f"""-- @envy bin-dir "tools"
local helper = envy.loadenv("packages")
PACKAGES = envy.extend(helper.HELPER_PACKAGES or {{}}, {{
    -- Additional packages would go here
}})
"""
        manifest_path = self.test_dir / "envy.lua"
        manifest_path.write_text(manifest_content, encoding="utf-8")

        result = self.run_sync(manifest=manifest_path)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

    def test_loadenv_stdlib_access(self):
        """envy.loadenv() loaded files have access to Lua stdlib."""
        # Create helper that uses stdlib functions
        helper_content = """-- Use various stdlib functions
local str = string.format("value: %d", 42)
local math_val = math.floor(3.7)
local tbl = table.concat({"a", "b", "c"}, ",")

RESULTS = {
    str = str,
    math_val = math_val,
    tbl = tbl,
}
"""
        self.create_helper_file("stdlib_test.lua", helper_content)

        spec_content = """IDENTITY = "local.stdlib-access@v1"
DEPENDENCIES = {}

local helper = envy.loadenv("stdlib_test")
assert(helper.RESULTS.str == "value: 42", "string.format failed")
assert(helper.RESULTS.math_val == 3, "math.floor failed")
assert(helper.RESULTS.tbl == "a,b,c", "table.concat failed")

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
        spec_path = self.test_dir / "spec.lua"
        spec_path.write_text(spec_content, encoding="utf-8")

        manifest_content = f"""-- @envy bin-dir "tools"
PACKAGES = {{
    {{ spec = "local.stdlib-access@v1", source = "{self.lua_path(spec_path)}" }},
}}
"""
        manifest_path = self.test_dir / "envy.lua"
        manifest_path.write_text(manifest_content, encoding="utf-8")

        result = self.run_sync(manifest=manifest_path)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")


class TestLoadenvErrors(EnvyTestCase):
    """Tests for envy.loadenv() error cases."""

    def setUp(self):
        super().setUp()
        self.test_dir = self.make_temp_dir("test_dir")

    @staticmethod
    def lua_path(path: Path) -> str:
        return path.as_posix()

    def run_sync(self, manifest: Path):
        cmd = [
            str(self.envy),
            "--cache-root",
            str(self.cache_root),
            "install",
            "--manifest",
            str(manifest),
        ]
        return test_config.run(
            cmd,
            cwd=self.project_root,
            capture_output=True,
            text=True,
        )

    def test_loadenv_file_not_found(self):
        """envy.loadenv() errors when file doesn't exist."""
        spec_content = """IDENTITY = "local.missing-file@v1"
DEPENDENCIES = {}

local helper = envy.loadenv("nonexistent")

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
        spec_path = self.test_dir / "spec.lua"
        spec_path.write_text(spec_content, encoding="utf-8")

        manifest_content = f"""-- @envy bin-dir "tools"
PACKAGES = {{
    {{ spec = "local.missing-file@v1", source = "{self.lua_path(spec_path)}" }},
}}
"""
        manifest_path = self.test_dir / "envy.lua"
        manifest_path.write_text(manifest_content, encoding="utf-8")

        result = self.run_sync(manifest=manifest_path)

        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(
            "nonexistent" in result.stderr.lower()
            or "cannot open" in result.stderr.lower(),
            f"Expected file not found error, got: {result.stderr}",
        )


if __name__ == "__main__":
    unittest.main()
