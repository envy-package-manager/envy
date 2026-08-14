"""Functional tests for 'envy sync' and 'envy deploy' commands.

Tests syncing (install + deploy), deploy-only, specific identities,
error handling, and transitive dependencies.
"""

import hashlib
import io
import os
import shutil
import sys
import tarfile
import tempfile
import time
import unittest
from pathlib import Path
from typing import Optional, List

from . import test_config
from .env import EnvyTestCase
from .test_config import make_manifest
from .trace_parser import TraceParser


# Test archive contents
TEST_ARCHIVE_FILES = {
    "root/file1.txt": "Test file content\n",
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


# =============================================================================
# Shared specs (used by multiple test classes)
# =============================================================================

# Simple user-managed package: no dependencies, check always false
SPEC_SIMPLE = """IDENTITY = "local.simple@v1"
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

# Cache-managed dependency with archive fetch and build phase
SPEC_BUILD_DEP = """IDENTITY = "local.build_dependency@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = {{strip = 1}}

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  envy.run([[echo 'dependency_data' > dependency.txt
      mkdir -p bin
      echo 'binary' > bin/app]])
end
"""

# Diamond D: base of diamond dependency graph
SPEC_DIAMOND_D = """IDENTITY = "local.diamond_d@v1"
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

# Diamond C: depends on D, right side of diamond
SPEC_DIAMOND_C = """IDENTITY = "local.diamond_c@v1"
DEPENDENCIES = {{
  {{ spec = "local.diamond_d@v1", source = "diamond_d.lua" }}
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

# Product provider: cached package exposing 'tool' product at bin/tool
SPEC_PRODUCT_PROVIDER = """IDENTITY = "local.product_provider@v1"
PRODUCTS = {{ tool = "bin/tool" }}

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}",
}}

INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
end
"""

# Product provider with mixed script/noscript products
SPEC_MIXED_PRODUCTS = """IDENTITY = "local.mixed_products@v1"
PRODUCTS = {{
  tool = "bin/tool",
  library = {{ value = "lib/libfoo.so", script = false }},
}}

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}",
}}

INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
end
"""

# Product provider with spec-level PLATFORMS constraint (linux only)
SPEC_PRODUCT_LINUX_ONLY = """IDENTITY = "local.linux_tool@v1"
PLATFORMS = {{ "linux" }}
PRODUCTS = {{ aptutil = "bin/aptutil" }}

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}",
}}

INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
end
"""

# Product provider with spec-level PLATFORMS constraint (darwin only)
SPEC_PRODUCT_DARWIN_ONLY = """IDENTITY = "local.darwin_tool@v1"
PLATFORMS = {{ "darwin" }}
PRODUCTS = {{ brewtool = "bin/brewtool" }}

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}",
}}

INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
end
"""

# Product provider with no PLATFORMS (all platforms)
SPEC_PRODUCT_ALL_PLATFORMS = """IDENTITY = "local.cross_tool@v1"
PRODUCTS = {{ cross = "bin/cross" }}

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}",
}}

INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
end
"""


class TestSyncCommand(EnvyTestCase):
    """Tests for 'envy sync' command."""

    def setUp(self):
        super().setUp()
        self.test_dir = self.make_temp_dir("test_dir")
        self.specs_dir = self.make_temp_dir("specs_dir")

        # Create test archive and get its hash
        self.archive_path = self.specs_dir / "test.tar.gz"
        self.archive_hash = create_test_archive(self.archive_path)

    def write_spec(self, name: str, content: str) -> str:
        """Write spec to temp dir with placeholder substitution, return Lua path."""
        spec_content = content.format(
            ARCHIVE_PATH=self.archive_path.as_posix(),
            ARCHIVE_HASH=self.archive_hash,
        )
        path = self.specs_dir / f"{name}.lua"
        path.write_text(spec_content, encoding="utf-8")
        return path.as_posix()

    def create_manifest(self, content: str) -> Path:
        """Create manifest file with given content."""
        manifest_path = self.test_dir / "envy.lua"
        manifest_path.write_text(make_manifest(content), encoding="utf-8")
        return manifest_path

    def run_sync(
        self,
        identities: Optional[List[str]] = None,
        manifest: Optional[Path] = None,
    ):
        """Run 'envy sync' command and return result."""
        cmd = [str(self.envy), "--cache-root", str(self.cache_root), "sync"]
        if identities:
            cmd.extend(identities)
        if manifest:
            cmd.extend(["--manifest", str(manifest)])

        result = test_config.run(
            cmd,
            cwd=self.project_root,
            capture_output=True,
            text=True,
        )
        return result

    def run_install(
        self,
        queries: Optional[List[str]] = None,
        manifest: Optional[Path] = None,
    ):
        """Run 'envy install' command and return result."""
        cmd = [str(self.envy), "--cache-root", str(self.cache_root), "install"]
        if queries:
            cmd.extend(queries)
        if manifest:
            cmd.extend(["--manifest", str(manifest)])

        result = test_config.run(
            cmd,
            cwd=self.project_root,
            capture_output=True,
            text=True,
        )
        return result

    def test_install_installs_packages(self):
        """Install installs entire manifest."""
        simple_path = self.write_spec("simple", SPEC_SIMPLE)
        build_dep_path = self.write_spec("build_dep", SPEC_BUILD_DEP)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.build_dependency@v1", source = "{build_dep_path}" }},
    {{ spec = "local.simple@v1", source = "{simple_path}", setup = {{ "main" }} }},
}}
""")

        result = self.run_install(manifest=manifest)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        build_dep_cache = self.cache_root / "packages" / "local.build_dependency@v1"
        simple_cache = self.cache_root / "packages" / "local.simple@v1"
        self.assertTrue(build_dep_cache.exists())
        self.assertTrue(simple_cache.exists())

    def test_install_single_query(self):
        """Install with single query installs only that package."""
        simple_path = self.write_spec("simple", SPEC_SIMPLE)
        build_dep_path = self.write_spec("build_dep", SPEC_BUILD_DEP)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.build_dependency@v1", source = "{build_dep_path}" }},
    {{ spec = "local.simple@v1", source = "{simple_path}", setup = {{ "main" }} }},
}}
""")

        result = self.run_install(queries=["local.simple@v1"], manifest=manifest)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        simple_cache = self.cache_root / "packages" / "local.simple@v1"
        build_dep_cache = self.cache_root / "packages" / "local.build_dependency@v1"
        self.assertTrue(simple_cache.exists())
        self.assertFalse(build_dep_cache.exists())

    def test_install_multiple_queries(self):
        """Install with multiple queries installs all specified."""
        simple_path = self.write_spec("simple", SPEC_SIMPLE)
        build_dep_path = self.write_spec("build_dep", SPEC_BUILD_DEP)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.build_dependency@v1", source = "{build_dep_path}" }},
    {{ spec = "local.simple@v1", source = "{simple_path}", setup = {{ "main" }} }},
}}
""")

        result = self.run_install(
            queries=["local.simple@v1", "local.build_dependency@v1"],
            manifest=manifest,
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        simple_cache = self.cache_root / "packages" / "local.simple@v1"
        build_dep_cache = self.cache_root / "packages" / "local.build_dependency@v1"
        self.assertTrue(simple_cache.exists())
        self.assertTrue(build_dep_cache.exists())

    def test_sync_identity_not_in_manifest_errors(self):
        """Sync with identity not in manifest returns error."""
        simple_path = self.write_spec("simple", SPEC_SIMPLE)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.simple@v1", source = "{simple_path}" }},
}}
""")

        result = self.run_sync(identities=["local.missing@v1"], manifest=manifest)

        self.assertNotEqual(result.returncode, 0, "Expected non-zero exit code")
        self.assertIn("not found in manifest", result.stderr.lower())
        self.assertIn("local.missing@v1", result.stderr)

    def test_sync_partial_missing_identities_errors(self):
        """Sync with some valid and some invalid identities returns error."""
        simple_path = self.write_spec("simple", SPEC_SIMPLE)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.simple@v1", source = "{simple_path}" }},
}}
""")

        result = self.run_sync(
            identities=["local.simple@v1", "local.missing@v1"], manifest=manifest
        )

        self.assertNotEqual(result.returncode, 0, "Expected non-zero exit code")
        self.assertIn("not found in manifest", result.stderr.lower())

        simple_cache = self.cache_root / "packages" / "local.simple@v1"
        self.assertFalse(simple_cache.exists(), "Nothing should be installed on error")

    def test_install_second_run_is_noop(self):
        """Second install run is a no-op (cache hits)."""
        simple_path = self.write_spec("simple", SPEC_SIMPLE)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.simple@v1", source = "{simple_path}", setup = {{ "main" }} }},
}}
""")

        # First install - cache miss
        trace_file1 = self.cache_root / "trace1.jsonl"
        cmd1 = [
            str(self.envy),
            "--cache-root",
            str(self.cache_root),
            f"--trace=file:{trace_file1}",
            "install",
            "local.simple@v1",
            "--manifest",
            str(manifest),
        ]
        result1 = test_config.run(
            cmd1, cwd=self.project_root, capture_output=True, text=True
        )
        self.assertEqual(result1.returncode, 0, f"stderr: {result1.stderr}")

        parser1 = TraceParser(trace_file1)
        cache_misses1 = parser1.filter_by_event("cache_miss")
        self.assertGreater(len(cache_misses1), 0, "Expected cache misses on first run")

        # Second install - should be cache hits
        trace_file2 = self.cache_root / "trace2.jsonl"
        cmd2 = [
            str(self.envy),
            "--cache-root",
            str(self.cache_root),
            f"--trace=file:{trace_file2}",
            "install",
            "local.simple@v1",
            "--manifest",
            str(manifest),
        ]
        result2 = test_config.run(
            cmd2, cwd=self.project_root, capture_output=True, text=True
        )
        self.assertEqual(result2.returncode, 0, f"stderr: {result2.stderr}")

        parser2 = TraceParser(trace_file2)
        completes2 = parser2.filter_by_event("phase_complete")
        self.assertGreater(
            len(completes2), 0, "Expected phase completions on second run"
        )

    def test_sync_no_stdout_output(self):
        """Sync command produces no stdout output."""
        simple_path = self.write_spec("simple", SPEC_SIMPLE)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.simple@v1", source = "{simple_path}" }},
}}
""")

        result = self.run_sync(manifest=manifest)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertEqual(result.stdout, "", "Expected no stdout output from sync")

    def test_install_respects_cache_root(self):
        """Install respects --cache-root flag."""
        custom_cache = self.make_temp_dir("custom_cache")
        simple_path = self.write_spec("simple", SPEC_SIMPLE)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.simple@v1", source = "{simple_path}", setup = {{ "main" }} }},
}}
""")

        cmd = [
            str(self.envy),
            "--cache-root",
            str(custom_cache),
            "install",
            "--manifest",
            str(manifest),
        ]
        result = test_config.run(
            cmd, cwd=self.project_root, capture_output=True, text=True
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        simple_cache = custom_cache / "packages" / "local.simple@v1"
        self.assertTrue(simple_cache.exists())

    def test_sync_empty_manifest(self):
        """Sync with empty manifest succeeds silently."""
        manifest = self.create_manifest("PACKAGES = {}")

        result = self.run_sync(manifest=manifest)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

    def test_install_transitive_dependencies(self):
        """Install installs transitive dependencies."""
        self.write_spec("diamond_d", SPEC_DIAMOND_D)
        diamond_c_path = self.write_spec("diamond_c", SPEC_DIAMOND_C)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.diamond_c@v1", source = "{diamond_c_path}" }},
}}
""")

        result = self.run_install(queries=["local.diamond_c@v1"], manifest=manifest)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")


