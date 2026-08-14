"""Functional tests for transitive product provision."""

import hashlib
import io
import subprocess
import tarfile
import unittest
from pathlib import Path

from . import test_config
from .env import EnvyTestCase
from .test_config import make_manifest

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


class TestProductTransitive(EnvyTestCase):
    """Test transitive product provision validation."""

    def setUp(self):
        super().setUp()
        self.test_dir = self.make_temp_dir("test_dir")
        self.specs_dir = self.make_temp_dir("specs_dir")

        # Create test archive and get its hash
        self.archive_path = self.specs_dir / "test.tar.gz"
        self.archive_hash = create_test_archive(self.archive_path)

    def write_spec(self, name: str, content: str) -> Path:
        """Write a spec file with {ARCHIVE_PATH} and {ARCHIVE_HASH} substituted."""
        spec_content = content.format(
            ARCHIVE_PATH=self.archive_path.as_posix(),
            ARCHIVE_HASH=self.archive_hash,
        )
        path = self.specs_dir / name
        path.write_text(spec_content, encoding="utf-8")
        return path

    def manifest(self, content: str) -> Path:
        manifest_path = self.test_dir / "envy.lua"
        manifest_path.write_text(make_manifest(content), encoding="utf-8")
        return manifest_path

    def run_envy(self, args):
        cmd = [str(self.envy), "--cache-root", str(self.cache_root), *args]
        return test_config.run(
            cmd, cwd=self.project_root, capture_output=True, text=True
        )

    def test_transitive_provision_chain_success(self):
        """Weak dep fallback transitively provides via dependency chain (A→B→C, C provides)."""
        # Leaf node that actually provides the product
        provider_spec = """
IDENTITY = "local.product_transitive_provider@v1"

PRODUCTS = {{
    tool = "bin/actual_tool"
}}

FETCH = {{
    source = "{ARCHIVE_PATH}",
    sha256 = "{ARCHIVE_HASH}",
}}

INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
end
"""
        provider_path = self.write_spec(
            "product_transitive_provider.lua", provider_spec
        )

        # Middle node that depends on the actual provider
        intermediate_spec = """
IDENTITY = "local.product_transitive_intermediate@v1"

-- Does NOT provide "tool" directly, but transitively via dependency
DEPENDENCIES = {{
    {{
        spec = "local.product_transitive_provider@v1",
        source = "{provider_path}",
    }}
}}

FETCH = {{
    source = "{ARCHIVE_PATH}",
    sha256 = "{ARCHIVE_HASH}",
}}

INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
end
""".replace("{provider_path}", provider_path.as_posix())
        intermediate_path = self.write_spec(
            "product_transitive_intermediate.lua", intermediate_spec
        )

        # Root node with weak product dependency on "tool"
        root_spec = """
IDENTITY = "local.product_transitive_root@v1"

DEPENDENCIES = {{
    {{
        product = "tool",
        weak = {{
            spec = "local.product_transitive_intermediate@v1",
            source = "{intermediate_path}",
        }}
    }}
}}

FETCH = {{
    source = "{ARCHIVE_PATH}",
    sha256 = "{ARCHIVE_HASH}",
}}

INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
    -- If fallback was used, we have intermediate; if provider was in manifest, we don't
    -- Either way, the transitive provision validation should have ensured correctness
end
""".replace("{intermediate_path}", intermediate_path.as_posix())
        root_path = self.write_spec("product_transitive_root.lua", root_spec)

        manifest = self.manifest(
            f"""
PACKAGES = {{
  {{
    spec = "local.product_transitive_root@v1",
    source = "{root_path.as_posix()}",
  }},
}}
"""
        )

        result = self.run_envy(["install", "--manifest", str(manifest)])
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        # Verify all three specs were installed
        root_dir = self.cache_root / "packages" / "local.product_transitive_root@v1"
        intermediate_dir = (
            self.cache_root / "packages" / "local.product_transitive_intermediate@v1"
        )
        provider_dir = (
            self.cache_root / "packages" / "local.product_transitive_provider@v1"
        )

        self.assertTrue(root_dir.exists(), f"missing {root_dir}")
        self.assertTrue(intermediate_dir.exists(), f"missing {intermediate_dir}")
        self.assertTrue(provider_dir.exists(), f"missing {provider_dir}")

    def test_fallback_doesnt_transitively_provide_error(self):
        """Weak dep fallback that doesn't transitively provide should fail validation."""
        # Middle node that does NOT provide "tool" and has no dependencies
        # Used to test validation failure when fallback doesn't transitively provide
        intermediate_no_provide_spec = """
IDENTITY = "local.product_transitive_intermediate_no_provide@v1"

-- No PRODUCTS, no DEPENDENCIES - cannot provide "tool" transitively

FETCH = {{
    source = "{ARCHIVE_PATH}",
    sha256 = "{ARCHIVE_HASH}",
}}

INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
end
"""
        intermediate_no_provide_path = self.write_spec(
            "product_transitive_intermediate_no_provide.lua",
            intermediate_no_provide_spec,
        )

        # Root node with weak product dependency that will fail validation
        root_fail_spec = """
IDENTITY = "local.product_transitive_root_fail@v1"

DEPENDENCIES = {{
    {{
        product = "tool",
        weak = {{
            spec = "local.product_transitive_intermediate_no_provide@v1",
            source = "{intermediate_no_provide_path}",
        }}
    }}
}}

FETCH = {{
    source = "{ARCHIVE_PATH}",
    sha256 = "{ARCHIVE_HASH}",
}}

INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
end
""".replace("{intermediate_no_provide_path}", intermediate_no_provide_path.as_posix())
        root_fail_path = self.write_spec(
            "product_transitive_root_fail.lua", root_fail_spec
        )

        manifest = self.manifest(
            f"""
PACKAGES = {{
  {{
    spec = "local.product_transitive_root_fail@v1",
    source = "{root_fail_path.as_posix()}",
  }},
}}
"""
        )

        result = self.run_envy(["install", "--manifest", str(manifest)])
        self.assertNotEqual(
            result.returncode, 0, "Expected failure for non-transitive fallback"
        )

        # Should mention validation failure and the product name
        stderr_lower = result.stderr.lower()
        self.assertTrue(
            "tool" in stderr_lower
            and ("does not provide" in stderr_lower or "fallback" in stderr_lower),
            f"Expected validation error mentioning product 'tool', got: {result.stderr}",
        )

    def test_fallback_transitively_provides_via_dependency(self):
        """Fallback with dependency that provides product should pass validation."""
        # Leaf node that actually provides the product
        provider_spec = """
IDENTITY = "local.product_transitive_provider@v1"

PRODUCTS = {{
    tool = "bin/actual_tool"
}}

FETCH = {{
    source = "{ARCHIVE_PATH}",
    sha256 = "{ARCHIVE_HASH}",
}}

INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
end
"""
        provider_path = self.write_spec(
            "product_transitive_provider.lua", provider_spec
        )

        # Middle node that depends on the actual provider
        intermediate_spec = """
IDENTITY = "local.product_transitive_intermediate@v1"

-- Does NOT provide "tool" directly, but transitively via dependency
DEPENDENCIES = {{
    {{
        spec = "local.product_transitive_provider@v1",
        source = "{provider_path}",
    }}
}}

FETCH = {{
    source = "{ARCHIVE_PATH}",
    sha256 = "{ARCHIVE_HASH}",
}}

INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
end
""".replace("{provider_path}", provider_path.as_posix())
        intermediate_path = self.write_spec(
            "product_transitive_intermediate.lua", intermediate_spec
        )

        # Root node with weak product dependency on "tool"
        root_spec = """
IDENTITY = "local.product_transitive_root@v1"

DEPENDENCIES = {{
    {{
        product = "tool",
        weak = {{
            spec = "local.product_transitive_intermediate@v1",
            source = "{intermediate_path}",
        }}
    }}
}}

FETCH = {{
    source = "{ARCHIVE_PATH}",
    sha256 = "{ARCHIVE_HASH}",
}}

INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
    -- If fallback was used, we have intermediate; if provider was in manifest, we don't
    -- Either way, the transitive provision validation should have ensured correctness
end
""".replace("{intermediate_path}", intermediate_path.as_posix())
        root_path = self.write_spec("product_transitive_root.lua", root_spec)

        manifest = self.manifest(
            f"""
PACKAGES = {{
  {{
    spec = "local.product_transitive_root@v1",
    source = "{root_path.as_posix()}",
  }},
}}
"""
        )

        result = self.run_envy(["install", "--manifest", str(manifest)])
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        # Verify provider was installed (intermediate doesn't provide, but its dep does)
        provider_dir = (
            self.cache_root / "packages" / "local.product_transitive_provider@v1"
        )
        self.assertTrue(
            provider_dir.exists(),
            "Transitive provider should be installed via fallback dependency",
        )

    def test_transitive_provision_with_existing_provider(self):
        """When provider exists in manifest, weak dep should use it instead of fallback."""
        # Leaf node that actually provides the product
        provider_spec = """
IDENTITY = "local.product_transitive_provider@v1"

PRODUCTS = {{
    tool = "bin/actual_tool"
}}

FETCH = {{
    source = "{ARCHIVE_PATH}",
    sha256 = "{ARCHIVE_HASH}",
}}

INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
end
"""
        provider_path = self.write_spec(
            "product_transitive_provider.lua", provider_spec
        )

        # Middle node that depends on the actual provider
        intermediate_spec = """
IDENTITY = "local.product_transitive_intermediate@v1"

-- Does NOT provide "tool" directly, but transitively via dependency
DEPENDENCIES = {{
    {{
        spec = "local.product_transitive_provider@v1",
        source = "{provider_path}",
    }}
}}

FETCH = {{
    source = "{ARCHIVE_PATH}",
    sha256 = "{ARCHIVE_HASH}",
}}

INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
end
""".replace("{provider_path}", provider_path.as_posix())
        intermediate_path = self.write_spec(
            "product_transitive_intermediate.lua", intermediate_spec
        )

        # Root node with weak product dependency on "tool"
        root_spec = """
IDENTITY = "local.product_transitive_root@v1"

DEPENDENCIES = {{
    {{
        product = "tool",
        weak = {{
            spec = "local.product_transitive_intermediate@v1",
            source = "{intermediate_path}",
        }}
    }}
}}

FETCH = {{
    source = "{ARCHIVE_PATH}",
    sha256 = "{ARCHIVE_HASH}",
}}

INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
    -- If fallback was used, we have intermediate; if provider was in manifest, we don't
    -- Either way, the transitive provision validation should have ensured correctness
end
""".replace("{intermediate_path}", intermediate_path.as_posix())
        root_path = self.write_spec("product_transitive_root.lua", root_spec)

        manifest = self.manifest(
            f"""
PACKAGES = {{
  {{
    spec = "local.product_transitive_provider@v1",
    source = "{provider_path.as_posix()}",
  }},
  {{
    spec = "local.product_transitive_root@v1",
    source = "{root_path.as_posix()}",
  }},
}}
"""
        )

        result = self.run_envy(["install", "--manifest", str(manifest)])
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        # Both provider and root should exist
        # Intermediate might not be installed since provider was found directly
        root_dir = self.cache_root / "packages" / "local.product_transitive_root@v1"
        provider_dir = (
            self.cache_root / "packages" / "local.product_transitive_provider@v1"
        )

        self.assertTrue(root_dir.exists(), f"missing {root_dir}")
        self.assertTrue(provider_dir.exists(), f"missing {provider_dir}")

    def test_deep_transitive_chain(self):
        """Verify transitive provision works through multiple levels."""
        # Leaf node that actually provides the product
        provider_spec = """
IDENTITY = "local.product_transitive_provider@v1"

PRODUCTS = {{
    tool = "bin/actual_tool"
}}

FETCH = {{
    source = "{ARCHIVE_PATH}",
    sha256 = "{ARCHIVE_HASH}",
}}

INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
end
"""
        provider_path = self.write_spec(
            "product_transitive_provider.lua", provider_spec
        )

        # Create a 4-level chain: consumer -> mid1 -> mid2 -> provider
        # Level 3: mid2 depends on provider
        mid2_spec = """
IDENTITY = "local.transitive_mid2@v1"

DEPENDENCIES = {{
  {{
    spec = "local.product_transitive_provider@v1",
    source = "{provider_path}",
  }}
}}

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}",
}}

INSTALL = function(ctx)
end
""".replace("{provider_path}", provider_path.as_posix())
        mid2_path = self.write_spec("transitive_mid2.lua", mid2_spec)

        # Level 2: mid1 depends on mid2
        mid1_spec = """
IDENTITY = "local.transitive_mid1@v1"

DEPENDENCIES = {{
  {{
    spec = "local.transitive_mid2@v1",
    source = "{mid2_path}",
  }}
}}

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}",
}}

INSTALL = function(ctx)
end
""".replace("{mid2_path}", mid2_path.as_posix())
        mid1_path = self.write_spec("transitive_mid1.lua", mid1_spec)

        # Level 1: consumer has weak dep on "tool" with fallback to mid1
        consumer_spec = """
IDENTITY = "local.transitive_consumer@v1"

DEPENDENCIES = {{
  {{
    product = "tool",
    weak = {{
      spec = "local.transitive_mid1@v1",
      source = "{mid1_path}",
    }}
  }}
}}

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}",
}}

INSTALL = function(ctx)
end
""".replace("{mid1_path}", mid1_path.as_posix())
        consumer_path = self.write_spec("transitive_consumer.lua", consumer_spec)

        manifest = self.manifest(
            f"""
PACKAGES = {{
  {{
    spec = "local.transitive_consumer@v1",
    source = "{consumer_path.as_posix()}",
  }},
}}
"""
        )

        result = self.run_envy(["install", "--manifest", str(manifest)])
        self.assertEqual(
            result.returncode,
            0,
            f"Deep transitive chain should succeed. stderr: {result.stderr}",
        )

        # Verify provider at the end was reached
        provider_dir = (
            self.cache_root / "packages" / "local.product_transitive_provider@v1"
        )
        self.assertTrue(
            provider_dir.exists(), "Provider should be installed via deep chain"
        )


if __name__ == "__main__":
    unittest.main()
