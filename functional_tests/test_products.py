"""Functional tests for products feature."""

import hashlib
import io
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


# Shared spec: Product provider with cached package - used by multiple tests
SPEC_PRODUCT_PROVIDER = """-- Product provider with cached package
IDENTITY = "local.product_provider@v1"
PRODUCTS = {{ tool = "bin/tool" }}

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}",
}}

INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  -- No real payload needed; just mark complete to populate pkg_path
end
"""

# Shared spec: Programmatic provider (user-managed) returning raw product value
SPEC_PRODUCT_PROGRAMMATIC = """-- Programmatic provider (user-managed) returning raw product value
IDENTITY = "local.product_programmatic@v1"
PRODUCTS = {{ tool = "programmatic-tool" }}

USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      return true  -- Already satisfied; no cache artifact
    end,
    INSTALL = function(pkg_dir, options)
      -- User-managed; no cache artifact
    end,
  }},
}}

"""

# Shared spec: Consumer with ref-only product dependency (no recipe/source, unconstrained)
SPEC_REF_ONLY_CONSUMER = """-- Consumer with ref-only product dependency (no recipe/source, unconstrained)
IDENTITY = "local.ref_only_consumer@v1"

DEPENDENCIES = {{
  {{
    product = "tool",
    -- No spec, no source - resolves to ANY provider of "tool"
  }},
}}

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}",
}}

INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
end
"""