class TestSyncProductScripts(EnvyTestCase):
    """Tests for product script deployment via 'envy sync'."""

    def setUp(self):
        super().setUp()
        self.test_dir = self.make_temp_dir("test_dir")
        self.specs_dir = self.make_temp_dir("specs_dir")

        # Create test archive and get its hash
        self.archive_path = self.specs_dir / "test.tar.gz"
        self.archive_hash = create_test_archive(self.archive_path)

    def write_spec(self, name: str, content: str) -> str:
        """Write spec to temp dir with placeholder substitution, return Lua path."""
        spec_content = content.format(
            ARCHIVE_PATH=self.archive_path.as_posix(),
            ARCHIVE_HASH=self.archive_hash,
        )
        path = self.specs_dir / f"{name}.lua"
        path.write_text(spec_content, encoding="utf-8")
        return path.as_posix()

    def create_manifest(self, content: str, deploy: bool = True) -> Path:
        manifest_path = self.test_dir / "envy.lua"
        manifest_path.write_text(
            make_manifest(content, deploy=deploy), encoding="utf-8"
        )
        return manifest_path

    def run_sync(
        self,
        manifest: Path,
        identities: Optional[List[str]] = None,
        strict: bool = False,
        platform: Optional[str] = None,
    ):
        cmd = [str(self.envy), "--cache-root", str(self.cache_root), "sync"]
        if strict:
            cmd.append("--strict")
        if platform:
            cmd.extend(["--platform", platform])
        if identities:
            cmd.extend(identities)
        cmd.extend(["--manifest", str(manifest)])
        return test_config.run(
            cmd, cwd=self.project_root, capture_output=True, text=True
        )

    def run_deploy(
        self,
        manifest: Path,
        identities: Optional[List[str]] = None,
        strict: bool = False,
        platform: Optional[str] = None,
    ):
        cmd = [str(self.envy), "--cache-root", str(self.cache_root), "deploy"]
        if strict:
            cmd.append("--strict")
        if platform:
            cmd.extend(["--platform", platform])
        if identities:
            cmd.extend(identities)
        cmd.extend(["--manifest", str(manifest)])
        return test_config.run(
            cmd, cwd=self.project_root, capture_output=True, text=True
        )

    def test_sync_creates_product_scripts(self):
        """Default sync creates product scripts in bin directory."""
        product_path = self.write_spec("product_provider", SPEC_PRODUCT_PROVIDER)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.product_provider@v1", source = "{product_path}" }},
}}
""")

        result = self.run_sync(manifest=manifest)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        bin_dir = self.test_dir / "envy-bin"
        self.assertTrue(bin_dir.exists())

        script_name = "tool.bat" if sys.platform == "win32" else "tool"
        script_path = bin_dir / script_name
        self.assertTrue(script_path.exists())

        content = script_path.read_text()
        self.assertIn("envy-managed", content)
        self.assertIn("product", content)

    def test_sync_product_scripts_have_lf_line_endings(self):
        """Deployed product scripts must use LF line endings, never CRLF."""
        product_path = self.write_spec("product_provider", SPEC_PRODUCT_PROVIDER)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.product_provider@v1", source = "{product_path}" }},
}}
""")

        result = self.run_sync(manifest=manifest)
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        bin_dir = self.test_dir / "envy-bin"
        script_name = "tool.bat" if sys.platform == "win32" else "tool"
        raw = (bin_dir / script_name).read_bytes()
        self.assertNotIn(b"\r", raw, "Product script contains CR bytes (CRLF line endings)")
        self.assertIn(b"\n", raw, "Product script has no line endings at all")

    def test_sync_platform_all_scripts_have_lf_line_endings(self):
        """Both POSIX and Windows scripts use LF when deployed with --platform all."""
        product_path = self.write_spec("product_provider", SPEC_PRODUCT_PROVIDER)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.product_provider@v1", source = "{product_path}" }},
}}
""")

        result = self.run_sync(manifest=manifest, platform="all")
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        bin_dir = self.test_dir / "envy-bin"
        for name in ("tool", "tool.bat", "envy", "envy.bat"):
            path = bin_dir / name
            self.assertTrue(path.exists(), f"{name} missing")
            raw = path.read_bytes()
            self.assertNotIn(
                b"\r", raw, f"{name} contains CR bytes (CRLF line endings)"
            )

    def test_sync_replaces_crlf_script_with_lf(self):
        """Deploy overwrites an existing CRLF envy-managed script with LF."""
        product_path = self.write_spec("product_provider", SPEC_PRODUCT_PROVIDER)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.product_provider@v1", source = "{product_path}" }},
}}
""")

        bin_dir = self.test_dir / "envy-bin"
        bin_dir.mkdir(parents=True, exist_ok=True)

        script_name = "tool.bat" if sys.platform == "win32" else "tool"
        script_path = bin_dir / script_name
        script_path.write_bytes(b"# envy-managed OLD\r\nold content\r\n")

        result = self.run_sync(manifest=manifest)
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        raw = script_path.read_bytes()
        self.assertNotIn(b"\r", raw, "Rewritten script still contains CR bytes")
        self.assertIn(b"envy-managed", raw)

    def test_sync_updates_envy_managed_scripts(self):
        """Sync updates existing envy-managed scripts."""
        product_path = self.write_spec("product_provider", SPEC_PRODUCT_PROVIDER)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.product_provider@v1", source = "{product_path}" }},
}}
""")

        bin_dir = self.test_dir / "envy-bin"
        bin_dir.mkdir(parents=True, exist_ok=True)

        script_name = "tool.bat" if sys.platform == "win32" else "tool"
        script_path = bin_dir / script_name
        script_path.write_text("# envy-managed OLD_VERSION\nold content\n")

        result = self.run_sync(manifest=manifest)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        content = script_path.read_text()
        self.assertNotIn("OLD_VERSION", content)
        self.assertIn("envy-managed", content)

    def test_sync_skips_non_envy_file_conflict(self):
        """Sync silently skips user-owned (non-envy-managed) product scripts."""
        product_path = self.write_spec("product_provider", SPEC_PRODUCT_PROVIDER)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.product_provider@v1", source = "{product_path}" }},
}}
""")

        bin_dir = self.test_dir / "envy-bin"
        bin_dir.mkdir(parents=True, exist_ok=True)

        script_name = "tool.bat" if sys.platform == "win32" else "tool"
        script_path = bin_dir / script_name
        user_content = "#!/bin/bash\necho 'user script'\n"
        script_path.write_text(user_content)

        result = self.run_sync(manifest=manifest)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertEqual(script_path.read_text(), user_content)

    def test_sync_strict_errors_on_non_envy_file_conflict(self):
        """Sync --strict errors if non-envy-managed file conflicts with product name."""
        product_path = self.write_spec("product_provider", SPEC_PRODUCT_PROVIDER)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.product_provider@v1", source = "{product_path}" }},
}}
""")

        bin_dir = self.test_dir / "envy-bin"
        bin_dir.mkdir(parents=True, exist_ok=True)

        script_name = "tool.bat" if sys.platform == "win32" else "tool"
        script_path = bin_dir / script_name
        script_path.write_text("#!/bin/bash\necho 'user script'\n")

        result = self.run_sync(manifest=manifest, strict=True)

        self.assertNotEqual(result.returncode, 0, "Expected non-zero exit code")
        self.assertIn("not envy-managed", result.stderr.lower())

    def test_sync_removes_obsolete_scripts(self):
        """Sync removes obsolete envy-managed scripts."""
        simple_path = self.write_spec("simple", SPEC_SIMPLE)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.simple@v1", source = "{simple_path}" }},
}}
""")

        bin_dir = self.test_dir / "envy-bin"
        bin_dir.mkdir(parents=True, exist_ok=True)

        obsolete_name = "old_tool.bat" if sys.platform == "win32" else "old_tool"
        obsolete_path = bin_dir / obsolete_name
        obsolete_path.write_text("# envy-managed v1.0.0\nold content\n")

        result = self.run_sync(manifest=manifest)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertFalse(obsolete_path.exists())

    def test_sync_preserves_envy_executable_content_when_unchanged(self):
        """Sync does not rewrite bootstrap script when content matches."""
        simple_path = self.write_spec("simple", SPEC_SIMPLE)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.simple@v1", source = "{simple_path}" }},
}}
""")

        # Run sync once to create bootstrap
        result1 = self.run_sync(manifest=manifest)
        self.assertEqual(result1.returncode, 0, f"stderr: {result1.stderr}")

        bin_dir = self.test_dir / "envy-bin"
        envy_name = "envy.bat" if sys.platform == "win32" else "envy"
        envy_path = bin_dir / envy_name
        self.assertTrue(envy_path.exists())

        # Set mtime to a known past date to reliably detect rewrites
        jan_1_2000 = time.mktime((2000, 1, 1, 0, 0, 0, 0, 0, 0))
        os.utime(envy_path, (jan_1_2000, jan_1_2000))

        # Run sync again
        result2 = self.run_sync(manifest=manifest)
        self.assertEqual(result2.returncode, 0, f"stderr: {result2.stderr}")

        # Bootstrap should not be rewritten (mtime unchanged)
        mtime_after = envy_path.stat().st_mtime
        self.assertEqual(
            mtime_after, jan_1_2000, "Bootstrap should not be rewritten when unchanged"
        )

    def test_install_then_deploy_deploys_scripts(self):
        """Install installs packages, then deploy deploys scripts standalone."""
        product_path = self.write_spec("product_provider", SPEC_PRODUCT_PROVIDER)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.product_provider@v1", source = "{product_path}" }},
}}
""")

        # Install packages first
        cmd = [
            str(self.envy),
            "--cache-root",
            str(self.cache_root),
            "install",
            "--manifest",
            str(manifest),
        ]
        result = test_config.run(
            cmd, cwd=self.project_root, capture_output=True, text=True
        )
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        pkg_path = self.cache_root / "packages" / "local.product_provider@v1"
        self.assertTrue(pkg_path.exists())

        # Then deploy to deploy scripts (standalone deploy, no install)
        result = self.run_deploy(manifest=manifest)
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        bin_dir = self.test_dir / "envy-bin"
        script_name = "tool.bat" if sys.platform == "win32" else "tool"
        script_path = bin_dir / script_name
        self.assertTrue(script_path.exists())

    def test_sync_installs_and_deploys_in_one_shot(self):
        """Sync installs packages AND deploys product scripts in one invocation."""
        product_path = self.write_spec("product_provider", SPEC_PRODUCT_PROVIDER)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.product_provider@v1", source = "{product_path}" }},
}}
""")

        # Single sync: should install packages and deploy scripts
        result = self.run_sync(manifest=manifest)
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        # Verify packages were installed
        pkg_path = self.cache_root / "packages" / "local.product_provider@v1"
        self.assertTrue(pkg_path.exists(), "Package not installed by sync")

        # Verify product scripts were deployed
        bin_dir = self.test_dir / "envy-bin"
        script_name = "tool.bat" if sys.platform == "win32" else "tool"
        script_path = bin_dir / script_name
        self.assertTrue(script_path.exists(), "Product script not deployed by sync")

        content = script_path.read_text()
        self.assertIn("envy-managed", content)

    def test_sync_timestamp_preserved_when_content_unchanged(self):
        """Sync preserves file timestamps when content is unchanged."""
        product_path = self.write_spec("product_provider", SPEC_PRODUCT_PROVIDER)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.product_provider@v1", source = "{product_path}" }},
}}
""")

        result1 = self.run_sync(manifest=manifest)
        self.assertEqual(result1.returncode, 0, f"stderr: {result1.stderr}")

        bin_dir = self.test_dir / "envy-bin"
        script_name = "tool.bat" if sys.platform == "win32" else "tool"
        script_path = bin_dir / script_name

        # Set mtime to a known past date to reliably detect rewrites
        jan_1_2000 = time.mktime((2000, 1, 1, 0, 0, 0, 0, 0, 0))
        os.utime(script_path, (jan_1_2000, jan_1_2000))

        result2 = self.run_sync(manifest=manifest)
        self.assertEqual(result2.returncode, 0, f"stderr: {result2.stderr}")

        mtime_after = script_path.stat().st_mtime
        self.assertEqual(mtime_after, jan_1_2000, "File timestamp should be unchanged")

    def test_sync_stamp_uses_schema_version_marker(self):
        """Stamped scripts carry `envy-managed schema "N"`, not a release version string."""
        product_path = self.write_spec("product_provider", SPEC_PRODUCT_PROVIDER)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.product_provider@v1", source = "{product_path}" }},
}}
""")

        result = self.run_sync(manifest=manifest)
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        bin_dir = self.test_dir / "envy-bin"
        script_name = "tool.bat" if sys.platform == "win32" else "tool"
        content = (bin_dir / script_name).read_text()
        self.assertIn("envy-managed schema \"", content)

    def _write_legacy_stamped_script(self, script_path):
        """Plant a script in the pre-change `# envy-managed <release-version>` format."""
        if sys.platform == "win32":
            legacy = "@echo off\r\nrem envy-managed 1.2.3\r\necho legacy\r\n"
        else:
            legacy = "#!/usr/bin/env bash\n# envy-managed 1.2.3\necho legacy\n"
        script_path.write_text(legacy)

    def test_sync_rewrites_legacy_release_version_stamp(self):
        """Pre-existing scripts in the old release-version stamp format get migrated."""
        product_path = self.write_spec("product_provider", SPEC_PRODUCT_PROVIDER)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.product_provider@v1", source = "{product_path}" }},
}}
""")

        bin_dir = self.test_dir / "envy-bin"
        bin_dir.mkdir(parents=True, exist_ok=True)
        script_name = "tool.bat" if sys.platform == "win32" else "tool"
        script_path = bin_dir / script_name

        self._write_legacy_stamped_script(script_path)

        jan_1_2000 = time.mktime((2000, 1, 1, 0, 0, 0, 0, 0, 0))
        os.utime(script_path, (jan_1_2000, jan_1_2000))

        result = self.run_sync(manifest=manifest)
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        mtime_after = script_path.stat().st_mtime
        self.assertNotEqual(
            mtime_after, jan_1_2000,
            "Legacy-format script should have been rewritten, but timestamp is unchanged",
        )

        new_content = script_path.read_text()
        self.assertIn("envy-managed schema \"", new_content)
        self.assertNotIn("1.2.3", new_content)
        self.assertNotIn("legacy", new_content)

    def test_sync_strict_accepts_legacy_envy_managed_stamp(self):
        """Strict mode must treat legacy `# envy-managed <release-version>` as envy-owned, not as a user-owned conflict, and migrate it."""
        product_path = self.write_spec("product_provider", SPEC_PRODUCT_PROVIDER)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.product_provider@v1", source = "{product_path}" }},
}}
""")

        bin_dir = self.test_dir / "envy-bin"
        bin_dir.mkdir(parents=True, exist_ok=True)
        script_name = "tool.bat" if sys.platform == "win32" else "tool"
        script_path = bin_dir / script_name

        self._write_legacy_stamped_script(script_path)

        result = self.run_sync(manifest=manifest, strict=True)

        self.assertEqual(
            result.returncode, 0,
            f"Strict sync rejected a legacy envy-managed stamp as a conflict; stderr: {result.stderr}",
        )
        self.assertNotIn("not envy-managed", result.stderr.lower())

        migrated = script_path.read_text()
        self.assertIn("envy-managed schema \"", migrated)
        self.assertNotIn("1.2.3", migrated)
        self.assertNotIn("legacy", migrated)

    def test_sync_legacy_migration_settles_into_idempotence(self):
        """After a legacy-stamp script is migrated, the next sync is a no-op (mtime unchanged)."""
        product_path = self.write_spec("product_provider", SPEC_PRODUCT_PROVIDER)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.product_provider@v1", source = "{product_path}" }},
}}
""")

        bin_dir = self.test_dir / "envy-bin"
        bin_dir.mkdir(parents=True, exist_ok=True)
        script_name = "tool.bat" if sys.platform == "win32" else "tool"
        script_path = bin_dir / script_name

        self._write_legacy_stamped_script(script_path)

        result1 = self.run_sync(manifest=manifest)
        self.assertEqual(result1.returncode, 0, f"stderr: {result1.stderr}")
        migrated = script_path.read_text()
        self.assertIn("envy-managed schema \"", migrated)

        jan_1_2000 = time.mktime((2000, 1, 1, 0, 0, 0, 0, 0, 0))
        os.utime(script_path, (jan_1_2000, jan_1_2000))

        result2 = self.run_sync(manifest=manifest)
        self.assertEqual(result2.returncode, 0, f"stderr: {result2.stderr}")

        mtime_after = script_path.stat().st_mtime
        self.assertEqual(
            mtime_after, jan_1_2000,
            "Post-migration sync should be a no-op, but the script was rewritten",
        )
        self.assertEqual(script_path.read_text(), migrated)

    def test_sync_rewrites_script_with_mismatched_schema_version(self):
        """A stamped script whose schema version drifts gets re-stamped on next sync."""
        import re

        product_path = self.write_spec("product_provider", SPEC_PRODUCT_PROVIDER)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.product_provider@v1", source = "{product_path}" }},
}}
""")

        result1 = self.run_sync(manifest=manifest)
        self.assertEqual(result1.returncode, 0, f"stderr: {result1.stderr}")

        bin_dir = self.test_dir / "envy-bin"
        script_name = "tool.bat" if sys.platform == "win32" else "tool"
        script_path = bin_dir / script_name

        canonical = script_path.read_text()
        self.assertIn("envy-managed schema \"", canonical)

        mutated = re.sub(
            r'schema "\d+"',
            'schema "999"',
            canonical,
        )
        self.assertNotEqual(canonical, mutated, "Failed to mutate version stamp")
        script_path.write_text(mutated)

        jan_1_2000 = time.mktime((2000, 1, 1, 0, 0, 0, 0, 0, 0))
        os.utime(script_path, (jan_1_2000, jan_1_2000))

        result2 = self.run_sync(manifest=manifest)
        self.assertEqual(result2.returncode, 0, f"stderr: {result2.stderr}")

        mtime_after = script_path.stat().st_mtime
        self.assertNotEqual(
            mtime_after, jan_1_2000,
            "Script with mismatched schema version should have been rewritten",
        )

        restored = script_path.read_text()
        self.assertEqual(restored, canonical)
        self.assertNotIn('schema "999"', restored)

    def test_product_script_execution_and_arg_forwarding(self):
        """Product scripts execute correctly and forward arguments."""
        # Create archive similar to other tests
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode="w:gz") as tar:
            if sys.platform == "win32":
                tool_content = b"@echo off\necho Args: %*\n"
                tool_name = "echotool.bat"
            else:
                tool_content = b'#!/bin/sh\necho "Args: $@"\n'
                tool_name = "echotool"

            # Add tool file to archive
            tool_info = tarfile.TarInfo(name=f"bin/{tool_name}")
            tool_info.size = len(tool_content)
            tool_info.mode = 0o755
            tar.addfile(tool_info, io.BytesIO(tool_content))

        archive_data = buf.getvalue()
        archive_path = self.specs_dir / "echotool.tar.gz"
        archive_path.write_bytes(archive_data)
        archive_hash = hashlib.sha256(archive_data).hexdigest()

        # Create spec for product provider (use normal Python substitution to avoid escaping issues)
        spec = 'IDENTITY = "local.echotool@v1"\n'
        spec += 'PRODUCTS = { echotool = "bin/' + tool_name + '" }\n\n'
        spec += "FETCH = {\n"
        spec += '  source = "' + archive_path.as_posix() + '",\n'
        spec += '  sha256 = "' + archive_hash + '",\n'
        spec += "}\n\n"
        spec += "STAGE = { strip = 0 }\n\n"
        spec += (
            "INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)\n"
        )
        spec += '  envy.run("cp -r " .. stage_dir .. "/* " .. install_dir .. "/")\n'
        spec += "end\n"
        spec_file = self.specs_dir / "echotool.lua"
        spec_file.write_text(spec, encoding="utf-8")
        spec_path = spec_file.as_posix()

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.echotool@v1", source = "{spec_path}" }},
}}
""")

        # Run sync to create product script
        result = self.run_sync(manifest=manifest)
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        # Copy envy executable to bin dir so product script can find it.
        # Atomic copy (temp + rename) avoids ETXTBSY on Linux.
        bin_dir = self.test_dir / "envy-bin"
        envy_name = "envy.exe" if sys.platform == "win32" else "envy"
        tmp = bin_dir / (envy_name + ".tmp")
        shutil.copy(self.envy, tmp)
        try:
            os.rename(str(tmp), str(bin_dir / envy_name))
        except OSError:
            tmp.unlink(missing_ok=True)
            raise

        # Test 1: Execute product script without arguments
        script_name = "echotool.bat" if sys.platform == "win32" else "echotool"
        script_path = bin_dir / script_name
        self.assertTrue(
            script_path.exists(), f"Product script not created: {script_path}"
        )

        result = test_config.run(
            [str(script_path)],
            cwd=self.test_dir,
            capture_output=True,
            text=True,
            env={**test_config.get_test_env(), "ENVY_CACHE_ROOT": str(self.cache_root)},
        )
        self.assertEqual(
            result.returncode, 0, f"Product script failed: {result.stderr}"
        )
        self.assertIn("Args:", result.stdout)

        # Test 2: Execute product script with arguments
        result = test_config.run(
            [str(script_path), "arg1", "arg2", "arg with spaces"],
            cwd=self.test_dir,
            capture_output=True,
            text=True,
            env={**test_config.get_test_env(), "ENVY_CACHE_ROOT": str(self.cache_root)},
        )
        self.assertEqual(
            result.returncode, 0, f"Product script with args failed: {result.stderr}"
        )
        self.assertIn("arg1", result.stdout)
        self.assertIn("arg2", result.stdout)
        self.assertIn("arg with spaces", result.stdout)

    def test_product_script_propagates_exit_code(self):
        """Product scripts propagate the underlying tool's non-zero exit code."""
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode="w:gz") as tar:
            if sys.platform == "win32":
                tool_content = b"@echo off\nexit /b 42\n"
                tool_name = "failtool.bat"
            else:
                tool_content = b"#!/bin/sh\nexit 42\n"
                tool_name = "failtool"

            tool_info = tarfile.TarInfo(name=f"bin/{tool_name}")
            tool_info.size = len(tool_content)
            tool_info.mode = 0o755
            tar.addfile(tool_info, io.BytesIO(tool_content))

        archive_data = buf.getvalue()
        archive_path = self.specs_dir / "failtool.tar.gz"
        archive_path.write_bytes(archive_data)
        archive_hash = hashlib.sha256(archive_data).hexdigest()

        spec = 'IDENTITY = "local.failtool@v1"\n'
        spec += 'PRODUCTS = { failtool = "bin/' + tool_name + '" }\n\n'
        spec += "FETCH = {\n"
        spec += '  source = "' + archive_path.as_posix() + '",\n'
        spec += '  sha256 = "' + archive_hash + '",\n'
        spec += "}\n\n"
        spec += "STAGE = { strip = 0 }\n\n"
        spec += (
            "INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)\n"
        )
        spec += '  envy.run("cp -r " .. stage_dir .. "/* " .. install_dir .. "/")\n'
        spec += "end\n"
        spec_file = self.specs_dir / "failtool.lua"
        spec_file.write_text(spec, encoding="utf-8")
        spec_path = spec_file.as_posix()

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.failtool@v1", source = "{spec_path}" }},
}}
""")

        result = self.run_sync(manifest=manifest)
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        # Atomic copy (temp + rename) avoids ETXTBSY on Linux.
        bin_dir = self.test_dir / "envy-bin"
        envy_name = "envy.exe" if sys.platform == "win32" else "envy"
        tmp = bin_dir / (envy_name + ".tmp")
        shutil.copy(self.envy, tmp)
        try:
            os.rename(str(tmp), str(bin_dir / envy_name))
        except OSError:
            tmp.unlink(missing_ok=True)
            raise

        script_name = "failtool.bat" if sys.platform == "win32" else "failtool"
        script_path = bin_dir / script_name
        self.assertTrue(
            script_path.exists(), f"Product script not created: {script_path}"
        )

        result = test_config.run(
            [str(script_path)],
            cwd=self.test_dir,
            capture_output=True,
            text=True,
            env={**test_config.get_test_env(), "ENVY_CACHE_ROOT": str(self.cache_root)},
        )
        self.assertEqual(
            result.returncode,
            42,
            f"Expected exit code 42, got {result.returncode}. "
            f"stdout: {result.stdout}, stderr: {result.stderr}",
        )

    def test_no_script_for_noscript_product(self):
        """Products with script=false do not get scripts created."""
        mixed_path = self.write_spec("mixed_products", SPEC_MIXED_PRODUCTS)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.mixed_products@v1", source = "{mixed_path}" }},
}}
""")

        result = self.run_sync(manifest=manifest)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        bin_dir = self.test_dir / "envy-bin"
        self.assertTrue(bin_dir.exists())

        # Tool script should exist
        tool_script = "tool.bat" if sys.platform == "win32" else "tool"
        self.assertTrue(
            (bin_dir / tool_script).exists(), "tool script should be created"
        )

        # Library script should NOT exist
        library_script = "library.bat" if sys.platform == "win32" else "library"
        self.assertFalse(
            (bin_dir / library_script).exists(),
            "library script should NOT be created (script=false)",
        )

    def test_sync_platform_posix_creates_only_posix_scripts(self):
        """--platform posix creates only POSIX product scripts and bootstrap."""
        product_path = self.write_spec("product_provider", SPEC_PRODUCT_PROVIDER)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.product_provider@v1", source = "{product_path}" }},
}}
""")

        result = self.run_sync(manifest=manifest, platform="posix")
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        bin_dir = self.test_dir / "envy-bin"
        self.assertTrue((bin_dir / "tool").exists(), "POSIX product script missing")
        self.assertFalse(
            (bin_dir / "tool.bat").exists(), "Windows product script should not exist"
        )
        self.assertTrue((bin_dir / "envy").exists(), "POSIX bootstrap missing")
        self.assertFalse(
            (bin_dir / "envy.bat").exists(), "Windows bootstrap should not exist"
        )

    def test_sync_platform_windows_creates_only_windows_scripts(self):
        """--platform windows creates only Windows product scripts and bootstrap."""
        product_path = self.write_spec("product_provider", SPEC_PRODUCT_PROVIDER)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.product_provider@v1", source = "{product_path}" }},
}}
""")

        result = self.run_sync(manifest=manifest, platform="windows")
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        bin_dir = self.test_dir / "envy-bin"
        self.assertTrue(
            (bin_dir / "tool.bat").exists(), "Windows product script missing"
        )
        self.assertFalse(
            (bin_dir / "tool").exists(), "POSIX product script should not exist"
        )
        self.assertTrue((bin_dir / "envy.bat").exists(), "Windows bootstrap missing")
        self.assertFalse(
            (bin_dir / "envy").exists(), "POSIX bootstrap should not exist"
        )

    def test_sync_platform_all_creates_both_platform_scripts(self):
        """--platform all creates both POSIX and Windows product scripts."""
        product_path = self.write_spec("product_provider", SPEC_PRODUCT_PROVIDER)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.product_provider@v1", source = "{product_path}" }},
}}
""")

        result = self.run_sync(manifest=manifest, platform="all")
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        bin_dir = self.test_dir / "envy-bin"
        self.assertTrue((bin_dir / "tool").exists(), "POSIX product script missing")
        self.assertTrue(
            (bin_dir / "tool.bat").exists(), "Windows product script missing"
        )
        self.assertTrue((bin_dir / "envy").exists(), "POSIX bootstrap missing")
        self.assertTrue((bin_dir / "envy.bat").exists(), "Windows bootstrap missing")

        # Both should be envy-managed
        self.assertIn("envy-managed", (bin_dir / "tool").read_text())
        self.assertIn("envy-managed", (bin_dir / "tool.bat").read_text())
        self.assertIn("envy-managed", (bin_dir / "envy").read_text())
        self.assertIn("envy-managed", (bin_dir / "envy.bat").read_text())

    @unittest.skipIf(sys.platform == "win32", "Unix permissions test")
    def test_sync_platform_all_posix_scripts_are_executable(self):
        """--platform all sets executable bit on POSIX scripts only."""
        product_path = self.write_spec("product_provider", SPEC_PRODUCT_PROVIDER)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.product_provider@v1", source = "{product_path}" }},
}}
""")

        result = self.run_sync(manifest=manifest, platform="all")
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        bin_dir = self.test_dir / "envy-bin"
        self.assertTrue(os.access(bin_dir / "tool", os.X_OK))
        self.assertTrue(os.access(bin_dir / "envy", os.X_OK))

    def test_sync_platform_all_cleanup_removes_obsolete_for_both(self):
        """--platform all removes obsolete envy-managed scripts for both platforms."""
        simple_path = self.write_spec("simple", SPEC_SIMPLE)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.simple@v1", source = "{simple_path}" }},
}}
""")

        bin_dir = self.test_dir / "envy-bin"
        bin_dir.mkdir(parents=True, exist_ok=True)

        # Plant obsolete scripts for both platforms
        (bin_dir / "old_tool").write_text("# envy-managed v1.0.0\nold posix\n")
        (bin_dir / "old_tool.bat").write_text("REM envy-managed v1.0.0\nold windows\n")

        result = self.run_sync(manifest=manifest, platform="all")
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        self.assertFalse((bin_dir / "old_tool").exists())
        self.assertFalse((bin_dir / "old_tool.bat").exists())

    def test_product_command_returns_noscript_value(self):
        """envy product command returns full path for noscript products."""
        mixed_path = self.write_spec("mixed_products", SPEC_MIXED_PRODUCTS)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.mixed_products@v1", source = "{mixed_path}" }},
}}
""")

        # First sync to install the package
        result = self.run_sync(manifest=manifest)
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        # Now run envy product to get the library path
        cmd = [
            str(self.envy),
            "--cache-root",
            str(self.cache_root),
            "product",
            "library",
            "--manifest",
            str(manifest),
        ]
        result = test_config.run(
            cmd, cwd=self.project_root, capture_output=True, text=True
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        # The output should contain the full resolved path
        self.assertIn("lib/libfoo.so", result.stdout)

    def test_product_listing_includes_noscript(self):
        """envy product listing includes both script and noscript products."""
        mixed_path = self.write_spec("mixed_products", SPEC_MIXED_PRODUCTS)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.mixed_products@v1", source = "{mixed_path}" }},
}}
""")

        # First sync to install the package
        result = self.run_sync(manifest=manifest)
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        # Now run envy product to list all products
        cmd = [
            str(self.envy),
            "--cache-root",
            str(self.cache_root),
            "product",
            "--manifest",
            str(manifest),
        ]
        result = test_config.run(
            cmd, cwd=self.project_root, capture_output=True, text=True
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        # Both products should be listed (output is to stderr for human-readable format)
        self.assertIn("tool", result.stderr)
        self.assertIn("library", result.stderr)


class TestSyncPlatformConstraints(EnvyTestCase):
    """Tests for declarative platforms constraints on packages."""

    def setUp(self):
        super().setUp()
        self.test_dir = self.make_temp_dir("test_dir")
        self.specs_dir = self.make_temp_dir("specs_dir")

        self.archive_path = self.specs_dir / "test.tar.gz"
        self.archive_hash = create_test_archive(self.archive_path)

    def write_spec(self, name: str, content: str) -> str:
        spec_content = content.format(
            ARCHIVE_PATH=self.archive_path.as_posix(),
            ARCHIVE_HASH=self.archive_hash,
        )
        path = self.specs_dir / f"{name}.lua"
        path.write_text(spec_content, encoding="utf-8")
        return path.as_posix()

    def create_manifest(self, content: str) -> Path:
        manifest_path = self.test_dir / "envy.lua"
        manifest_path.write_text(make_manifest(content, deploy=True), encoding="utf-8")
        return manifest_path

    def run_sync(self, manifest: Path, platform: Optional[str] = None):
        cmd = [str(self.envy), "--cache-root", str(self.cache_root), "sync"]
        if platform:
            cmd.extend(["--platform", platform])
        cmd.extend(["--manifest", str(manifest)])
        return test_config.run(
            cmd, cwd=self.project_root, capture_output=True, text=True
        )

    def run_deploy(self, manifest: Path, platform: Optional[str] = None):
        cmd = [str(self.envy), "--cache-root", str(self.cache_root), "deploy"]
        if platform:
            cmd.extend(["--platform", platform])
        cmd.extend(["--manifest", str(manifest)])
        return test_config.run(
            cmd, cwd=self.project_root, capture_output=True, text=True
        )

    def test_manifest_platforms_constraint_filters_sync_targets(self):
        """Package with platforms not matching host is skipped during sync."""
        # Use a platform that doesn't match the current host
        non_host = "windows" if sys.platform != "win32" else "linux"
        spec_path = self.write_spec("simple", SPEC_SIMPLE)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.simple@v1", source = "{spec_path}",
       platforms = {{ "{non_host}" }} }},
}}
""")

        result = self.run_sync(manifest=manifest)
        # Should succeed (package filtered out, nothing to do)
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

    def test_manifest_platforms_matching_host_runs_sync(self):
        """Package with platforms matching host is synced normally."""
        host_platform = (
            "darwin"
            if sys.platform == "darwin"
            else ("linux" if sys.platform == "linux" else "windows")
        )
        spec_path = self.write_spec("simple", SPEC_SIMPLE)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.simple@v1", source = "{spec_path}",
       platforms = {{ "{host_platform}" }} }},
}}
""")

        result = self.run_sync(manifest=manifest)
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

    def test_deploy_platform_all_skips_scripts_for_nonmatching_spec_platforms(self):
        """deploy --platform all with PLATFORMS=linux creates posix script but no .bat."""
        linux_path = self.write_spec("linux_tool", SPEC_PRODUCT_LINUX_ONLY)
        cross_path = self.write_spec("cross_tool", SPEC_PRODUCT_ALL_PLATFORMS)

        # Use deploy (not sync) to avoid host-platform target filtering.
        # Deploy resolves all packages and filters at script generation time.
        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.linux_tool@v1", source = "{linux_path}",
       platforms = {{ "linux" }} }},
    {{ spec = "local.cross_tool@v1", source = "{cross_path}" }},
}}
""")

        result = self.run_deploy(manifest=manifest, platform="all")
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        bin_dir = self.test_dir / "envy-bin"

        # linux-only product: posix script yes, .bat no
        self.assertTrue(
            (bin_dir / "aptutil").exists(),
            "POSIX script for linux-only product should exist",
        )
        self.assertFalse(
            (bin_dir / "aptutil.bat").exists(),
            "Windows script for linux-only product should NOT exist",
        )

        # cross-platform product: both scripts
        self.assertTrue(
            (bin_dir / "cross").exists(),
            "POSIX script for unconstrained product should exist",
        )
        self.assertTrue(
            (bin_dir / "cross.bat").exists(),
            "Windows script for unconstrained product should exist",
        )

    def test_sync_platform_all_resolves_nonhost_for_scripts(self):
        """sync --platform all resolves non-host packages for script generation."""
        linux_path = self.write_spec("linux_tool", SPEC_PRODUCT_LINUX_ONLY)
        cross_path = self.write_spec("cross_tool", SPEC_PRODUCT_ALL_PLATFORMS)

        # Manifest platforms = {"linux"}, spec PLATFORMS = {"linux"}.
        # Intersection = {"linux"} → POSIX script, no .bat.
        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.linux_tool@v1", source = "{linux_path}",
       platforms = {{ "linux" }} }},
    {{ spec = "local.cross_tool@v1", source = "{cross_path}" }},
}}
""")

        result = self.run_sync(manifest=manifest, platform="all")
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        bin_dir = self.test_dir / "envy-bin"

        # linux-only product: posix yes (linux maps to posix), .bat no
        self.assertTrue(
            (bin_dir / "aptutil").exists(),
            "POSIX script for linux-only product should exist after sync --platform all",
        )
        self.assertFalse(
            (bin_dir / "aptutil.bat").exists(),
            "Windows script for linux-only product should NOT exist",
        )

        # Cross-platform product should still get both
        self.assertTrue((bin_dir / "cross").exists())
        self.assertTrue((bin_dir / "cross.bat").exists())

    @unittest.skipUnless(sys.platform == "darwin", "macOS-specific test")
    def test_deploy_platform_all_darwin_only_creates_posix_not_bat(self):
        """darwin-only spec with --platform all creates POSIX script, not .bat."""
        darwin_path = self.write_spec("darwin_tool", SPEC_PRODUCT_DARWIN_ONLY)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.darwin_tool@v1", source = "{darwin_path}",
       platforms = {{ "darwin" }} }},
}}
""")

        result = self.run_deploy(manifest=manifest, platform="all")
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        bin_dir = self.test_dir / "envy-bin"
        self.assertTrue(
            (bin_dir / "brewtool").exists(),
            "POSIX script for darwin-only product should exist",
        )
        self.assertFalse(
            (bin_dir / "brewtool.bat").exists(),
            "Windows script for darwin-only product should NOT exist",
        )

    def test_platform_all_cleanup_respects_constraints(self):
        """--platform all cleanup does not remove scripts for constrained products."""
        cross_path = self.write_spec("cross_tool", SPEC_PRODUCT_ALL_PLATFORMS)
        linux_path = self.write_spec("linux_tool", SPEC_PRODUCT_LINUX_ONLY)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.cross_tool@v1", source = "{cross_path}" }},
    {{ spec = "local.linux_tool@v1", source = "{linux_path}",
       platforms = {{ "linux" }} }},
}}
""")

        bin_dir = self.test_dir / "envy-bin"
        bin_dir.mkdir(parents=True, exist_ok=True)

        # Plant an obsolete envy-managed script
        (bin_dir / "obsolete").write_text("# envy-managed v1.0.0\nold\n")
        (bin_dir / "obsolete.bat").write_text("REM envy-managed v1.0.0\nold\n")

        result = self.run_sync(manifest=manifest, platform="all")
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        # Obsolete scripts should be removed
        self.assertFalse(
            (bin_dir / "obsolete").exists(),
            "Obsolete POSIX script should be removed",
        )
        self.assertFalse(
            (bin_dir / "obsolete.bat").exists(),
            "Obsolete Windows script should be removed",
        )

        # linux-only product: .bat should NOT have been created and should not
        # cause cleanup to remove something that was never there
        self.assertFalse(
            (bin_dir / "aptutil.bat").exists(),
            "Windows script for linux-only product should never exist",
        )

    def test_empty_platforms_matches_all(self):
        """Package without platforms field gets scripts on all requested platforms."""
        cross_path = self.write_spec("cross_tool", SPEC_PRODUCT_ALL_PLATFORMS)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.cross_tool@v1", source = "{cross_path}" }},
}}
""")

        result = self.run_sync(manifest=manifest, platform="all")
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        bin_dir = self.test_dir / "envy-bin"
        self.assertTrue((bin_dir / "cross").exists())
        self.assertTrue((bin_dir / "cross.bat").exists())

    def test_spec_platforms_intersects_with_manifest_platforms(self):
        """Spec PLATFORMS intersected with manifest platforms narrows constraint."""
        # Spec declares PLATFORMS = {"linux"} (linux only)
        # Manifest also constrains to platforms = {"linux"}
        # Effective: linux only — no .bat should be generated
        # Use deploy (not sync) to avoid host-platform target filtering.
        linux_path = self.write_spec("linux_tool", SPEC_PRODUCT_LINUX_ONLY)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.linux_tool@v1", source = "{linux_path}",
       platforms = {{ "linux" }} }},
}}
""")

        result = self.run_deploy(manifest=manifest, platform="all")
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        bin_dir = self.test_dir / "envy-bin"
        # Should create POSIX (linux maps to posix) but not Windows
        self.assertTrue(
            (bin_dir / "aptutil").exists(),
            "POSIX script for linux-only product should exist",
        )
        self.assertFalse(
            (bin_dir / "aptutil.bat").exists(),
            "Windows script for linux-only product should NOT exist",
        )

    def test_spec_platforms_alone_without_manifest_platforms(self):
        """Spec PLATFORMS constrains scripts even without manifest-level platforms."""
        # Manifest has NO platforms field — constraint comes from spec only
        linux_path = self.write_spec("linux_tool", SPEC_PRODUCT_LINUX_ONLY)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.linux_tool@v1", source = "{linux_path}" }},
}}
""")

        result = self.run_deploy(manifest=manifest, platform="all")
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        bin_dir = self.test_dir / "envy-bin"
        # Spec PLATFORMS = {"linux"} → posix yes, windows no
        self.assertTrue(
            (bin_dir / "aptutil").exists(),
            "POSIX script should exist (spec PLATFORMS includes linux)",
        )
        self.assertFalse(
            (bin_dir / "aptutil.bat").exists(),
            "Windows script should NOT exist (spec PLATFORMS is linux-only)",
        )

    def test_spec_platforms_intersection_narrows_to_empty(self):
        """Spec PLATFORMS={linux} ∩ manifest platforms={darwin} = empty → no scripts."""
        linux_path = self.write_spec("linux_tool", SPEC_PRODUCT_LINUX_ONLY)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.linux_tool@v1", source = "{linux_path}",
       platforms = {{ "darwin" }} }},
}}
""")

        result = self.run_deploy(manifest=manifest, platform="all")
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        bin_dir = self.test_dir / "envy-bin"
        # Intersection of {"linux"} and {"darwin"} is empty — no scripts
        self.assertFalse(
            (bin_dir / "aptutil").exists(),
            "No POSIX script when intersection is empty",
        )
        self.assertFalse(
            (bin_dir / "aptutil.bat").exists(),
            "No Windows script when intersection is empty",
        )

    def test_constrained_package_with_unconstrained_dependency(self):
        """Platform-constrained package can depend on unconstrained package."""
        # cross_tool has no PLATFORMS (all platforms)
        # darwin_dep has PLATFORMS = {"darwin"} and depends on cross_tool
        cross_path = self.write_spec("cross_tool", SPEC_PRODUCT_ALL_PLATFORMS)

        # Spec that depends on cross_tool, with darwin-only constraint.
        # Can't use write_spec (double-brace escaping conflict), write directly.
        dep_spec = (
            'IDENTITY = "local.darwin_dep@v1"\n'
            'PLATFORMS = { "darwin" }\n'
            'PRODUCTS = { darwindep = "bin/darwindep" }\n'
            "\n"
            "DEPENDENCIES = {\n"
            '    { spec = "local.cross_tool@v1", source = "' + cross_path + '" },\n'
            "}\n"
            "\n"
            "FETCH = {\n"
            '  source = "' + self.archive_path.as_posix() + '",\n'
            '  sha256 = "' + self.archive_hash + '",\n'
            "}\n"
            "\n"
            "INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)\n"
            "end\n"
        )
        dep_spec_path = self.specs_dir / "darwin_dep.lua"
        dep_spec_path.write_text(dep_spec, encoding="utf-8")
        dep_path = dep_spec_path.as_posix()

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.darwin_dep@v1", source = "{dep_path}",
       platforms = {{ "darwin" }} }},
}}
""")

        result = self.run_deploy(manifest=manifest, platform="all")
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        bin_dir = self.test_dir / "envy-bin"

        # darwin-only package: posix yes, .bat no
        self.assertTrue(
            (bin_dir / "darwindep").exists(),
            "POSIX script for darwin-only package should exist",
        )
        self.assertFalse(
            (bin_dir / "darwindep.bat").exists(),
            "Windows script for darwin-only package should NOT exist",
        )

        # cross_tool (dependency): both scripts (unconstrained)
        self.assertTrue(
            (bin_dir / "cross").exists(),
            "POSIX script for unconstrained dependency should exist",
        )
        self.assertTrue(
            (bin_dir / "cross.bat").exists(),
            "Windows script for unconstrained dependency should exist",
        )

    def test_sync_after_deploy_does_not_remove_nonhost_scripts(self):
        """sync --platform all should not remove scripts deploy created for non-host packages."""
        linux_path = self.write_spec("linux_tool", SPEC_PRODUCT_LINUX_ONLY)
        cross_path = self.write_spec("cross_tool", SPEC_PRODUCT_ALL_PLATFORMS)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.linux_tool@v1", source = "{linux_path}",
       platforms = {{ "linux" }} }},
    {{ spec = "local.cross_tool@v1", source = "{cross_path}" }},
}}
""")

        # Deploy first — creates scripts for all packages including non-host
        result = self.run_deploy(manifest=manifest, platform="all")
        self.assertEqual(result.returncode, 0, f"deploy stderr: {result.stderr}")

        bin_dir = self.test_dir / "envy-bin"
        # linux-only product should have POSIX script after deploy
        self.assertTrue(
            (bin_dir / "aptutil").exists(),
            "POSIX script for linux-only product should exist after deploy",
        )

        # Sync should not remove the linux-only script
        result = self.run_sync(manifest=manifest, platform="all")
        self.assertEqual(result.returncode, 0, f"sync stderr: {result.stderr}")

        self.assertTrue(
            (bin_dir / "aptutil").exists(),
            "POSIX script for linux-only product should survive sync",
        )
        self.assertTrue(
            (bin_dir / "cross").exists(),
            "POSIX script for cross-platform product should exist after sync",
        )
        self.assertTrue(
            (bin_dir / "cross.bat").exists(),
            "Windows script for cross-platform product should exist after sync",
        )

    def test_sync_platform_all_creates_nonhost_scripts(self):
        """sync --platform all should create scripts for non-host packages."""
        linux_path = self.write_spec("linux_tool", SPEC_PRODUCT_LINUX_ONLY)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.linux_tool@v1", source = "{linux_path}",
       platforms = {{ "linux" }} }},
}}
""")

        result = self.run_sync(manifest=manifest, platform="all")
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        bin_dir = self.test_dir / "envy-bin"
        # linux maps to posix — script should be created
        self.assertTrue(
            (bin_dir / "aptutil").exists(),
            "POSIX script for linux-only product should exist after sync --platform all",
        )
        # linux does not map to windows — no .bat
        self.assertFalse(
            (bin_dir / "aptutil.bat").exists(),
            "Windows script for linux-only product should NOT exist",
        )


