"""Functional tests for bundle custom fetch functionality.

Tests bundles with custom fetch functions, including bundles
that have dependencies that must be installed before the fetch runs.
"""

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from . import test_config


class TestBundleCustomFetch(unittest.TestCase):
    """Tests for bundles with custom fetch functions."""

    def setUp(self):
        self.cache_root = Path(
            tempfile.mkdtemp(prefix="envy-bundle-custom-fetch-test-")
        )

    def tearDown(self):
        shutil.rmtree(self.cache_root, ignore_errors=True)

    def run_envy(self, args: list[str], cwd: str = None) -> subprocess.CompletedProcess:
        """Run envy with given arguments, pinning cache_root to a per-test temp dir."""
        exe = test_config.get_envy_executable()
        env = test_config.get_test_env()
        full_args = [str(exe), f"--cache-root={self.cache_root}"] + args
        return test_config.run(
            full_args, cwd=cwd, capture_output=True, text=True, env=env
        )

    def test_bundle_custom_fetch_simple(self):
        """Test bundle with simple custom fetch function (no dependencies)."""
        with tempfile.TemporaryDirectory() as tmpdir:
            bundle_dir = os.path.join(tmpdir, "bundle")
            os.makedirs(os.path.join(bundle_dir, "specs"))

            # Create bundle manifest
            bundle_lua = """
BUNDLE = "test.custom-fetch-bundle@v1"
SPECS = {
    ["test.simple@v1"] = "specs/simple.lua"
}
"""
            with open(os.path.join(bundle_dir, "envy-bundle.lua"), "w") as f:
                f.write(bundle_lua)

            # Create simple spec
            simple_spec = """
IDENTITY = "test.simple@v1"
USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir) return true end,
    INSTALL = function() end,
  },
}

"""
            with open(os.path.join(bundle_dir, "specs", "simple.lua"), "w") as f:
                f.write(simple_spec)

            # Escape path for Lua string (use forward slashes to avoid escaping issues)
            bundle_dir_lua = bundle_dir.replace("\\", "/")

            # Create manifest with custom fetch bundle
            manifest = f"""
-- @envy bin "tools"

BUNDLES = {{
    ["custom"] = {{
        identity = "test.custom-fetch-bundle@v1",
        source = {{
            fetch = function(tmp_dir)
                -- Simple fetch: copy bundle files
                if envy.PLATFORM == "windows" then
                    local src = '{bundle_dir_lua}'
                    envy.run('xcopy /E /I /Y "' .. src .. '" "' .. tmp_dir .. '"', {{shell = ENVY_SHELL.CMD}})
                else
                    envy.run("cp -r '{bundle_dir_lua}/'* " .. tmp_dir .. "/")
                end
                envy.commit_fetch({{"envy-bundle.lua", "specs"}})
            end,
            dependencies = {{}}
        }}
    }}
}}

PACKAGES = {{
    {{spec = "test.simple@v1", bundle = "custom"}}
}}
"""
            manifest_path = os.path.join(tmpdir, "envy.lua")
            with open(manifest_path, "w") as f:
                f.write(manifest)

            # Run install
            result = self.run_envy(
                ["install", "--manifest", manifest_path], tmpdir
            )

            # Should succeed - bundle custom fetch creates the bundle, then spec resolves
            self.assertEqual(
                result.returncode,
                0,
                f"stderr: {result.stderr}",
            )

    def test_bundle_custom_fetch_with_dependency(self):
        """Test bundle custom fetch with user-managed dependency."""
        with tempfile.TemporaryDirectory() as tmpdir:
            specs_dir = os.path.join(tmpdir, "specs")
            os.makedirs(specs_dir)

            # Create a user-managed tool spec that the bundle depends on
            tool_spec = """
IDENTITY = "local.fetcher-tool@v1"
USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir)
        return true  -- Always installed
    end,
    INSTALL = function()
        -- Tool installs (user-managed)
    end,
  },
}

"""
            tool_path = os.path.join(specs_dir, "fetcher-tool.lua")
            with open(tool_path, "w") as f:
                f.write(tool_spec)

            # Create bundle directory
            bundle_dir = os.path.join(tmpdir, "bundle")
            os.makedirs(os.path.join(bundle_dir, "specs"))

            bundle_lua = """
BUNDLE = "test.dep-bundle@v1"
SPECS = {
    ["test.from-dep-bundle@v1"] = "specs/from-dep.lua"
}
"""
            with open(os.path.join(bundle_dir, "envy-bundle.lua"), "w") as f:
                f.write(bundle_lua)

            from_dep_spec = """
IDENTITY = "test.from-dep-bundle@v1"
USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir) return true end,
    INSTALL = function() end,
  },
}

"""
            with open(os.path.join(bundle_dir, "specs", "from-dep.lua"), "w") as f:
                f.write(from_dep_spec)

            # Escape paths for Lua strings (use forward slashes to avoid escaping issues)
            bundle_dir_lua = bundle_dir.replace("\\", "/")
            tool_path_lua = tool_path.replace("\\", "/")

            # Create manifest with custom fetch bundle that has a dependency
            # Note: We don't use envy.package() since the tool is user-managed
            # The key test is that dependencies are resolved before the fetch runs
            manifest = f"""
-- @envy bin "tools"

BUNDLES = {{
    ["dep-bundle"] = {{
        identity = "test.dep-bundle@v1",
        source = {{
            fetch = function(tmp_dir)
                -- Dependencies are resolved before this runs (user-managed tool installed)
                -- Copy bundle files
                if envy.PLATFORM == "windows" then
                    local src = '{bundle_dir_lua}'
                    envy.run('xcopy /E /I /Y "' .. src .. '" "' .. tmp_dir .. '"', {{shell = ENVY_SHELL.CMD}})
                else
                    envy.run("cp -r '{bundle_dir_lua}/'* " .. tmp_dir .. "/")
                end
                envy.commit_fetch({{"envy-bundle.lua", "specs"}})
            end,
            dependencies = {{
                {{spec = "local.fetcher-tool@v1", source = "{tool_path_lua}"}}
            }}
        }}
    }}
}}

PACKAGES = {{
    {{spec = "test.from-dep-bundle@v1", bundle = "dep-bundle"}}
}}
"""
            manifest_path = os.path.join(tmpdir, "envy.lua")
            with open(manifest_path, "w") as f:
                f.write(manifest)

            # Run install
            result = self.run_envy(
                ["install", "--manifest", manifest_path], tmpdir
            )

            # Should succeed - dependency installs first, then custom fetch runs
            self.assertEqual(
                result.returncode,
                0,
                f"stderr: {result.stderr}",
            )

    def test_spec_declared_custom_fetch_bundle_supplies_spec(self):
        """A spec declares a custom-fetch bundle, then pulls a spec out of it.

        The fetch function lives in the declaring spec's Lua state, so the bundle
        package's parent must stay that spec. Consumers of the bundle are blocked on
        it and have no Lua state loaded, so re-parenting to one is fatal.
        """
        with tempfile.TemporaryDirectory() as tmpdir:
            root_spec = """
IDENTITY = "test.root@v1"

DEPENDENCIES = {
  { bundle = "test.tc@v1", source = { fetch = function(tmp_dir)
      local b = io.open(tmp_dir .. "/envy-bundle.lua", "w")
      b:write('BUNDLE = "test.tc@v1"\\n')
      b:write('SPECS = { ["test.tool@v1"] = "tool.lua" }\\n')
      b:close()
      local t = io.open(tmp_dir .. "/tool.lua", "w")
      t:write('IDENTITY = "test.tool@v1"\\n')
      t:write('DEPENDENCIES = {}\\n')
      t:write('FETCH = function(d, o) end\\n')
      t:close()
      envy.commit_fetch({"envy-bundle.lua", "tool.lua"})
    end } },
  { spec = "test.tool@v1", bundle = "test.tc@v1" },
}

FETCH = function(dir, options) end
"""
            spec_path = os.path.join(tmpdir, "root.lua")
            with open(spec_path, "w") as f:
                f.write(root_spec)

            manifest = """-- @envy bin "envy-bin"
PACKAGES = {
    { spec = "test.root@v1", source = "root.lua" },
}
"""
            manifest_path = os.path.join(tmpdir, "envy.lua")
            with open(manifest_path, "w") as f:
                f.write(manifest)

            result = self.run_envy(["install", "--manifest", manifest_path], tmpdir)

            self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
            self.assertNotIn("Lua state unavailable", result.stderr)
            # The bundle reported its own outcome row, like any other package.
            self.assertRegex(result.stderr, r"\[test\.tc@v1\] fetched \(\d+\.\ds\)")


if __name__ == "__main__":
    unittest.main()
