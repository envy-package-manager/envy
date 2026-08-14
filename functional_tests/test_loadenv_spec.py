"""Functional tests for envy.loadenv_spec() function.

Tests loading Lua code from declared dependencies:
- Within phase functions (valid)
- At global scope (error)
- Phase validation (needed_by)
- Standard require() within bundles
"""

import subprocess
import unittest
from pathlib import Path

from . import test_config
from .env import EnvyTestCase
from .test_config import make_manifest


class TestLoadenvSpec(EnvyTestCase):
    """Tests for envy.loadenv_spec() functionality."""

    def setUp(self):
        super().setUp()
        self.test_dir = self.make_temp_dir("test_dir")
        self.bundle_dir = self.make_temp_dir("bundle_dir")

    @staticmethod
    def lua_path(path: Path) -> str:
        return path.as_posix()

    def create_manifest(self, content: str) -> Path:
        manifest_path = self.test_dir / "envy.lua"
        manifest_path.write_text(make_manifest(content), encoding="utf-8")
        return manifest_path

    def create_spec(self, name: str, content: str) -> Path:
        spec_path = self.test_dir / f"{name}.lua"
        spec_path.write_text(content, encoding="utf-8")
        return spec_path

    def create_bundle_with_helper(
        self,
        bundle_identity: str,
        specs: dict[str, str],
        helper_path: str,
        helper_content: str,
    ) -> Path:
        """Create a bundle with specs and a helper file."""
        specs_dir = self.bundle_dir / "specs"
        specs_dir.mkdir(parents=True, exist_ok=True)

        # Create bundle manifest
        specs_lua = ",\n".join(
            f'  ["{spec_id}"] = "{path}"' for spec_id, path in specs.items()
        )
        bundle_lua = f"""BUNDLE = "{bundle_identity}"
SPECS = {{
{specs_lua}
}}
"""
        (self.bundle_dir / "envy-bundle.lua").write_text(bundle_lua)

        # Create spec files
        for spec_id, path in specs.items():
            spec_path = self.bundle_dir / path
            spec_path.parent.mkdir(parents=True, exist_ok=True)
            spec_lua = f"""IDENTITY = "{spec_id}"
DEPENDENCIES = {{}}

USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
    end,
  }},
}}

"""
            spec_path.write_text(spec_lua)

        # Create helper file
        helper_file_path = self.bundle_dir / helper_path
        helper_file_path.parent.mkdir(parents=True, exist_ok=True)
        helper_file_path.write_text(helper_content)

        return self.bundle_dir

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

    def test_loadenv_spec_in_phase_function(self):
        """envy.loadenv_spec() works within phase functions."""
        # Create bundle with helper
        bundle_path = self.create_bundle_with_helper(
            "test.helpers@v1",
            {"test.dummy@v1": "specs/dummy.lua"},
            "lib/helper.lua",
            """HELPER_VERSION = "1.0.0"
function get_message()
  return "Hello from helper"
end
""",
        )

        # Create spec that depends on bundle and uses loadenv_spec in phase
        spec_content = f"""IDENTITY = "local.consumer@v1"
DEPENDENCIES = {{
  {{
    bundle = "test.helpers@v1",
    source = "{self.lua_path(bundle_path)}",
    needed_by = "check",
  }},
}}

USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      local helper = envy.loadenv_spec("test.helpers@v1", "lib.helper")
      return helper.HELPER_VERSION == "1.0.0"
    end,
    INSTALL = function(pkg_dir, options)
    end,
  }},
}}

"""
        spec_path = self.create_spec("consumer", spec_content)

        manifest = self.create_manifest(
            f"""
PACKAGES = {{
    {{ spec = "local.consumer@v1", source = "{self.lua_path(spec_path)}" }},
}}
"""
        )

        result = self.run_sync(manifest=manifest)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

    def test_loadenv_spec_at_global_scope_errors(self):
        """envy.loadenv_spec() at global scope produces error."""
        # Create bundle with helper
        bundle_path = self.create_bundle_with_helper(
            "test.helpers@v1",
            {"test.dummy@v1": "specs/dummy.lua"},
            "lib/helper.lua",
            """HELPER_VERSION = "1.0.0"
""",
        )

        # Create spec that calls loadenv_spec at global scope (ERROR)
        spec_content = f"""IDENTITY = "local.bad-consumer@v1"
DEPENDENCIES = {{
  {{
    bundle = "test.helpers@v1",
    source = "{self.lua_path(bundle_path)}",
  }},
}}

-- This should ERROR - loadenv_spec at global scope
local helper = envy.loadenv_spec("test.helpers@v1", "lib.helper")

USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
    end,
  }},
}}

"""
        spec_path = self.create_spec("bad_consumer", spec_content)

        manifest = self.create_manifest(
            f"""
PACKAGES = {{
    {{ spec = "local.bad-consumer@v1", source = "{self.lua_path(spec_path)}" }},
}}
"""
        )

        result = self.run_sync(manifest=manifest)

        self.assertNotEqual(
            result.returncode, 0, "Expected error for global scope call"
        )
        self.assertTrue(
            "global scope" in result.stderr.lower() or "phase" in result.stderr.lower(),
            f"Expected error about global scope, got: {result.stderr}",
        )

    def test_loadenv_spec_undeclared_dependency_errors(self):
        """envy.loadenv_spec() with undeclared dependency produces error."""
        # Create spec that tries to load from undeclared dependency
        spec_content = """IDENTITY = "local.undeclared@v1"
DEPENDENCIES = {}

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options)
      -- Error: test.helpers@v1 not declared as dependency
      local helper = envy.loadenv_spec("test.helpers@v1", "lib.helper")
      return false
    end,
    INSTALL = function(pkg_dir, options)
    end,
  },
}

"""
        spec_path = self.create_spec("undeclared", spec_content)

        manifest = self.create_manifest(
            f"""
PACKAGES = {{
    {{ spec = "local.undeclared@v1", source = "{self.lua_path(spec_path)}", setup = {{ "main" }} }},
}}
"""
        )

        result = self.run_sync(manifest=manifest)

        self.assertNotEqual(
            result.returncode, 0, "Expected error for undeclared dependency"
        )
        self.assertTrue(
            "dependency" in result.stderr.lower()
            or "not found" in result.stderr.lower(),
            f"Expected dependency error, got: {result.stderr}",
        )

    def test_loadenv_spec_fuzzy_matching(self):
        """envy.loadenv_spec() supports fuzzy identity matching."""
        # Create bundle with helper
        bundle_path = self.create_bundle_with_helper(
            "acme.toolchain-helpers@v2",
            {"acme.dummy@v1": "specs/dummy.lua"},
            "lib/helper.lua",
            """HELPER_VERSION = "fuzzy-test"
function get_message()
  return "Fuzzy match worked"
end
""",
        )

        # Create spec that uses fuzzy matching (no version, no namespace)
        spec_content = f"""IDENTITY = "local.fuzzy-consumer@v1"
DEPENDENCIES = {{
  {{
    bundle = "acme.toolchain-helpers@v2",
    source = "{self.lua_path(bundle_path)}",
    needed_by = "check",
  }},
}}

USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      -- Fuzzy match: "toolchain-helpers" matches "acme.toolchain-helpers@v2"
      local helper = envy.loadenv_spec("toolchain-helpers", "lib.helper")
      return helper.HELPER_VERSION == "fuzzy-test"
    end,
    INSTALL = function(pkg_dir, options)
    end,
  }},
}}

"""
        spec_path = self.create_spec("fuzzy_consumer", spec_content)

        manifest = self.create_manifest(
            f"""
PACKAGES = {{
    {{ spec = "local.fuzzy-consumer@v1", source = "{self.lua_path(spec_path)}" }},
}}
"""
        )

        result = self.run_sync(manifest=manifest)

        self.assertEqual(
            result.returncode, 0, f"Fuzzy matching should work. stderr: {result.stderr}"
        )