class TestSyncDeployDirective(EnvyTestCase):
    """Tests for @envy deploy directive behavior in 'envy sync'."""

    def setUp(self):
        super().setUp()
        self.test_dir = self.make_temp_dir("test_dir")
        self.specs_dir = self.make_temp_dir("specs_dir")

        # Create test archive and get its hash
        self.archive_path = self.specs_dir / "test.tar.gz"
        self.archive_hash = create_test_archive(self.archive_path)

    def write_spec(self, name: str, content: str) -> str:
        """Write spec to temp dir with placeholder substitution, return Lua path."""
        spec_content = content.format(
            ARCHIVE_PATH=self.archive_path.as_posix(),
            ARCHIVE_HASH=self.archive_hash,
        )
        path = self.specs_dir / f"{name}.lua"
        path.write_text(spec_content, encoding="utf-8")
        return path.as_posix()

    def create_manifest(self, content: str, deploy: Optional[str] = None) -> Path:
        """Create manifest with optional deploy directive."""
        manifest_path = self.test_dir / "envy.lua"
        header = '-- @envy bin "envy-bin"\n'
        if deploy is not None:
            header += f'-- @envy deploy "{deploy}"\n'
        manifest_path.write_text(header + content, encoding="utf-8")
        return manifest_path

    def run_sync(self, manifest: Path):
        cmd = [str(self.envy), "--cache-root", str(self.cache_root), "sync"]
        cmd.extend(["--manifest", str(manifest)])
        return test_config.run(
            cmd, cwd=self.project_root, capture_output=True, text=True
        )

    def run_deploy(self, manifest: Path):
        cmd = [str(self.envy), "--cache-root", str(self.cache_root), "deploy"]
        cmd.extend(["--manifest", str(manifest)])
        return test_config.run(
            cmd, cwd=self.project_root, capture_output=True, text=True
        )

    def run_install(self, manifest: Path):
        cmd = [str(self.envy), "--cache-root", str(self.cache_root), "install"]
        cmd.extend(["--manifest", str(manifest)])
        return test_config.run(
            cmd, cwd=self.project_root, capture_output=True, text=True
        )

    def test_sync_deploy_true_creates_scripts(self):
        """Sync with deploy=true creates product scripts."""
        product_path = self.write_spec("product_provider", SPEC_PRODUCT_PROVIDER)

        manifest = self.create_manifest(
            f"""
PACKAGES = {{
    {{ spec = "local.product_provider@v1", source = "{product_path}" }},
}}
""",
            deploy="true",
        )

        result = self.run_sync(manifest=manifest)
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        bin_dir = self.test_dir / "envy-bin"
        script_name = "tool.bat" if sys.platform == "win32" else "tool"
        script_path = bin_dir / script_name
        self.assertTrue(script_path.exists())

    def test_sync_deploy_false_no_scripts(self):
        """Sync with deploy=false does not create product scripts."""
        product_path = self.write_spec("product_provider", SPEC_PRODUCT_PROVIDER)

        manifest = self.create_manifest(
            f"""
PACKAGES = {{
    {{ spec = "local.product_provider@v1", source = "{product_path}" }},
}}
""",
            deploy="false",
        )

        result = self.run_sync(manifest=manifest)
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        bin_dir = self.test_dir / "envy-bin"
        script_name = "tool.bat" if sys.platform == "win32" else "tool"
        script_path = bin_dir / script_name
        self.assertFalse(script_path.exists())
        self.assertIn("deployment is disabled", result.stderr)

    def test_sync_deploy_absent_warns(self):
        """Naked sync with deploy absent warns user."""
        simple_path = self.write_spec("simple", SPEC_SIMPLE)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.simple@v1", source = "{simple_path}" }},
}}
""")  # No deploy directive

        result = self.run_sync(manifest=manifest)
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertIn("deployment is disabled", result.stderr)
        self.assertIn("@envy deploy", result.stderr)

    def test_install_no_deploy_warning(self):
        """Install does not warn about deploy directive."""
        simple_path = self.write_spec("simple", SPEC_SIMPLE)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.simple@v1", source = "{simple_path}" }},
}}
""")  # No deploy directive

        result = self.run_install(manifest=manifest)
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertNotIn("deployment is disabled", result.stderr)

    def test_deploy_deploy_true_creates_scripts(self):
        """Deploy standalone with deploy=true creates product scripts."""
        product_path = self.write_spec("product_provider", SPEC_PRODUCT_PROVIDER)

        manifest = self.create_manifest(
            f"""
PACKAGES = {{
    {{ spec = "local.product_provider@v1", source = "{product_path}" }},
}}
""",
            deploy="true",
        )

        result = self.run_deploy(manifest=manifest)
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        bin_dir = self.test_dir / "envy-bin"
        script_name = "tool.bat" if sys.platform == "win32" else "tool"
        script_path = bin_dir / script_name
        self.assertTrue(script_path.exists())

    def test_deploy_deploy_absent_warns(self):
        """Deploy standalone with deploy absent warns user."""
        simple_path = self.write_spec("simple", SPEC_SIMPLE)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.simple@v1", source = "{simple_path}" }},
}}
""")  # No deploy directive

        result = self.run_deploy(manifest=manifest)
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertIn("deployment is disabled", result.stderr)
        self.assertIn("@envy deploy", result.stderr)


class TestSyncBootstrap(EnvyTestCase):
    """Tests for bootstrap script update via 'envy sync'."""

    def setUp(self):
        super().setUp()
        self.test_dir = self.make_temp_dir("test_dir")
        self.specs_dir = self.make_temp_dir("specs_dir")

        # Create test archive and get its hash
        self.archive_path = self.specs_dir / "test.tar.gz"
        self.archive_hash = create_test_archive(self.archive_path)

    def write_spec(self, name: str, content: str) -> str:
        """Write spec to temp dir with placeholder substitution, return Lua path."""
        # Always call format() to handle {{}} escapes in Lua tables
        # Extra kwargs are silently ignored by Python's str.format()
        spec_content = content.format(
            ARCHIVE_PATH=self.archive_path.as_posix(),
            ARCHIVE_HASH=self.archive_hash,
        )
        path = self.specs_dir / f"{name}.lua"
        path.write_text(spec_content, encoding="utf-8")
        return path.as_posix()

    def create_manifest(self, content: str, deploy: bool = True) -> Path:
        manifest_path = self.test_dir / "envy.lua"
        manifest_path.write_text(
            make_manifest(content, deploy=deploy), encoding="utf-8"
        )
        return manifest_path

    def run_sync(self, manifest: Path, platform: Optional[str] = None):
        cmd = [str(self.envy), "--cache-root", str(self.cache_root), "sync"]
        if platform:
            cmd.extend(["--platform", platform])
        cmd.extend(["--manifest", str(manifest)])
        return test_config.run(
            cmd, cwd=self.project_root, capture_output=True, text=True
        )

    def run_deploy(self, manifest: Path, platform: Optional[str] = None):
        cmd = [str(self.envy), "--cache-root", str(self.cache_root), "deploy"]
        if platform:
            cmd.extend(["--platform", platform])
        cmd.extend(["--manifest", str(manifest)])
        return test_config.run(
            cmd, cwd=self.project_root, capture_output=True, text=True
        )

    def get_bootstrap_path(self) -> Path:
        bin_dir = self.test_dir / "envy-bin"
        script_name = "envy.bat" if sys.platform == "win32" else "envy"
        return bin_dir / script_name

    def test_sync_creates_bootstrap_if_missing(self):
        """Sync creates bootstrap script if it doesn't exist."""
        simple_path = self.write_spec("simple", SPEC_SIMPLE)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.simple@v1", source = "{simple_path}" }},
}}
""")

        # Ensure bin dir exists but bootstrap doesn't
        bin_dir = self.test_dir / "envy-bin"
        bin_dir.mkdir(parents=True, exist_ok=True)

        bootstrap_path = self.get_bootstrap_path()
        self.assertFalse(bootstrap_path.exists())

        result = self.run_sync(manifest=manifest)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertTrue(bootstrap_path.exists())
        content = bootstrap_path.read_text()
        self.assertIn("envy-managed", content)

    def _make_old_bootstrap_content(self) -> str:
        """Create platform-appropriate old bootstrap content with marker."""
        if sys.platform == "win32":
            return """@echo off