class TestProducts(EnvyTestCase):
    """End-to-end tests for product providers and consumers."""

    def setUp(self):
        super().setUp()
        self.test_dir = self.make_temp_dir("test_dir")
        self.specs_dir = self.make_temp_dir("specs_dir")

        # Create test archive and get its hash
        self.archive_path = self.specs_dir / "test.tar.gz"
        self.archive_hash = create_test_archive(self.archive_path)

        # Write shared specs to temp directory with placeholders substituted
        self._write_spec("product_provider.lua", SPEC_PRODUCT_PROVIDER)
        self._write_spec("product_provider_programmatic.lua", SPEC_PRODUCT_PROGRAMMATIC)
        self._write_spec("product_ref_only_consumer.lua", SPEC_REF_ONLY_CONSUMER)

    def _write_spec(self, name: str, content: str) -> Path:
        """Write a spec file with archive placeholders substituted."""
        spec_content = content.format(
            ARCHIVE_PATH=self.archive_path.as_posix(),
            ARCHIVE_HASH=self.archive_hash,
        )
        path = self.specs_dir / name
        path.write_text(spec_content, encoding="utf-8")
        return path

    def lua_path(self, name: str) -> str:
        return (self.specs_dir / name).as_posix()

    def manifest(self, content: str) -> Path:
        manifest_path = self.test_dir / "envy.lua"
        manifest_path.write_text(make_manifest(content), encoding="utf-8")
        return manifest_path

    def run_envy(self, args):
        cmd = [str(self.envy), "--cache-root", str(self.cache_root), *args]
        return test_config.run(
            cmd, cwd=self.project_root, capture_output=True, text=True
        )

    def test_strong_product_dependency_resolves_provider(self):
        # Consumer with strong product dependency
        spec_consumer_strong = """-- Consumer with strong product dependency
IDENTITY = "local.product_consumer_strong@v1"

DEPENDENCIES = {{
  {{
    product = "tool",
    spec = "local.product_provider@v1",
    source = "product_provider.lua",
  }},
}}

INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
end

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}",
}}
"""
        self._write_spec("product_consumer_strong.lua", spec_consumer_strong)

        manifest = self.manifest(
            f"""
PACKAGES = {{
  {{
    spec = "local.product_consumer_strong@v1",
    source = "{self.lua_path("product_consumer_strong.lua")}",
  }},
}}
"""
        )

        result = self.run_envy(["install", "--manifest", str(manifest)])
        self.assertEqual(result.returncode, 0, result.stderr)

        provider_dir = self.cache_root / "packages" / "local.product_provider@v1"
        consumer_dir = self.cache_root / "packages" / "local.product_consumer_strong@v1"
        self.assertTrue(provider_dir.exists(), f"missing {provider_dir}")
        self.assertTrue(consumer_dir.exists(), f"missing {consumer_dir}")

    def test_weak_product_dependency_uses_fallback(self):
        # Consumer with weak product dependency (fallback)
        spec_consumer_weak = """-- Consumer with weak product dependency (fallback)
IDENTITY = "local.product_consumer_weak@v1"

DEPENDENCIES = {{
  {{
    product = "tool",
    spec = "local.product_provider@v1",
    weak = {{
      spec = "local.product_provider@v1",
      source = "product_provider.lua",
    }},
  }},
}}

INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
end

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}",
}}
"""
        self._write_spec("product_consumer_weak.lua", spec_consumer_weak)

        manifest = self.manifest(
            f"""
PACKAGES = {{
  {{
    spec = "local.product_consumer_weak@v1",
    source = "{self.lua_path("product_consumer_weak.lua")}",
  }},
}}
"""
        )

        result = self.run_envy(["install", "--manifest", str(manifest)])
        self.assertEqual(result.returncode, 0, result.stderr)

        provider_dir = self.cache_root / "packages" / "local.product_provider@v1"
        self.assertTrue(provider_dir.exists(), f"missing {provider_dir}")

    def test_missing_product_dependency_errors(self):
        # Consumer with missing product dependency (no fallback)
        spec_consumer_missing = """-- Consumer with missing product dependency (no fallback)
IDENTITY = "local.product_consumer_missing@v1"

DEPENDENCIES = {{
  {{
    product = "missing_tool",
  }},
}}

INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
end

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}",
}}
"""
        self._write_spec("product_consumer_missing.lua", spec_consumer_missing)

        manifest = self.manifest(
            f"""
PACKAGES = {{
  {{
    spec = "local.product_consumer_missing@v1",
    source = "{self.lua_path("product_consumer_missing.lua")}",
  }},
}}
"""
        )

        result = self.run_envy(["install", "--manifest", str(manifest)])
        self.assertNotEqual(
            result.returncode, 0, "expected failure for missing product"
        )
        self.assertIn("missing", result.stderr.lower())

    def test_product_collision_errors(self):
        # Second provider for collision testing
        spec_provider_b = """-- Second provider for collision testing
IDENTITY = "local.product_provider_b@v1"
PRODUCTS = {{ tool = "bin/other" }}

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}",
}}

INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
end
"""
        self._write_spec("product_provider_b.lua", spec_provider_b)

        manifest = self.manifest(
            f"""
PACKAGES = {{
  {{
    spec = "local.product_provider@v1",
    source = "{self.lua_path("product_provider.lua")}",
  }},
  {{
    spec = "local.product_provider_b@v1",
    source = "{self.lua_path("product_provider_b.lua")}",
  }},
}}
"""
        )

        result = self.run_envy(["product", "tool", "--manifest", str(manifest)])
        self.assertNotEqual(result.returncode, 0, "collision should fail")
        self.assertIn("Product 'tool'", result.stderr)

    def test_product_command_cached_provider(self):
        manifest = self.manifest(
            f"""
PACKAGES = {{
  {{
    spec = "local.product_provider@v1",
    source = "{self.lua_path("product_provider.lua")}",
  }},
}}
"""
        )

        result = self.run_envy(["product", "tool", "--manifest", str(manifest)])
        self.assertEqual(result.returncode, 0, result.stderr)
        output = result.stdout.strip()
        self.assertPathEndsWith(output, "bin/tool")
        self.assertIn("local.product_provider@v1", output)

    def test_product_command_programmatic_provider_returns_raw_value(self):
        manifest = self.manifest(
            f"""
PACKAGES = {{
  {{
    spec = "local.product_programmatic@v1",
    source = "{self.lua_path("product_provider_programmatic.lua")}",
  }},
}}
"""
        )

        result = self.run_envy(["product", "tool", "--manifest", str(manifest)])
        self.assertEqual(result.returncode, 0, result.stderr)
        output = result.stdout.strip()
        self.assertEqual(output, "programmatic-tool")

    def test_product_function_with_options(self):
        """Products can be a function taking options and returning a table."""
        # Provider with programmatic products function (takes options, returns table)
        spec_provider_function = """-- Provider with programmatic products function (takes options, returns table)
IDENTITY = "local.product_function@v1"

PRODUCTS = function(options)
  return {{
    ["python" .. options.version] = "bin/python",
    ["pip" .. options.version] = "bin/pip",
  }}
end

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}",
}}

INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
end
"""
        self._write_spec("product_provider_function.lua", spec_provider_function)

        manifest = self.manifest(
            f"""
PACKAGES = {{
  {{
    spec = "local.product_function@v1",
    source = "{self.lua_path("product_provider_function.lua")}",
    options = {{ version = "3.14" }},
  }},
}}
"""
        )

        # Query the dynamically-generated product name
        result = self.run_envy(["product", "python3.14", "--manifest", str(manifest)])
        self.assertEqual(result.returncode, 0, result.stderr)
        output = result.stdout.strip()
        self.assertPathEndsWith(output, "bin/python")

        # Verify second product also works
        result = self.run_envy(["product", "pip3.14", "--manifest", str(manifest)])
        self.assertEqual(result.returncode, 0, result.stderr)
        output = result.stdout.strip()
        self.assertPathEndsWith(output, "bin/pip")

    def test_absolute_path_in_product_value_rejected(self):
        """Product values with absolute paths should be rejected during parsing."""
        # Bad provider with absolute path in product value
        lua_content = f"""
IDENTITY = "local.bad_provider@v1"
PRODUCTS = {{ tool = "/etc/passwd" }}
FETCH = {{
  source = "{self.archive_path.as_posix()}",
  sha256 = "{self.archive_hash}",
}}
INSTALL = function(ctx)
end
"""
        provider_path = self.specs_dir / "bad_provider.lua"
        provider_path.write_text(lua_content, encoding="utf-8")

        manifest = self.manifest(
            f"""
PACKAGES = {{
  {{
    spec = "local.bad_provider@v1",
    source = "{provider_path.as_posix()}"
  }}
}}
"""
        )

        result = self.run_envy(["install", "--manifest", str(manifest)])
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("absolute", result.stderr.lower())

    def test_path_traversal_in_product_value_rejected(self):
        """Product values with path traversal should be rejected during parsing."""
        # Bad provider with path traversal in product value
        lua_content = f"""
IDENTITY = "local.bad_provider@v1"
PRODUCTS = {{ tool = "../../etc/passwd" }}
FETCH = {{
  source = "{self.archive_path.as_posix()}",
  sha256 = "{self.archive_hash}",
}}
INSTALL = function(ctx)
end
"""
        provider_path = self.specs_dir / "bad_provider.lua"
        provider_path.write_text(lua_content, encoding="utf-8")

        manifest = self.manifest(
            f"""
PACKAGES = {{
  {{
    spec = "local.bad_provider@v1",
    source = "{provider_path.as_posix()}"
  }}
}}
"""
        )

        result = self.run_envy(["install", "--manifest", str(manifest)])
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("traversal", result.stderr.lower())

    def test_strong_product_dep_not_resolved_as_weak(self):
        """Strong product dependencies should wire directly, not via weak resolution."""
        # Provider A for same product
        lua_content_a = f"""
IDENTITY = "local.provider_a@v1"
PRODUCTS = {{ tool = "bin/tool_a" }}
FETCH = {{
  source = "{self.archive_path.as_posix()}",
  sha256 = "{self.archive_hash}",
}}
INSTALL = function(ctx)
end
"""
        provider_a_path = self.specs_dir / "provider_a.lua"
        provider_a_path.write_text(lua_content_a, encoding="utf-8")

        # Provider B with different product
        lua_content_b = f"""
IDENTITY = "local.provider_b@v1"
PRODUCTS = {{ other_tool = "bin/other" }}
FETCH = {{
  source = "{self.archive_path.as_posix()}",
  sha256 = "{self.archive_hash}",
}}
INSTALL = function(ctx)
end
"""
        provider_b_path = self.specs_dir / "provider_b.lua"
        provider_b_path.write_text(lua_content_b, encoding="utf-8")

        # Consumer with STRONG dep on provider_a (has source)
        # If this goes through weak resolution, it might pick up provider_b instead
        lua_content_consumer = f"""
IDENTITY = "local.consumer_strong_only@v1"
DEPENDENCIES = {{
  {{
    product = "tool",
    spec = "local.provider_a@v1",
    source = "{provider_a_path.as_posix()}",
  }},
}}
FETCH = {{
  source = "{self.archive_path.as_posix()}",
  sha256 = "{self.archive_hash}",
}}
INSTALL = function(ctx)
  local tool_path = envy.package("local.provider_a@v1")
  if not tool_path:match("provider_a") then
    error("Expected provider_a but got: " .. tool_path)
  end
end
"""
        consumer_path = self.specs_dir / "consumer_strong.lua"
        consumer_path.write_text(lua_content_consumer, encoding="utf-8")

        # Include both providers in manifest - provider_b appears first (registry order)
        manifest = self.manifest(
            f"""
PACKAGES = {{
  {{
    spec = "local.provider_b@v1",
    source = "{provider_b_path.as_posix()}"
  }},
  {{
    spec = "local.consumer_strong_only@v1",
    source = "{consumer_path.as_posix()}"
  }},
}}
"""
        )

        result = self.run_envy(["install", "--manifest", str(manifest)])
        self.assertEqual(result.returncode, 0, result.stderr)

        # Verify consumer used provider_a (from strong dep), not provider_b (from registry)
        provider_a_dir = self.cache_root / "packages" / "local.provider_a@v1"
        self.assertTrue(provider_a_dir.exists(), "provider_a should be fetched")

    def test_product_semantic_cycle_detected(self):
        """Product dependencies forming a semantic cycle should be detected and rejected."""
        # Cycle provider A - depends on tool_b from cycle_b
        spec_cycle_a = """IDENTITY = "local.cycle_a@v1"
PRODUCTS = {{ tool_a = "bin/a" }}

DEPENDENCIES = {{
  {{
    product = "tool_b",
    spec = "local.cycle_b@v1",
    weak = {{
      spec = "local.cycle_b@v1",
      source = "product_cycle_b.lua",
    }}
  }},
}}

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}",
}}

INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
end
"""
        self._write_spec("product_cycle_a.lua", spec_cycle_a)

        # Cycle provider B - depends on tool_a from cycle_a
        spec_cycle_b = """IDENTITY = "local.cycle_b@v1"
PRODUCTS = {{ tool_b = "bin/b" }}

DEPENDENCIES = {{
  {{
    product = "tool_a",
    spec = "local.cycle_a@v1",
    weak = {{
      spec = "local.cycle_a@v1",
      source = "product_cycle_a.lua",
    }}
  }},
}}

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}",
}}

INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
end
"""
        self._write_spec("product_cycle_b.lua", spec_cycle_b)

        manifest = self.manifest(
            f"""
PACKAGES = {{
  {{
    spec = "local.cycle_a@v1",
    source = "{self.lua_path("product_cycle_a.lua")}",
  }},
}}
"""
        )

        result = self.run_envy(["install", "--manifest", str(manifest)])
        self.assertNotEqual(result.returncode, 0)
        # Should detect cycle through products
        self.assertTrue(
            "cycle" in result.stderr.lower() or "circular" in result.stderr.lower(),
            f"Expected cycle error but got: {result.stderr}",
        )

    def test_ref_only_product_dependency_unconstrained(self):
        """Ref-only product dependencies (no spec/source) should resolve to any provider."""
        manifest = self.manifest(
            f"""
PACKAGES = {{
  {{
    spec = "local.product_provider@v1",
    source = "{self.lua_path("product_provider.lua")}",
  }},
  {{
    spec = "local.ref_only_consumer@v1",
    source = "{self.lua_path("product_ref_only_consumer.lua")}",
  }},
}}
"""
        )

        result = self.run_envy(["install", "--manifest", str(manifest)])
        self.assertEqual(result.returncode, 0, result.stderr)

        # Both provider and consumer should exist
        provider_dir = self.cache_root / "packages" / "local.product_provider@v1"
        consumer_dir = self.cache_root / "packages" / "local.ref_only_consumer@v1"
        self.assertTrue(provider_dir.exists(), f"missing {provider_dir}")
        self.assertTrue(consumer_dir.exists(), f"missing {consumer_dir}")

    def test_ref_only_product_dependency_missing_errors(self):
        """Ref-only product dependency with no provider should error."""
        manifest = self.manifest(
            f"""
PACKAGES = {{
  {{
    spec = "local.ref_only_consumer@v1",
    source = "{self.lua_path("product_ref_only_consumer.lua")}",
  }},
}}
"""
        )

        result = self.run_envy(["install", "--manifest", str(manifest)])
        self.assertNotEqual(result.returncode, 0)
        # Should report missing product
        self.assertTrue(
            "tool" in result.stderr.lower() and "not found" in result.stderr.lower(),
            f"Expected missing product error but got: {result.stderr}",
        )

    def test_product_listing_shows_all_products(self):
        """Product command with no args should list all products from all providers."""
        # Second provider with non-colliding product for listing test
        lua_content = f"""
IDENTITY = "local.list_provider@v1"
PRODUCTS = {{ compiler = "bin/gcc" }}
FETCH = {{
  source = "{self.archive_path.as_posix()}",
  sha256 = "{self.archive_hash}",
}}
INSTALL = function(ctx)
end
"""
        list_provider_path = self.specs_dir / "list_provider.lua"
        list_provider_path.write_text(lua_content, encoding="utf-8")

        manifest = self.manifest(
            f"""
PACKAGES = {{
  {{
    spec = "local.product_provider@v1",
    source = "{self.lua_path("product_provider.lua")}",
  }},
  {{
    spec = "local.list_provider@v1",
    source = "{list_provider_path.as_posix()}",
  }},
}}
"""
        )

        result = self.run_envy(["product", "--manifest", str(manifest)])
        self.assertEqual(result.returncode, 0, result.stderr)
        # Should see products from both providers in stderr (human-readable)
        # product_provider has "tool", list_provider has "compiler"
        self.assertIn("tool", result.stderr.lower())
        self.assertIn("compiler", result.stderr.lower())

    def test_product_listing_json_output(self):
        """Product command with --json should output dict of resolved values to stdout."""
        import json

        manifest = self.manifest(
            f"""
PACKAGES = {{
  {{
    spec = "local.product_provider@v1",
    source = "{self.lua_path("product_provider.lua")}",
  }},
}}
"""
        )

        result = self.run_envy(["product", "--json", "--manifest", str(manifest)])
        self.assertEqual(result.returncode, 0, result.stderr)

        # Parse JSON dict from stdout
        products = json.loads(result.stdout)
        self.assertIsInstance(products, dict)
        self.assertIn("tool", products)

        # Cache-managed: resolved value should be an absolute path ending with bin/tool
        self.assertPathEndsWith(products["tool"], "bin/tool")
        self.assertIn("packages", products["tool"])

    def test_product_listing_programmatic_marked(self):
        """Product listing should emit verbatim value for user-managed products."""
        import json

        manifest = self.manifest(
            f"""
PACKAGES = {{
  {{
    spec = "local.product_programmatic@v1",
    source = "{self.lua_path("product_provider_programmatic.lua")}",
  }},
}}
"""
        )

        result = self.run_envy(["product", "--json", "--manifest", str(manifest)])
        self.assertEqual(result.returncode, 0, result.stderr)

        products = json.loads(result.stdout)
        self.assertIsInstance(products, dict)
        self.assertIn("tool", products)
        self.assertEqual(products["tool"], "programmatic-tool")

    def test_product_listing_empty(self):
        """Product listing with no products should indicate empty result."""
        # Spec with no products for empty listing test
        lua_content = f"""
IDENTITY = "local.no_products@v1"
FETCH = {{
  source = "{self.archive_path.as_posix()}",
  sha256 = "{self.archive_hash}",
}}
INSTALL = function(ctx)
end
"""
        no_products_path = self.specs_dir / "no_products.lua"
        no_products_path.write_text(lua_content, encoding="utf-8")

        manifest = self.manifest(
            f"""
PACKAGES = {{
  {{
    spec = "local.no_products@v1",
    source = "{no_products_path.as_posix()}"
  }},
}}
"""
        )

        result = self.run_envy(["product", "--manifest", str(manifest)])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("no products", result.stderr.lower())

    def test_product_command_resolves_providers_clean_cache(self):
        """Product command must resolve graph to find providers on clean cache.

        Regression test: product command should call resolve_graph() with all
        manifest packages to find product providers, not just specs directly
        requested. This ensures products can be queried even when the provider
        isn't explicitly the target.
        """
        # Product provider spec for clean cache resolution test
        provider_spec = f"""IDENTITY = "local.test_product_query_provider@v1"

FETCH = {{ source = "{self.archive_path.as_posix()}",
          sha256 = "{self.archive_hash}" }}

INSTALL = function(ctx)
end

PRODUCTS = {{ test_query_tool = "bin/query_tool" }}
"""
        provider_path = self.specs_dir / "test_product_query_provider.lua"
        provider_path.write_text(provider_spec, encoding="utf-8")

        # Manifest with the provider
        manifest = self.manifest(
            f"""
PACKAGES = {{
    {{ spec = "local.test_product_query_provider@v1", source = "{provider_path.as_posix()}" }}
}}
"""
        )

        # Query the product with clean cache - should resolve graph and find provider
        result = self.run_envy(
            ["product", "test_query_tool", "--manifest", str(manifest)]
        )

        # Should succeed - product command must resolve graph to find provider
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertTrue(result.stdout.strip(), "Expected product value in stdout")

        # Verify the output contains the expected path
        output = result.stdout.strip()
        self.assertPathContains(
            output, "bin/query_tool", f"Expected product path in output: {output}"
        )
        self.assertIn(
            "local.test_product_query_provider@v1",
            output,
            f"Expected provider identity in output: {output}",
        )


if __name__ == "__main__":
    unittest.main()