class TestRequireInBundle(EnvyTestCase):
    """Tests for standard require() within bundles."""

    def setUp(self):
        super().setUp()
        self.test_dir = self.make_temp_dir("test_dir")
        self.bundle_dir = self.make_temp_dir("bundle_dir")

    @staticmethod
    def lua_path(path: Path) -> str:
        return path.as_posix()

    def create_manifest(self, content: str) -> Path:
        manifest_path = self.test_dir / "envy.lua"
        manifest_path.write_text(make_manifest(content), encoding="utf-8")
        return manifest_path

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

    def test_require_sibling_in_bundle(self):
        """Specs in bundles can require() sibling helper files."""
        # Create bundle structure:
        # /envy-bundle.lua
        # /specs/main.lua (uses require)
        # /lib/helper.lua (provides functions)

        specs_dir = self.bundle_dir / "specs"
        specs_dir.mkdir(parents=True, exist_ok=True)
        lib_dir = self.bundle_dir / "lib"
        lib_dir.mkdir(parents=True, exist_ok=True)

        # Bundle manifest
        bundle_lua = """BUNDLE = "test.require-bundle@v1"
SPECS = {
  ["test.main@v1"] = "specs/main.lua",
}
"""
        (self.bundle_dir / "envy-bundle.lua").write_text(bundle_lua)

        # Helper module
        helper_content = """local M = {}
M.VERSION = "2.0.0"
function M.get_greeting()
  return "Hello from lib"
end
return M
"""
        (lib_dir / "helper.lua").write_text(helper_content)

        # Main spec that uses require to load helper
        main_spec = """IDENTITY = "test.main@v1"
DEPENDENCIES = {}

-- This require should work because bundle root is in package.path
local helper = require("lib.helper")

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options)
      return helper.VERSION == "2.0.0"
    end,
    INSTALL = function(pkg_dir, options)
      envy.info("Helper version: " .. helper.VERSION)
    end,
  },
}

"""
        (specs_dir / "main.lua").write_text(main_spec)

        manifest = self.create_manifest(
            f"""
PACKAGES = {{
    {{
        spec = "test.main@v1",
        bundle = {{
            identity = "test.require-bundle@v1",
            source = "{self.lua_path(self.bundle_dir)}",
        }},
    }},
}}
"""
        )

        result = self.run_sync(manifest=manifest)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")


if __name__ == "__main__":
    unittest.main()