REM envy-managed bootstrap script - do not edit
set FALLBACK_VERSION=0.0.1
echo old bootstrap
"""
        else:
            return """#!/usr/bin/env bash
# envy-managed bootstrap script - do not edit
FALLBACK_VERSION="0.0.1"
echo "old bootstrap"
"""

    def test_sync_updates_bootstrap_on_version_change(self):
        """Sync updates bootstrap when version differs."""
        simple_path = self.write_spec("simple", SPEC_SIMPLE)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.simple@v1", source = "{simple_path}" }},
}}
""")

        bin_dir = self.test_dir / "envy-bin"
        bin_dir.mkdir(parents=True, exist_ok=True)

        bootstrap_path = self.get_bootstrap_path()
        # Create an old bootstrap with different version
        bootstrap_path.write_text(self._make_old_bootstrap_content())

        result = self.run_sync(manifest=manifest)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertIn("Updated bootstrap script", result.stderr)

        new_content = bootstrap_path.read_text()
        # Check for the full FALLBACK_VERSION assignment to avoid substring
        # matches (e.g. "0.0.15" containing "0.0.1"). Use platform-specific
        # patterns matching quote positions in POSIX vs Windows templates.
        if sys.platform == "win32":
            old_version_token = 'FALLBACK_VERSION=0.0.1"'
        else:
            old_version_token = 'FALLBACK_VERSION="0.0.1"'
        self.assertNotIn(old_version_token, new_content)
        self.assertIn("envy-managed", new_content)

    def test_sync_leaves_bootstrap_unchanged_when_current(self):
        """Sync does not rewrite bootstrap when content matches."""
        simple_path = self.write_spec("simple", SPEC_SIMPLE)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.simple@v1", source = "{simple_path}" }},
}}
""")

        # First sync to create bootstrap
        result1 = self.run_sync(manifest=manifest)
        self.assertEqual(result1.returncode, 0, f"stderr: {result1.stderr}")

        bootstrap_path = self.get_bootstrap_path()

        # Set mtime to a known past date to reliably detect rewrites
        jan_1_2000 = time.mktime((2000, 1, 1, 0, 0, 0, 0, 0, 0))
        os.utime(bootstrap_path, (jan_1_2000, jan_1_2000))

        # Second sync should not update
        result2 = self.run_sync(manifest=manifest)
        self.assertEqual(result2.returncode, 0, f"stderr: {result2.stderr}")
        self.assertNotIn("Updated bootstrap script", result2.stderr)

        mtime_after = bootstrap_path.stat().st_mtime
        self.assertEqual(mtime_after, jan_1_2000, "Bootstrap should not be rewritten")

    def test_sync_errors_on_non_envy_managed_bootstrap(self):
        """Sync errors if bootstrap exists but is not envy-managed."""
        simple_path = self.write_spec("simple", SPEC_SIMPLE)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.simple@v1", source = "{simple_path}" }},
}}
""")

        bin_dir = self.test_dir / "envy-bin"
        bin_dir.mkdir(parents=True, exist_ok=True)

        bootstrap_path = self.get_bootstrap_path()
        # Create a non-envy-managed file
        bootstrap_path.write_text("#!/bin/bash\necho 'user script'\n")

        result = self.run_sync(manifest=manifest)

        self.assertNotEqual(result.returncode, 0, "Expected non-zero exit code")
        self.assertIn("not envy-managed", result.stderr.lower())

    @unittest.skipIf(sys.platform == "win32", "Unix permissions test")
    def test_bootstrap_has_executable_permissions(self):
        """Bootstrap script has executable permissions on Unix."""
        simple_path = self.write_spec("simple", SPEC_SIMPLE)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.simple@v1", source = "{simple_path}" }},
}}
""")

        result = self.run_sync(manifest=manifest)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        bootstrap_path = self.get_bootstrap_path()
        self.assertTrue(bootstrap_path.exists())

        import stat

        mode = bootstrap_path.stat().st_mode
        self.assertTrue(mode & stat.S_IXUSR, "Owner execute bit should be set")
        self.assertTrue(mode & stat.S_IXGRP, "Group execute bit should be set")
        self.assertTrue(mode & stat.S_IXOTH, "Others execute bit should be set")

    def test_sync_updates_bootstrap_regardless_of_deploy_directive(self):
        """Bootstrap update happens even when deploy is disabled."""
        simple_path = self.write_spec("simple", SPEC_SIMPLE)

        # Create manifest with deploy=false
        manifest_path = self.test_dir / "envy.lua"
        manifest_path.write_text(
            f'-- @envy bin "envy-bin"\n-- @envy deploy "false"\nPACKAGES = {{\n'
            f'    {{ spec = "local.simple@v1", source = "{simple_path}" }},\n}}\n',
            encoding="utf-8",
        )

        bin_dir = self.test_dir / "envy-bin"
        bin_dir.mkdir(parents=True, exist_ok=True)

        bootstrap_path = self.get_bootstrap_path()
        # Create an old bootstrap
        bootstrap_path.write_text(self._make_old_bootstrap_content())

        cmd = [str(self.envy), "--cache-root", str(self.cache_root), "sync"]
        cmd.extend(["--manifest", str(manifest_path)])
        result = test_config.run(
            cmd, cwd=self.project_root, capture_output=True, text=True
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        # Bootstrap should still be updated even though deploy is false
        self.assertIn("Updated bootstrap script", result.stderr)

        new_content = bootstrap_path.read_text()
        # Check for the full FALLBACK_VERSION assignment to avoid substring
        # matches (e.g. "0.0.15" containing "0.0.1"). Use platform-specific
        # patterns matching quote positions in POSIX vs Windows templates.
        if sys.platform == "win32":
            old_version_token = 'FALLBACK_VERSION=0.0.1"'
        else:
            old_version_token = 'FALLBACK_VERSION="0.0.1"'
        self.assertNotIn(old_version_token, new_content)

    def test_sync_platform_all_creates_both_bootstraps(self):
        """--platform all creates both POSIX and Windows bootstrap scripts."""
        simple_path = self.write_spec("simple", SPEC_SIMPLE)

        manifest = self.create_manifest(f"""
