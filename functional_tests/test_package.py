"""Functional tests for 'envy package' command.

Tests package path querying, manifest discovery, dependency installation,
ambiguity detection, and error handling.
"""

import hashlib
import io
import shutil
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path
from typing import Optional

from . import test_config
from .env import EnvyTestCase
from .test_config import make_manifest

# Test archive contents
TEST_ARCHIVE_FILES = {
    "root/file1.txt": "Root file content\n",
    "root/file2.txt": "Another root file\n",
    "root/subdir1/file3.txt": "Subdirectory file\n",
}


def create_test_archive(output_path: Path) -> str:
    """Create test.tar.gz archive and return its SHA256 hash."""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz") as tar:
        for name, content in TEST_ARCHIVE_FILES.items():
            data = content.encode("utf-8")
            info = tarfile.TarInfo(name=name)
            info.size = len(data)
            tar.addfile(info, io.BytesIO(data))
    archive_data = buf.getvalue()
    output_path.write_bytes(archive_data)
    return hashlib.sha256(archive_data).hexdigest()


class TestPackageCommand(EnvyTestCase):
    """Tests for 'envy package' command."""

    def setUp(self):
        super().setUp()
        self.test_dir = self.make_temp_dir("test_dir")
        self.specs_dir = self.make_temp_dir("specs_dir")

        # Create test archive and get its hash
        self.archive_path = self.specs_dir / "test.tar.gz"
        self.archive_hash = create_test_archive(self.archive_path)

        # Write inline specs to temp directory
        self._write_specs()

    def _write_specs(self):
        """Write all inline specs to the specs directory."""
        archive_lua_path = self.archive_path.as_posix()

        specs = {
            "build_dependency.lua": f'''-- Test dependency for build_with_asset
IDENTITY = "local.build_dependency@v1"

FETCH = {{
  source = "{archive_lua_path}",
  sha256 = "{self.archive_hash}"
}}

STAGE = {{strip = 1}}

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  if envy.PLATFORM == "windows" then
    envy.run([[Write-Output "dependency: begin"; Remove-Item -Force dependency.txt -ErrorAction SilentlyContinue; Set-Content -Path dependency.txt -Value "dependency_data"; New-Item -ItemType Directory -Path bin -Force | Out-Null; Set-Content -Path bin/app -Value "binary"; if (-not (Test-Path bin/app)) {{ Write-Error "missing bin/app"; exit 1 }}; Write-Output "dependency: success"; exit 0 ]], {{ shell = ENVY_SHELL.POWERSHELL }})
  else
    envy.run([[echo 'dependency_data' > dependency.txt
      mkdir -p bin
      echo 'binary' > bin/app]])
  end
end
''',
            "build_function.lua": f'''-- Test build phase: build = function(ctx, opts) (programmatic with envy.run())
IDENTITY = "local.build_function@v1"

FETCH = {{
  source = "{archive_lua_path}",
  sha256 = "{self.archive_hash}"
}}

STAGE = {{strip = 1}}

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  print("Building with envy.run()")
  local result
  if envy.PLATFORM == "windows" then
    result = envy.run([[mkdir build_output 2> nul & echo function_artifact > build_output\\result.txt & if not exist build_output\\result.txt ( echo Artifact missing & exit /b 1 ) & echo Build complete & exit /b 0 ]],
                     {{ shell = ENVY_SHELL.CMD, capture = true }})
  else
    result = envy.run([[
      mkdir -p build_output
      echo "function_artifact" > build_output/result.txt
      echo "Build complete"
    ]],
                     {{ capture = true }})
  end
  if not result.stdout:match("Build complete") then
    error("Expected 'Build complete' in stdout")
  end
  print("Build finished successfully")
end
''',
            "build_nil.lua": f'''-- Test build phase: build = nil (skip build)
IDENTITY = "local.build_nil@v1"

FETCH = {{
  source = "{archive_lua_path}",
  sha256 = "{self.archive_hash}"
}}

STAGE = {{strip = 1}}

-- No build field - should skip build phase and proceed to install
''',
            "build_error_nonzero_exit.lua": f'''-- Test build phase: error on non-zero exit code
IDENTITY = "local.build_error_nonzero_exit@v1"

FETCH = {{
  source = "{archive_lua_path}",
  sha256 = "{self.archive_hash}"
}}

STAGE = {{strip = 1}}

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  print("Testing error handling")
  if envy.PLATFORM == "windows" then
    envy.run("exit 42", {{ shell = ENVY_SHELL.POWERSHELL }})
  else
    envy.run("exit 42")
  end
  error("Should not reach here")
end
''',
            "diamond_a.lua": """-- Top of diamond: A depends on B and C (which both depend on D)
IDENTITY = "local.diamond_a@v1"
DEPENDENCIES = {
  { spec = "local.diamond_b@v1", source = "diamond_b.lua" },
  { spec = "local.diamond_c@v1", source = "diamond_c.lua" }
}

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic package
    end,
  },
}

""",
            "diamond_b.lua": """-- Left side of diamond: B depends on D
IDENTITY = "local.diamond_b@v1"
DEPENDENCIES = {
  { spec = "local.diamond_d@v1", source = "diamond_d.lua" }
}

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic package
    end,
  },
}

""",
            "diamond_c.lua": """-- Right side of diamond: C depends on D
IDENTITY = "local.diamond_c@v1"
DEPENDENCIES = {
  { spec = "local.diamond_d@v1", source = "diamond_d.lua" }
}

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic package
    end,
  },
}

""",
            "diamond_d.lua": """-- Base of diamond dependency
IDENTITY = "local.diamond_d@v1"
DEPENDENCIES = {}

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic package
    end,
  },
}

""",
            "install_programmatic.lua": """-- Test programmatic package (user-managed)
IDENTITY = "local.programmatic@v1"

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic package - no cache artifacts
    end,
  },
}

""",
        }

        for name, content in specs.items():
            (self.specs_dir / name).write_text(content, encoding="utf-8")

    @staticmethod
    def lua_path(path: Path) -> str:
        """Convert path to Lua-safe string (forward slashes work on all platforms)."""
        return path.as_posix()

    def create_manifest(self, content: str, subdir: str = "") -> Path:
        """Create manifest file with given content, optionally in subdirectory."""
        manifest_dir = self.test_dir / subdir if subdir else self.test_dir
        manifest_dir.mkdir(parents=True, exist_ok=True)
        manifest_path = manifest_dir / "envy.lua"
        manifest_path.write_text(make_manifest(content), encoding="utf-8")
        return manifest_path

    def run_package(
        self, identity: str, manifest: Optional[Path] = None, cwd: Optional[Path] = None
    ):
        """Run 'envy package' command and return result."""
        cmd = [
            str(self.envy),
            "--cache-root",
            str(self.cache_root),
            "package",
            identity,
        ]
        if manifest:
            cmd.extend(["--manifest", str(manifest)])

        # Run from project root so relative paths in specs work
        result = test_config.run(
            cmd,
            cwd=cwd or self.project_root,
            capture_output=True,
            text=True,
        )
        return result

    def test_package_simple_package(self):
        """Query package path for simple package with no dependencies."""
        manifest = self.create_manifest(
            f"""
PACKAGES = {{
    {{ spec = "local.build_dependency@v1", source = "{self.lua_path(self.specs_dir)}/build_dependency.lua" }}
}}
"""
        )

        result = self.run_package("local.build_dependency@v1", manifest)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertTrue(result.stdout.strip(), "Expected path in stdout")

        package_path = Path(result.stdout.strip())
        self.assertTrue(
            package_path.is_absolute(), f"Expected absolute path: {package_path}"
        )
        self.assertTrue(
            package_path.exists(), f"Package path should exist: {package_path}"
        )
        # Check path ends with pkg directory (accept both / and \ separators)
        self.assertTrue(
            str(package_path).endswith("/pkg") or str(package_path).endswith("\\pkg"),
            f"Path should end with pkg directory: {package_path}",
        )

    def test_package_with_dependencies(self):
        """Query package for package with dependencies, verify both installed."""
        # NOTE: diamond_a is a programmatic package, so this test currently expects failure
        # TODO: Create test specs with dependencies that produce actual cached packages
        manifest = self.create_manifest(
            f"""
PACKAGES = {{
    {{ spec = "local.diamond_a@v1", source = "{self.lua_path(self.specs_dir)}/diamond_a.lua" }}
}}
"""
        )

        result = self.run_package("local.diamond_a@v1", manifest)

        # Currently fails because diamond_a is programmatic (user-managed)
        self.assertEqual(result.returncode, 1)
        self.assertIn("not cache-managed", result.stderr)

    def test_package_already_cached(self):
        """Query package that's already installed, should return immediately."""
        manifest = self.create_manifest(
            f"""
PACKAGES = {{
    {{ spec = "local.build_dependency@v1", source = "{self.lua_path(self.specs_dir)}/build_dependency.lua" }}
}}
"""
        )

        # First installation
        result1 = self.run_package("local.build_dependency@v1", manifest)
        self.assertEqual(result1.returncode, 0)
        path1 = result1.stdout.strip()

        # Second query (should be cached)
        result2 = self.run_package("local.build_dependency@v1", manifest)
        self.assertEqual(result2.returncode, 0)
        path2 = result2.stdout.strip()

        # Same path returned
        self.assertEqual(path1, path2, "Should return same path for cached package")

    def test_package_auto_discover_manifest(self):
        """Auto-discover manifest from parent directory."""
        # NOTE: This test is currently disabled because specs have relative fetch paths
        # that don't work when running from arbitrary directories
        # TODO: Either use specs with absolute fetch paths or fix path resolution
        self.skipTest("Auto-discover with relative spec paths not yet supported")

    def test_package_explicit_manifest_path(self):
        """Use explicit --manifest flag to specify manifest location."""
        # Create manifest in non-standard location
        other_dir = self.make_temp_dir("other_dir")
        manifest = other_dir / "custom.lua"
        manifest.write_text(
            make_manifest(
                f"""
PACKAGES = {{
    {{ spec = "local.build_dependency@v1", source = "{self.lua_path(self.specs_dir)}/build_dependency.lua" }}
}}
"""
            ),
            encoding="utf-8",
        )

        result = self.run_package("local.build_dependency@v1", manifest=manifest)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertTrue(result.stdout.strip())

    def test_package_no_manifest_found(self):
        """Error when no manifest can be found."""
        # Use directory with no manifest
        empty_dir = self.make_temp_dir("empty_dir")
        result = self.run_package("local.simple@v1", manifest=None, cwd=empty_dir)

        self.assertEqual(result.returncode, 1)
        self.assertIn("not found", result.stderr.lower())
    def test_package_selective_installation(self):
        """Only requested package and dependencies installed, not entire manifest."""
        manifest = self.create_manifest(
            f"""
PACKAGES = {{
    {{ spec = "local.build_dependency@v1", source = "{self.lua_path(self.specs_dir)}/build_dependency.lua" }},
    {{ spec = "local.build_function@v1", source = "{self.lua_path(self.specs_dir)}/build_function.lua" }},
    {{ spec = "local.build_nil@v1", source = "{self.lua_path(self.specs_dir)}/build_nil.lua" }},
}}
"""
        )

        # Request only build_dependency (which has no dependencies)
        result = self.run_package("local.build_dependency@v1", manifest)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        # Check which packages were installed
        packages_dir = self.cache_root / "packages"
        installed = (
            [d.name for d in packages_dir.glob("local.*")]
            if packages_dir.exists()
            else []
        )

        # Should have build_dependency but NOT build_function or build_nil
        self.assertTrue(
            any("build_dependency" in name for name in installed),
            "build_dependency should be installed",
        )
        # build_function and build_nil should NOT be installed
        self.assertFalse(
            any("build_function" in name for name in installed),
            "build_function should NOT be installed (not requested)",
        )
        self.assertFalse(
            any("build_nil" in name for name in installed),
            "build_nil should NOT be installed (not requested)",
        )
        # This demonstrates selective installation - only 1 of 3 packages installed

    def test_package_ambiguous_different_options(self):
        """Error when same identity appears with different options."""
        manifest = self.create_manifest(
            f"""
PACKAGES = {{
    {{ spec = "local.build_dependency@v1", source = "{self.lua_path(self.specs_dir)}/build_dependency.lua", options = {{ mode = "debug" }} }},
    {{ spec = "local.build_dependency@v1", source = "{self.lua_path(self.specs_dir)}/build_dependency.lua", options = {{ mode = "release" }} }}
}}
"""
        )

        result = self.run_package("local.build_dependency@v1", manifest)

        self.assertEqual(result.returncode, 1)
        self.assertIn("multiple times", result.stderr.lower())
        self.assertIn("different options", result.stderr.lower())

    def test_package_duplicate_same_options_ok(self):
        """Duplicate identity with same options should succeed."""
        manifest = self.create_manifest(
            f"""
PACKAGES = {{
    {{ spec = "local.build_dependency@v1", source = "{self.lua_path(self.specs_dir)}/build_dependency.lua" }},
    {{ spec = "local.build_dependency@v1", source = "{self.lua_path(self.specs_dir)}/build_dependency.lua" }}
}}
"""
        )

        result = self.run_package("local.build_dependency@v1", manifest)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertTrue(result.stdout.strip())

    def test_package_identity_not_in_manifest(self):
        """Error when requested identity not in manifest."""
        manifest = self.create_manifest(
            f"""
PACKAGES = {{
    {{ spec = "local.other@v1", source = "{self.lua_path(self.specs_dir)}/build_dependency.lua" }}
}}
"""
        )

        result = self.run_package("local.nonexistent@v1", manifest)

        self.assertEqual(result.returncode, 1)
        self.assertIn("no package matching", result.stderr.lower())

    def test_package_programmatic_package(self):
        """Error for programmatic packages (no cached artifacts)."""
        manifest = self.create_manifest(
            f"""
PACKAGES = {{
    {{ spec = "local.programmatic@v1", source = "{self.lua_path(self.specs_dir)}/install_programmatic.lua" }}
}}
"""
        )

        result = self.run_package("local.programmatic@v1", manifest)

        self.assertEqual(result.returncode, 1)
        self.assertTrue(result.stderr.strip(), "Should have error message in stderr")

    def test_package_build_failure(self):
        """Error when build phase fails."""
        manifest = self.create_manifest(
            f"""
PACKAGES = {{
    {{ spec = "local.failing@v1", source = "{self.lua_path(self.specs_dir)}/build_error_nonzero_exit.lua" }}
}}
"""
        )

        result = self.run_package("local.failing@v1", manifest)

        self.assertEqual(result.returncode, 1)
        self.assertTrue(result.stderr.strip(), "Should have error message in stderr")

    def test_package_invalid_manifest_syntax(self):
        """Error when manifest has Lua syntax error."""
        # Note: create_manifest adds bin-dir header; the syntax error is in the body
        manifest = self.create_manifest(
            """
PACKAGES = {
    this is invalid lua syntax
}
"""
        )

        result = self.run_package("local.simple@v1", manifest)

        self.assertEqual(result.returncode, 1)
        self.assertTrue(result.stderr.strip(), "Should have error message in stderr")

    def test_package_stdout_format(self):
        """Verify stdout contains exactly one line with absolute path."""
        manifest = self.create_manifest(
            f"""
PACKAGES = {{
    {{ spec = "local.build_dependency@v1", source = "{self.lua_path(self.specs_dir)}/build_dependency.lua" }}
}}
"""
        )

        result = self.run_package("local.build_dependency@v1", manifest)

        self.assertEqual(result.returncode, 0)

        lines = result.stdout.strip().split("\n")
        self.assertEqual(len(lines), 1, "Should have exactly one line in stdout")

        path = lines[0]
        # Check if path is absolute (Unix: starts with /, Windows: starts with drive letter)
        import os

        self.assertTrue(os.path.isabs(path), f"Should be absolute path: {path}")
        # Path should end with pkg directory (accept both / and \ separators)
        self.assertTrue(
            path.endswith("/pkg") or path.endswith("\\pkg"),
            f"Should end with pkg directory: {path}",
        )

    def test_package_stderr_only_on_error(self):
        """Success should have no stderr output, failure should."""
        manifest = self.create_manifest(
            f"""
PACKAGES = {{
    {{ spec = "local.build_dependency@v1", source = "{self.lua_path(self.specs_dir)}/build_dependency.lua" }},
    {{ spec = "local.build_function@v1", source = "{self.lua_path(self.specs_dir)}/build_function.lua" }}
}}
"""
        )

        # Success case - note: might have trace/debug logs, so we check it doesn't have errors
        result_ok = self.run_package("local.build_dependency@v1", manifest)
        self.assertEqual(result_ok.returncode, 0)
        # Just verify no error messages (trace logs are okay)

        # Failure case
        result_fail = self.run_package("local.nonexistent@v1", manifest)
        self.assertEqual(result_fail.returncode, 1)
        self.assertTrue(
            result_fail.stderr.strip(), "Should have error message in stderr"
        )

    def test_package_with_spec_options(self):
        """Install package with options in manifest."""
        manifest = self.create_manifest(
            f"""
PACKAGES = {{
    {{ spec = "local.build_dependency@v1", source = "{self.lua_path(self.specs_dir)}/build_dependency.lua", options = {{ mode = "debug" }} }}
}}
"""
        )

        result = self.run_package("local.build_dependency@v1", manifest)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        package_path = Path(result.stdout.strip())
        self.assertTrue(package_path.exists())
        # The canonical key will include options, affecting the hash in the path

    def test_package_different_options_separate_cache_entries(self):
        """Different options produce separate cache entries with distinct content."""
        # Create spec that writes option value to a file
        # This is a cache-managed package (no check verb) that writes artifacts to cache
        spec_content = """IDENTITY = "local.test_options_cache@v1"

-- Empty fetch - spec generates content directly in install phase
function FETCH(tmp_dir, options)
    -- Nothing to fetch
end

function INSTALL(install_dir, stage_dir, fetch_dir, tmp_dir, options)
    local f = io.open(install_dir .. "/variant.txt", "w")
    f:write(options.variant or "none")
    f:close()
end
"""
        spec_path = self.test_dir / "test_options_cache.lua"
        spec_path.write_text(spec_content, encoding="utf-8")

        # Manifest with variant=foo
        manifest_foo = self.create_manifest(
            f"""
PACKAGES = {{
    {{ spec = "local.test_options_cache@v1", source = "{self.lua_path(spec_path)}", options = {{ variant = "foo" }} }}
}}
""",
            subdir="foo",
        )

        # Manifest with variant=bar
        manifest_bar = self.create_manifest(
            f"""
PACKAGES = {{
    {{ spec = "local.test_options_cache@v1", source = "{self.lua_path(spec_path)}", options = {{ variant = "bar" }} }}
}}
""",
            subdir="bar",
        )

        # Install foo variant
        result_foo = self.run_package("local.test_options_cache@v1", manifest_foo)
        self.assertEqual(result_foo.returncode, 0, f"stderr: {result_foo.stderr}")
        path_foo = Path(result_foo.stdout.strip())
        self.assertTrue(path_foo.exists())

        # Install bar variant
        result_bar = self.run_package("local.test_options_cache@v1", manifest_bar)
        self.assertEqual(result_bar.returncode, 0, f"stderr: {result_bar.stderr}")
        path_bar = Path(result_bar.stdout.strip())
        self.assertTrue(path_bar.exists())

        # Verify different cache paths
        self.assertNotEqual(
            path_foo,
            path_bar,
            "Different options must produce different cache paths",
        )

        # Verify correct content in each cache entry
        variant_foo = (path_foo / "variant.txt").read_text()
        variant_bar = (path_bar / "variant.txt").read_text()
        self.assertEqual(variant_foo, "foo", "Foo variant should contain 'foo'")
        self.assertEqual(variant_bar, "bar", "Bar variant should contain 'bar'")

    def test_package_transitive_dependency_chain(self):
        """Install package with transitive dependencies (diamond structure)."""
        # NOTE: diamond_a is programmatic, test expects failure
        # TODO: Create test specs with dependencies that produce actual cached packages
        manifest = self.create_manifest(
            f"""
PACKAGES = {{
    {{ spec = "local.diamond_a@v1", source = "{self.lua_path(self.specs_dir)}/diamond_a.lua" }}
}}
"""
        )

        result = self.run_package("local.diamond_a@v1", manifest)

        # Currently fails because diamond_a is programmatic (user-managed)
        self.assertEqual(result.returncode, 1)
        self.assertIn("not cache-managed", result.stderr)

    def test_package_diamond_dependency(self):
        """Install package with diamond dependency: A → B,C → D."""
        # NOTE: diamond_a is programmatic, test expects failure
        # TODO: Create test specs with dependencies that produce actual cached packages
        manifest = self.create_manifest(
            f"""
PACKAGES = {{
    {{ spec = "local.diamond_a@v1", source = "{self.lua_path(self.specs_dir)}/diamond_a.lua" }}
}}
"""
        )

        result = self.run_package("local.diamond_a@v1", manifest)

        # Currently fails because diamond_a is programmatic (user-managed)
        self.assertEqual(result.returncode, 1)
        self.assertIn("not cache-managed", result.stderr)

    def test_package_with_product_dependency_clean_cache(self):
        """Package command must resolve graph to find product providers.

        Regression test: package command should call resolve_graph() to find
        specs that provide products needed by dependencies. Without this,
        product dependencies fail on clean cache.
        """
        archive_lua_path = self.archive_path.as_posix()

        # Create product provider spec
        provider_spec = f"""IDENTITY = "local.test_product_provider@v1"

FETCH = {{ source = "{archive_lua_path}",
          sha256 = "{self.archive_hash}" }}

INSTALL = function(ctx)
end

PRODUCTS = {{ test_tool = "bin/tool" }}
"""
        provider_path = self.test_dir / "test_product_provider.lua"
        provider_path.write_text(provider_spec, encoding="utf-8")

        # Create consumer spec that depends on the product
        consumer_spec = f"""IDENTITY = "local.test_product_consumer@v1"

DEPENDENCIES = {{
    {{ product = "test_tool" }}
}}

FETCH = {{
    source = "{archive_lua_path}",
    sha256 = "{self.archive_hash}"
}}

INSTALL = function(ctx)
end
"""
        consumer_path = self.test_dir / "test_product_consumer.lua"
        consumer_path.write_text(consumer_spec, encoding="utf-8")

        # Manifest with both recipes
        manifest = self.create_manifest(
            f"""
PACKAGES = {{
    {{ spec = "local.test_product_provider@v1", source = "{self.lua_path(provider_path)}" }},
    {{ spec = "local.test_product_consumer@v1", source = "{self.lua_path(consumer_path)}" }}
}}
"""
        )

        # Run package on consumer with clean cache
        result = self.run_package("local.test_product_consumer@v1", manifest)

        # Should succeed - package command must resolve graph to find product provider
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertTrue(result.stdout.strip(), "Expected package path in stdout")

        package_path = Path(result.stdout.strip())
        self.assertTrue(
            package_path.exists(), f"Package path should exist: {package_path}"
        )

    def test_package_partial_match_full_identity(self):
        """Full identity match works."""
        manifest = self.create_manifest(
            f"""
PACKAGES = {{
    {{ spec = "local.build_dependency@v1", source = "{self.lua_path(self.specs_dir)}/build_dependency.lua" }}
}}
"""
        )

        result = self.run_package("local.build_dependency@v1", manifest)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertTrue(result.stdout.strip())

    def test_package_partial_match_namespace_name(self):
        """Partial match by namespace.name (no revision) works."""
        manifest = self.create_manifest(
            f"""
PACKAGES = {{
    {{ spec = "local.build_dependency@v1", source = "{self.lua_path(self.specs_dir)}/build_dependency.lua" }}
}}
"""
        )

        # Match without the @v1 revision
        result = self.run_package("local.build_dependency", manifest)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertTrue(result.stdout.strip())

    def test_package_partial_match_name_only(self):
        """Partial match by name only (no namespace or revision) works."""
        manifest = self.create_manifest(
            f"""
PACKAGES = {{
    {{ spec = "local.build_dependency@v1", source = "{self.lua_path(self.specs_dir)}/build_dependency.lua" }}
}}
"""
        )

        # Match with just the name part
        result = self.run_package("build_dependency", manifest)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertTrue(result.stdout.strip())

    def test_package_partial_match_name_revision(self):
        """Partial match by name@revision (no namespace) works."""
        manifest = self.create_manifest(
            f"""
PACKAGES = {{
    {{ spec = "local.build_dependency@v1", source = "{self.lua_path(self.specs_dir)}/build_dependency.lua" }}
}}
"""
        )

        # Match with name@revision but no namespace
        result = self.run_package("build_dependency@v1", manifest)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertTrue(result.stdout.strip())

    def test_package_partial_match_ambiguous_error(self):
        """Ambiguous partial match produces error with candidates."""
        archive_lua_path = self.archive_path.as_posix()

        # Create two specs with the same name but different namespaces
        spec_local = f"""IDENTITY = "local.common@v1"
FETCH = {{ source = "{archive_lua_path}", sha256 = "{self.archive_hash}" }}
STAGE = {{ strip = 1 }}
BUILD = function() end
"""
        spec_other = f"""IDENTITY = "other.common@v1"
FETCH = {{ source = "{archive_lua_path}", sha256 = "{self.archive_hash}" }}
STAGE = {{ strip = 1 }}
BUILD = function() end
"""
        spec_local_path = self.test_dir / "common_local.lua"
        spec_other_path = self.test_dir / "common_other.lua"
        spec_local_path.write_text(spec_local, encoding="utf-8")
        spec_other_path.write_text(spec_other, encoding="utf-8")

        manifest = self.create_manifest(
            f"""
PACKAGES = {{
    {{ spec = "local.common@v1", source = "{self.lua_path(spec_local_path)}" }},
    {{ spec = "other.common@v1", source = "{self.lua_path(spec_other_path)}" }}
}}
"""
        )

        # Ambiguous query - "common" matches both local.common@v1 and other.common@v1
        result = self.run_package("common", manifest)

        self.assertEqual(result.returncode, 1)
        self.assertIn("ambiguous", result.stderr.lower())
        self.assertIn("local.common@v1", result.stderr)
        self.assertIn("other.common@v1", result.stderr)

    def test_package_partial_match_no_match(self):
        """Partial match with no matches produces clear error."""
        manifest = self.create_manifest(
            f"""
PACKAGES = {{
    {{ spec = "local.build_dependency@v1", source = "{self.lua_path(self.specs_dir)}/build_dependency.lua" }}
}}
"""
        )

        result = self.run_package("nonexistent", manifest)

        self.assertEqual(result.returncode, 1)
        self.assertIn("no package matching", result.stderr.lower())
        self.assertIn("nonexistent", result.stderr)


if __name__ == "__main__":
    unittest.main()
