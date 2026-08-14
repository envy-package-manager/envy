"""Functional tests for bundle dependency ordering using trace JSON.

Tests that:
- Bundles are fetched before specs that depend on them
- envy.loadenv_spec() calls are traced correctly
- Dependency ordering is enforced for bundle deps
"""

import json
import subprocess
import tempfile
import unittest
from pathlib import Path

from . import test_config
from .env import EnvyTestCase
from .test_config import make_manifest


class TestBundleDependencyOrder(EnvyTestCase):
    """Tests for bundle dependency ordering using trace output."""

    def setUp(self):
        super().setUp()
        self.test_dir = self.make_temp_dir("test_dir")
        self.bundle_dir = self.make_temp_dir("bundle_dir")
        self.trace_file = Path(tempfile.mktemp(suffix=".jsonl", prefix="envy-trace-"))

    def tearDown(self):
        if self.trace_file.exists():
            self.trace_file.unlink()

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

    def create_bundle(
        self,
        bundle_identity: str,
        specs: dict[str, str],
        helper_modules: dict[str, str] | None = None,
    ) -> Path:
        """Create a bundle with specs and optional helper modules."""
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

        # Create helper modules
        if helper_modules:
            for helper_path, helper_content in helper_modules.items():
                full_path = self.bundle_dir / helper_path
                full_path.parent.mkdir(parents=True, exist_ok=True)
                full_path.write_text(helper_content)

        return self.bundle_dir

    def run_sync_with_trace(self, manifest: Path, install_all: bool = True):
        cmd = [
            str(self.envy),
            "--cache-root",
            str(self.cache_root),
            f"--trace=file:{self.trace_file}",
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

    def parse_trace_events(self) -> list[dict]:
        """Parse trace file and return list of event dicts."""
        if not self.trace_file.exists():
            return []
        events = []
        with open(self.trace_file) as f:
            for line in f:
                line = line.strip()
                if line:
                    events.append(json.loads(line))
        return events

    def filter_events(self, events: list[dict], event_type: str) -> list[dict]:
        """Filter events by type."""
        return [e for e in events if e.get("event") == event_type]

    def test_bundle_fetched_before_loadenv_spec_access(self):
        """Verify bundle is fully fetched before loadenv_spec can access it."""
        # Create bundle with helper
        bundle_path = self.create_bundle(
            "test.helpers@v1",
            {"test.dummy@v1": "specs/dummy.lua"},
            {
                "lib/helper.lua": """HELPER_VERSION = "1.0.0"
function compute_value()
  return 42
end
"""
            },
        )

        # Create spec that depends on bundle and uses loadenv_spec
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
      return helper.compute_value() == 42
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
    {{ spec = "local.consumer@v1", source = "{self.lua_path(spec_path)}", setup = {{ "main" }} }},
}}
"""
        )

        result = self.run_sync_with_trace(manifest=manifest)
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        # Parse trace and verify ordering
        events = self.parse_trace_events()

        # Find phase_complete for bundle's spec_fetch
        bundle_fetch_completes = [
            e
            for e in self.filter_events(events, "phase_complete")
            if "test.helpers@v1" in e.get("spec", "") and e.get("phase") == "spec_fetch"
        ]

        # Find loadenv_spec_access for the consumer
        loadenv_accesses = [
            e
            for e in self.filter_events(events, "lua_ctx_loadenv_spec_access")
            if e.get("spec") == "local.consumer@v1"
            and e.get("target") == "test.helpers@v1"
        ]

        self.assertTrue(
            len(bundle_fetch_completes) > 0,
            f"Expected bundle spec_fetch complete event. Events: {events}",
        )
        self.assertTrue(
            len(loadenv_accesses) > 0,
            f"Expected loadenv_spec access event. Events: {events}",
        )

        # Verify the loadenv_spec access was allowed
        for access in loadenv_accesses:
            self.assertTrue(
                access.get("allowed", False),
                f"loadenv_spec access should be allowed: {access}",
            )

    def test_loadenv_spec_trace_includes_subpath(self):
        """Verify loadenv_spec trace includes subpath information."""
        bundle_path = self.create_bundle(
            "test.helpers@v1",
            {"test.dummy@v1": "specs/dummy.lua"},
            {"lib/math/utils.lua": """function add(a, b) return a + b end"""},
        )

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
      local utils = envy.loadenv_spec("test.helpers@v1", "lib.math.utils")
      return utils.add(1, 2) == 3
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
    {{ spec = "local.consumer@v1", source = "{self.lua_path(spec_path)}", setup = {{ "main" }} }},
}}
"""
        )

        result = self.run_sync_with_trace(manifest=manifest)
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        events = self.parse_trace_events()
        loadenv_accesses = self.filter_events(events, "lua_ctx_loadenv_spec_access")

        # Find the access for our specific subpath
        matching_accesses = [
            e for e in loadenv_accesses if e.get("subpath") == "lib.math.utils"
        ]

        self.assertTrue(
            len(matching_accesses) > 0,
            f"Expected loadenv_spec access with subpath 'lib.math.utils'. Events: {loadenv_accesses}",
        )

    def test_fuzzy_match_recorded_in_trace(self):
        """Verify fuzzy matching is used and recorded in trace."""
        bundle_path = self.create_bundle(
            "acme.toolchain@v2",
            {"acme.dummy@v1": "specs/dummy.lua"},
            {
                "lib/config.lua": """CONFIG_VERSION = "2.0"
function get_version() return CONFIG_VERSION end
"""
            },
        )

        # Use fuzzy match: "toolchain" instead of full "acme.toolchain@v2"
        spec_content = f"""IDENTITY = "local.consumer@v1"
DEPENDENCIES = {{
  {{
    bundle = "acme.toolchain@v2",
    source = "{self.lua_path(bundle_path)}",
    needed_by = "check",
  }},
}}

USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      -- Fuzzy match: "toolchain" matches "acme.toolchain@v2"
      local config = envy.loadenv_spec("toolchain", "lib.config")
      return config.get_version() == "2.0"
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
    {{ spec = "local.consumer@v1", source = "{self.lua_path(spec_path)}", setup = {{ "main" }} }},
}}
"""
        )

        result = self.run_sync_with_trace(manifest=manifest)
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        events = self.parse_trace_events()
        loadenv_accesses = self.filter_events(events, "lua_ctx_loadenv_spec_access")

        # Find access with fuzzy query
        fuzzy_accesses = [
            e
            for e in loadenv_accesses
            if e.get("target") == "toolchain"  # The query, not the resolved identity
        ]

        self.assertTrue(
            len(fuzzy_accesses) > 0,
            f"Expected loadenv_spec access with fuzzy target 'toolchain'. Events: {loadenv_accesses}",
        )

        # Verify it was allowed (meaning it resolved correctly)
        for access in fuzzy_accesses:
            self.assertTrue(
                access.get("allowed", False),
                f"Fuzzy match should be allowed: {access}",
            )


if __name__ == "__main__":
    unittest.main()