PACKAGES = {{
    {{ spec = "local.simple@v1", source = "{simple_path}" }},
}}
""")

        result = self.run_sync(manifest=manifest, platform="all")
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        bin_dir = self.test_dir / "envy-bin"
        self.assertTrue((bin_dir / "envy").exists(), "POSIX bootstrap missing")
        self.assertTrue((bin_dir / "envy.bat").exists(), "Windows bootstrap missing")

        posix_content = (bin_dir / "envy").read_text()
        windows_content = (bin_dir / "envy.bat").read_text()
        self.assertIn("envy-managed", posix_content)
        self.assertIn("envy-managed", windows_content)

    def test_init_then_sync_preserves_bootstrap_mtime(self):
        """Init creates bootstrap, sync preserves it when unchanged (mtime test)."""
        envy_main = test_config.get_envy_production_executable()
        env = test_config.get_test_env()
        env["ENVY_CACHE_ROOT"] = str(self.cache_root)

        project_dir = self.test_dir / "init-project"
        bin_dir = project_dir / "bin"

        # Run envy init to create bootstrap and manifest
        init_cmd = [str(envy_main), "init", str(project_dir), str(bin_dir)]
        init_result = test_config.run(
            init_cmd, capture_output=True, text=True, env=env, timeout=30
        )
        self.assertEqual(
            init_result.returncode, 0, f"init stderr: {init_result.stderr}"
        )

        # Get bootstrap path
        bootstrap_name = "envy.bat" if sys.platform == "win32" else "envy"
        bootstrap_path = bin_dir / bootstrap_name
        self.assertTrue(bootstrap_path.exists(), "Bootstrap not created by init")

        # Set file time to Jan 1, 2000 (a date clearly in the past)
        jan_1_2000 = time.mktime((2000, 1, 1, 0, 0, 0, 0, 0, 0))
        os.utime(bootstrap_path, (jan_1_2000, jan_1_2000))

        # Verify mtime was set
        mtime_before = bootstrap_path.stat().st_mtime
        self.assertEqual(mtime_before, jan_1_2000, "Failed to set mtime to Jan 1, 2000")

        # Run envy sync
        manifest_path = project_dir / "envy.lua"
        sync_cmd = [
            str(self.envy),
            "--cache-root",
            str(self.cache_root),
            "sync",
            "--manifest",
            str(manifest_path),
        ]
        sync_result = test_config.run(
            sync_cmd, cwd=self.project_root, capture_output=True, text=True
        )
        self.assertEqual(
            sync_result.returncode, 0, f"sync stderr: {sync_result.stderr}"
        )

        # Bootstrap should NOT be updated (content unchanged)
        self.assertNotIn("Updated bootstrap script", sync_result.stderr)

        # Verify mtime is still Jan 1, 2000
        mtime_after = bootstrap_path.stat().st_mtime
        self.assertEqual(
            mtime_after,
            jan_1_2000,
            f"Bootstrap mtime changed from {mtime_before} to {mtime_after}; "
            "sync should not rewrite when content unchanged",
        )


if __name__ == "__main__":
    unittest.main()
