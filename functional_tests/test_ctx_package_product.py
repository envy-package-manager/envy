"""Functional coverage for envy.package() and envy.product() enforcement."""

import hashlib
import io
import shutil
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path

from . import test_config
from .env import EnvyTestCase
from .test_config import make_manifest

# Test archive contents
TEST_ARCHIVE_FILES = {
    "root/file1.txt": "Root file content\n",
    "root/file2.txt": "Another root file\n",
    "root/subdir1/file3.txt": "Subdir file content\n",
    "root/subdir1/subdir2/file4.txt": "Deep nested file\n",
    "root/subdir1/subdir2/file5.txt": "Another deep file\n",
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
# Shared provider specs (used by multiple tests)
# =============================================================================

# Cache-managed package that other specs can depend on via envy.package()
SPEC_PACKAGE_PROVIDER = """IDENTITY = "local.ctx_package_provider@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}",
}}

INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
end
"""

# Product provider exposing 'tool' product at bin/tool
SPEC_PRODUCT_PROVIDER = """IDENTITY = "local.product_provider@v1"
PRODUCTS = {{ tool = "bin/tool" }}

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}",
}}

INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
end
"""


class TestCtxPackageProduct(EnvyTestCase):
    def setUp(self):
        self.cache_root = self.make_temp_dir("cache_root")
        self.test_dir = self.make_temp_dir("test_dir")
        self.specs_dir = self.make_temp_dir("specs_dir")
        self.envy = test_config.get_envy_executable()
        self.project_root = Path(__file__).parent.parent

        # Create test archive and get its hash
        self.archive_path = self.specs_dir / "test.tar.gz"
        self.archive_hash = create_test_archive(self.archive_path)

    def tearDown(self):
        shutil.rmtree(self.cache_root, ignore_errors=True)
        shutil.rmtree(self.test_dir, ignore_errors=True)
        shutil.rmtree(self.specs_dir, ignore_errors=True)

    def manifest(self, content: str) -> Path:
        manifest_path = self.test_dir / "envy.lua"
        manifest_path.write_text(make_manifest(content), encoding="utf-8")
        return manifest_path

    def run_envy(self, args):
        cmd = [str(self.envy), "--cache-root", str(self.cache_root), *args]
        return test_config.run(
            cmd, cwd=self.project_root, capture_output=True, text=True
        )

    def write_spec(self, name: str, content: str) -> str:
        """Write spec to temp dir, return Lua path string."""
        spec_content = content.format(
            ARCHIVE_PATH=self.archive_path.as_posix(),
            ARCHIVE_HASH=self.archive_hash,
            SPECS_DIR=self.specs_dir.as_posix(),
        )
        path = self.specs_dir / f"{name}.lua"
        path.write_text(spec_content, encoding="utf-8")
        return path.as_posix()

    # =========================================================================
    # ctx.package tests
    # =========================================================================

    def test_ctx_package_success(self):
        """envy.package() succeeds when dependency has correct needed_by."""
        # Consumer accesses provider via envy.package() with needed_by=stage
        spec_consumer = """IDENTITY = "local.ctx_package_consumer_ok@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}",
}}

DEPENDENCIES = {{
  {{ spec = "local.ctx_package_provider@v1", source = "{SPECS_DIR}/provider.lua", needed_by = "stage" }},
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  local path = envy.package("local.ctx_package_provider@v1")
  assert(path:match("ctx_package_provider"), "package path should include provider identity")
end

INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
end
"""
        provider_path = self.write_spec("provider", SPEC_PACKAGE_PROVIDER)
        consumer_path = self.write_spec("consumer", spec_consumer)

        manifest = self.manifest(f"""
PACKAGES = {{
  {{ spec = "local.ctx_package_provider@v1", source = "{provider_path}" }},
  {{ spec = "local.ctx_package_consumer_ok@v1", source = "{consumer_path}" }},
}}
""")

        result = self.run_envy(["install", "--manifest", str(manifest)])
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_ctx_package_needed_by_violation(self):
        """envy.package() fails when accessed before needed_by phase."""
        # Consumer accesses provider during stage but needed_by=install
        spec_consumer = """IDENTITY = "local.ctx_package_needed_by_violation@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}",
}}

DEPENDENCIES = {{
  {{ spec = "local.ctx_package_provider@v1", source = "{SPECS_DIR}/provider.lua", needed_by = "install" }},
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.package("local.ctx_package_provider@v1")
end

INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
end
"""
        provider_path = self.write_spec("provider", SPEC_PACKAGE_PROVIDER)
        consumer_path = self.write_spec("consumer", spec_consumer)

        manifest = self.manifest(f"""
PACKAGES = {{
  {{ spec = "local.ctx_package_provider@v1", source = "{provider_path}" }},
  {{ spec = "local.ctx_package_needed_by_violation@v1", source = "{consumer_path}" }},
}}
""")

        result = self.run_envy(["install", "--manifest", str(manifest)])
        self.assertNotEqual(
            result.returncode, 0, "expected needed_by violation to fail"
        )
        self.assertIn("needed_by 'install' but accessed during 'stage'", result.stderr)

    def test_ctx_package_user_managed_fails(self):
        """envy.package() fails for user-managed dependencies (no pkg path)."""
        # User-managed provider: CHECK returns true, no persistent cache path
        spec_user_provider = """IDENTITY = "local.ctx_package_user_provider@v1"

USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      return true
    end,
    INSTALL = function(pkg_dir, options)
    end,
  }},
}}

"""
        # Consumer tries to access user-managed package
        spec_consumer = """IDENTITY = "local.ctx_package_user_consumer@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}",
}}

DEPENDENCIES = {{
  {{ spec = "local.ctx_package_user_provider@v1", source = "{SPECS_DIR}/user_provider.lua", needed_by = "stage" }},
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.package("local.ctx_package_user_provider@v1")
end

INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
end
"""
        user_provider_path = self.write_spec("user_provider", spec_user_provider)
        consumer_path = self.write_spec("consumer", spec_consumer)

        manifest = self.manifest(f"""
PACKAGES = {{
  {{ spec = "local.ctx_package_user_provider@v1", source = "{user_provider_path}" }},
  {{ spec = "local.ctx_package_user_consumer@v1", source = "{consumer_path}" }},
}}
""")

        result = self.run_envy(["install", "--manifest", str(manifest)])
        self.assertNotEqual(
            result.returncode, 0, "expected user-managed package access to fail"
        )
        self.assertIn("is user-managed and has no pkg path", result.stderr)

    def test_ctx_package_missing_dependency(self):
        """envy.package() fails for undeclared dependencies."""
        # Consumer calls envy.package() for a dependency it didn't declare
        spec_consumer = """IDENTITY = "local.ctx_package_missing_dep@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}",
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.package("local.nonexistent_dep@v1")
end

INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
end
"""
        consumer_path = self.write_spec("consumer", spec_consumer)

        manifest = self.manifest(f"""
PACKAGES = {{
  {{ spec = "local.ctx_package_missing_dep@v1", source = "{consumer_path}" }},
}}
""")

        result = self.run_envy(["install", "--manifest", str(manifest)])
        self.assertNotEqual(result.returncode, 0, "expected missing dependency to fail")
        self.assertIn("has no strong dependency", result.stderr)

    # =========================================================================
    # ctx.product tests
    # =========================================================================

    def test_ctx_product_success(self):
        """envy.product() succeeds when product dependency has correct needed_by."""
        # Consumer accesses 'tool' product with needed_by=stage
        spec_consumer = """IDENTITY = "local.ctx_product_consumer_ok@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}",
}}

DEPENDENCIES = {{
  {{
    spec = "local.product_provider@v1",
    source = "{SPECS_DIR}/product_provider.lua",
    product = "tool",
    needed_by = "stage",
  }},
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  local val = envy.product("tool")
  assert(val:match("bin/tool"), "expected product path")
end

INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
end
"""
        provider_path = self.write_spec("product_provider", SPEC_PRODUCT_PROVIDER)
        consumer_path = self.write_spec("consumer", spec_consumer)

        manifest = self.manifest(f"""
PACKAGES = {{
  {{ spec = "local.product_provider@v1", source = "{provider_path}" }},
  {{ spec = "local.ctx_product_consumer_ok@v1", source = "{consumer_path}" }},
}}
""")

        result = self.run_envy(["install", "--manifest", str(manifest)])
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_ctx_product_needed_by_violation(self):
        """envy.product() fails when accessed before needed_by phase."""
        # Consumer accesses 'tool' product during stage but needed_by=install
        spec_consumer = """IDENTITY = "local.ctx_product_needed_by_violation@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}",
}}

DEPENDENCIES = {{
  {{
    spec = "local.product_provider@v1",
    source = "{SPECS_DIR}/product_provider.lua",
    product = "tool",
    needed_by = "install",
  }},
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.product("tool")
end

INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
end
"""
        provider_path = self.write_spec("product_provider", SPEC_PRODUCT_PROVIDER)
        consumer_path = self.write_spec("consumer", spec_consumer)

        manifest = self.manifest(f"""
PACKAGES = {{
  {{ spec = "local.product_provider@v1", source = "{provider_path}" }},
  {{ spec = "local.ctx_product_needed_by_violation@v1", source = "{consumer_path}" }},
}}
""")

        result = self.run_envy(["install", "--manifest", str(manifest)])
        self.assertNotEqual(
            result.returncode, 0, "expected product needed_by violation to fail"
        )
        self.assertIn("needed_by 'install' but accessed during 'stage'", result.stderr)

    def test_ctx_product_missing_dependency(self):
        """envy.product() fails when product dependency not declared."""
        # Consumer calls envy.product() without declaring a product dependency
        spec_consumer = """IDENTITY = "local.ctx_product_missing_dep@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}",
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.product("tool")
end

INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
end
"""
        consumer_path = self.write_spec("consumer", spec_consumer)

        manifest = self.manifest(f"""
PACKAGES = {{
  {{ spec = "local.ctx_product_missing_dep@v1", source = "{consumer_path}" }},
}}
""")

        result = self.run_envy(["install", "--manifest", str(manifest)])
        self.assertNotEqual(
            result.returncode, 0, "expected missing product dependency to fail"
        )
        self.assertIn("does not declare product dependency", result.stderr)


if __name__ == "__main__":
    unittest.main()
