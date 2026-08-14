"""Functional tests for bundle dependencies in spec files.

Tests spec DEPENDENCIES with bundle references, including:
- Pure bundle dependencies (for envy.loadenv_spec())
- Spec-from-bundle dependencies
- BUNDLES table alias resolution
- Error cases
"""

import subprocess
import unittest
from pathlib import Path

from . import test_config
from .env import EnvyTestCase
from .test_config import make_manifest


class TestSpecBundleDependencies(EnvyTestCase):
    """Tests for spec files with bundle dependencies."""

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

    def create_bundle(self, bundle_identity: str, specs: dict[str, str]) -> Path:
        """Create a bundle directory with given identity and specs."""
        specs_dir = self.bundle_dir / "specs"
        specs_dir.mkdir(parents=True, exist_ok=True)

        specs_lua = ",\n".join(
            f'  ["{spec_id}"] = "{path}"' for spec_id, path in specs.items()
        )
        bundle_lua = f"""BUNDLE = "{bundle_identity}"
SPECS = {{
{specs_lua}
}}
"""
        (self.bundle_dir / "envy-bundle.lua").write_text(bundle_lua)

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

    def test_spec_with_pure_bundle_dependency(self):
        """Spec can depend on a bundle directly (for envy.loadenv_spec())."""
        bundle_path = self.create_bundle(
            "test.helper-bundle@v1", {"test.helper@v1": "specs/helper.lua"}
        )

        # Create spec that depends on the bundle
        spec_content = f"""IDENTITY = "local.consumer@v1"
DEPENDENCIES = {{
  {{
    bundle = "test.helper-bundle@v1",
    source = "{self.lua_path(bundle_path)}",
  }},
}}

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

        # Verify bundle was cached
        bundle_cache = self.cache_root / "specs" / "test.helper-bundle@v1"
        self.assertTrue(
            bundle_cache.exists(), f"Expected bundle cache at {bundle_cache}"
        )

    def test_spec_with_spec_from_bundle_dependency(self):
        """Spec can depend on a spec from a bundle."""
        bundle_path = self.create_bundle(
            "test.toolchain@v1", {"test.gcc@v1": "specs/gcc.lua"}
        )

        # Create spec that depends on a spec from the bundle
        spec_content = f"""IDENTITY = "local.app@v1"
DEPENDENCIES = {{
  {{
    spec = "test.gcc@v1",
    setup = {{ "main" }},
    bundle = {{
      identity = "test.toolchain@v1",
      source = "{self.lua_path(bundle_path)}",
    }},
  }},
}}

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
        spec_path = self.create_spec("app", spec_content)

        manifest = self.create_manifest(
            f"""
PACKAGES = {{
    {{ spec = "local.app@v1", source = "{self.lua_path(spec_path)}" }},
}}
"""
        )

        result = self.run_sync(manifest=manifest)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        # Verify both bundle and spec were processed
        bundle_cache = self.cache_root / "specs" / "test.toolchain@v1"
        self.assertTrue(bundle_cache.exists())
        pkg_cache = self.cache_root / "packages" / "test.gcc@v1"
        self.assertTrue(pkg_cache.exists(), f"Expected package at {pkg_cache}")

    def test_spec_with_bundles_table_alias(self):
        """Spec can use BUNDLES table for alias resolution."""
        bundle_path = self.create_bundle(
            "test.toolchain@v1",
            {"test.gcc@v1": "specs/gcc.lua", "test.clang@v1": "specs/clang.lua"},
        )

        # Create spec with BUNDLES table
        spec_content = f"""IDENTITY = "local.app@v1"

BUNDLES = {{
  tc = {{
    identity = "test.toolchain@v1",
    source = "{self.lua_path(bundle_path)}",
  }},
}}

DEPENDENCIES = {{
  {{ spec = "test.gcc@v1", bundle = "tc", setup = {{ "main" }} }},
  {{ spec = "test.clang@v1", bundle = "tc", setup = {{ "main" }} }},
}}

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
        spec_path = self.create_spec("app", spec_content)

        manifest = self.create_manifest(
            f"""
PACKAGES = {{
    {{ spec = "local.app@v1", source = "{self.lua_path(spec_path)}" }},
}}
"""
        )

        result = self.run_sync(manifest=manifest)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        # Verify both specs from bundle were processed
        gcc_pkg = self.cache_root / "packages" / "test.gcc@v1"
        clang_pkg = self.cache_root / "packages" / "test.clang@v1"
        self.assertTrue(gcc_pkg.exists())
        self.assertTrue(clang_pkg.exists())

    def test_spec_reference_bundle_by_identity(self):
        """Spec can reference bundle by identity after declaring pure bundle dep."""
        bundle_path = self.create_bundle(
            "test.toolchain@v1", {"test.gcc@v1": "specs/gcc.lua"}
        )

        # Create spec that declares bundle first, then references by identity
        spec_content = f"""IDENTITY = "local.app@v1"

DEPENDENCIES = {{
  -- First declare the bundle (pure bundle dep)
  {{
    bundle = "test.toolchain@v1",
    source = "{self.lua_path(bundle_path)}",
  }},
  -- Then reference by bundle identity
  {{
    spec = "test.gcc@v1",
    bundle = "test.toolchain@v1",
  }},
}}

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
        spec_path = self.create_spec("app", spec_content)

        manifest = self.create_manifest(
            f"""
PACKAGES = {{
    {{ spec = "local.app@v1", source = "{self.lua_path(spec_path)}" }},
}}
"""
        )

        result = self.run_sync(manifest=manifest)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")


class TestBundleDependencyErrors(EnvyTestCase):
    """Tests for bundle dependency error cases."""

    def setUp(self):
        super().setUp()
        self.test_dir = self.make_temp_dir("test_dir")

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

    def test_unknown_bundle_alias_in_spec_errors(self):
        """Spec referencing unknown bundle alias produces error."""
        spec_content = """IDENTITY = "local.app@v1"

DEPENDENCIES = {
  { spec = "test.gcc@v1", bundle = "unknown_alias" },
}

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
        spec_path = self.create_spec("app", spec_content)

        manifest = self.create_manifest(
            f"""
PACKAGES = {{
    {{ spec = "local.app@v1", source = "{self.lua_path(spec_path)}" }},
}}
"""
        )

        result = self.run_sync(manifest=manifest)

        self.assertNotEqual(result.returncode, 0, "Expected non-zero exit code")
        self.assertIn("unknown_alias", result.stderr.lower())
        self.assertIn("not found", result.stderr.lower())

    def test_bundle_dep_without_spec_or_source_errors(self):
        """Bundle dependency without spec or source produces error."""
        spec_content = """IDENTITY = "local.app@v1"

DEPENDENCIES = {
  { bundle = "some.bundle@v1" },  -- No source, no spec - invalid
}

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
        spec_path = self.create_spec("app", spec_content)

        manifest = self.create_manifest(
            f"""
PACKAGES = {{
    {{ spec = "local.app@v1", source = "{self.lua_path(spec_path)}" }},
}}
"""
        )

        result = self.run_sync(manifest=manifest)

        self.assertNotEqual(result.returncode, 0, "Expected non-zero exit code")
        # Should fail because bundle = "some.bundle@v1" without source and without
        # being a known alias/identity


if __name__ == "__main__":
    unittest.main()
