"""Functional tests for envy.package() dependency validation.

Tests that specs must explicitly declare dependencies (direct or transitive)
before calling envy.package() to access them. This ensures build graph integrity
and enables better dependency analysis.
"""

import hashlib
import io
import os
import subprocess
import tarfile
from pathlib import Path
import unittest

from . import test_config
from .env import EnvyTestCase
from .trace_parser import TraceParser

# Test archive contents
TEST_ARCHIVE_FILES = {
    "root/file1.txt": "Root file content\n",
    "root/file2.txt": "Another root file\n",
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


class TestDependencyValidation(EnvyTestCase):
    """Tests for envy.package() dependency validation."""

    def setUp(self):
        super().setUp()
        self.specs_dir = self.make_temp_dir("specs_dir")

        # Create test archive and get its hash
        self.archive_path = self.specs_dir / "test.tar.gz"
        self.archive_hash = create_test_archive(self.archive_path)

    def write_spec(self, name: str, content: str) -> Path:
        """Write spec file with placeholder substitution."""
        spec_content = content.format(
            ARCHIVE_PATH=self.archive_path.as_posix(),
            ARCHIVE_HASH=self.archive_hash,
            SPECS_DIR=self.specs_dir.as_posix(),
        )
        path = self.specs_dir / name
        path.write_text(spec_content, encoding="utf-8")
        return path

    def install_spec(self, identity: str, spec_name: str, **kwargs):
        """Install one spec through a generated manifest.

        Returns (result, registered) where registered is the set of package keys
        the engine resolved.
        """
        trace_file = self.cache_root / "trace.jsonl"
        manifest = test_config.write_spec_manifest(
            self.specs_dir, [(identity, self.specs_dir / spec_name)]
        )
        result = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                f"--trace=file:{trace_file}",
                "install",
                "--manifest",
                str(manifest),
            ],
            capture_output=True,
            text=True,
            **kwargs,
        )
        return result, TraceParser(trace_file).registered_specs()

    def test_direct_dependency_declared(self):
        """Spec calls envy.package() on declared direct dependency - should succeed."""
        # Base library with no dependencies
        self.write_spec(
            "dep_val_lib.lua",
            """-- Dependency validation test: base library (no dependencies)
IDENTITY = "local.dep_val_lib@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  envy.run([[echo "lib built" > lib.txt]])
end
""",
        )

        # Spec that declares and accesses direct dependency
        self.write_spec(
            "dep_val_direct.lua",
            """-- Dependency validation test: POSITIVE - direct dependency access
IDENTITY = "local.dep_val_direct@v1"

DEPENDENCIES = {{
  {{ spec = "local.dep_val_lib@v1", source = "{SPECS_DIR}/dep_val_lib.lua" }}
}}

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  -- Access direct dependency - SHOULD WORK
  local lib_path = envy.package("local.dep_val_lib@v1")
  envy.run([[echo "direct access worked" > direct.txt]])
end
""",
        )

        result, output = self.install_spec(
            "local.dep_val_direct@v1", "dep_val_direct.lua"
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        # Success means validation passed - the build completed without error
        self.assertIn("local.dep_val_direct@v1", output)

    def test_missing_dependency_declaration(self):
        """Spec calls envy.package() without declaring dependency - should fail."""
        # Base library that missing spec tries to access
        self.write_spec(
            "dep_val_lib.lua",
            """-- Dependency validation test: base library (no dependencies)
IDENTITY = "local.dep_val_lib@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  envy.run([[echo "lib built" > lib.txt]])
end
""",
        )

        # Spec that tries to access lib without declaring dependency
        self.write_spec(
            "dep_val_missing.lua",
            """-- Dependency validation test: NEGATIVE - calls envy.package without declaring dependency
IDENTITY = "local.dep_val_missing@v1"

-- Note: NO dependencies declared, but we try to access dep_val_lib below

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  -- Try to access lib without declaring it - SHOULD FAIL
  local lib_path = envy.package("local.dep_val_lib@v1")
  envy.run([[echo "should not get here" > bad.txt]])
end
""",
        )

        result, _ = self.install_spec("local.dep_val_missing@v1", "dep_val_missing.lua")

        self.assertNotEqual(
            result.returncode, 0, "Expected failure for missing dependency"
        )
        self.assertIn(
            "has no strong dependency on 'local.dep_val_lib@v1'", result.stderr
        )
        self.assertIn("local.dep_val_missing@v1", result.stderr)

    def test_transitive_dependency(self):
        """Spec calls envy.package() on transitive dependency - should succeed."""
        # Base library
        self.write_spec(
            "dep_val_lib.lua",
            """-- Dependency validation test: base library (no dependencies)
IDENTITY = "local.dep_val_lib@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  envy.run([[echo "lib built" > lib.txt]])
end
""",
        )

        # Tool that depends on lib
        self.write_spec(
            "dep_val_tool.lua",
            """-- Dependency validation test: tool that depends on lib
IDENTITY = "local.dep_val_tool@v1"

DEPENDENCIES = {{
  {{ spec = "local.dep_val_lib@v1", source = "{SPECS_DIR}/dep_val_lib.lua" }}
}}

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  -- Access our direct dependency - should work
  local lib_path = envy.package("local.dep_val_lib@v1")
  envy.run([[echo "tool built with lib" > tool.txt]])
end
""",
        )

        # Spec that accesses lib transitively through tool
        self.write_spec(
            "dep_val_transitive.lua",
            """-- Dependency validation test: POSITIVE - transitive dependency access
IDENTITY = "local.dep_val_transitive@v1"

DEPENDENCIES = {{
  -- We depend on tool, which depends on lib
  {{ spec = "local.dep_val_tool@v1", source = "{SPECS_DIR}/dep_val_tool.lua" }},
  -- In the new design, we must explicitly declare all dependencies we use
  {{ spec = "local.dep_val_lib@v1", source = "{SPECS_DIR}/dep_val_lib.lua" }}
}}

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  -- Access tool (direct dependency) - should work
  local tool_path = envy.package("local.dep_val_tool@v1")

  -- Access lib (transitive dependency: us -> tool -> lib) - SHOULD WORK
  local lib_path = envy.package("local.dep_val_lib@v1")

  envy.run([[echo "transitive access worked" > transitive.txt]])
end
""",
        )

        result, output = self.install_spec(
            "local.dep_val_transitive@v1", "dep_val_transitive.lua"
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        # Success means validation passed - the build completed without error
        self.assertIn("local.dep_val_transitive@v1", output)

    def test_transitive_3_levels(self):
        """Spec calls envy.package() on transitive dependency 3 levels deep - should succeed."""
        # Base library for 3-level test
        self.write_spec(
            "dep_val_level3_base.lua",
            """-- Base library for 3-level transitive dependency test
IDENTITY = "local.dep_val_level3_base@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end
""",
        )

        # Middle layer for 3-level test
        self.write_spec(
            "dep_val_level3_mid.lua",
            """-- Middle layer for 3-level transitive dependency test
IDENTITY = "local.dep_val_level3_mid@v1"

DEPENDENCIES = {{
  {{ spec = "local.dep_val_level3_base@v1", source = "{SPECS_DIR}/dep_val_level3_base.lua", needed_by = "stage" }}
}}

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
  -- Access declared dependency
  envy.package("local.dep_val_level3_base@v1")
end
""",
        )

        # Top layer that accesses base 3 levels deep
        self.write_spec(
            "dep_val_level3_top.lua",
            """-- Top layer for 3-level transitive dependency test
-- Tests: A->B->C, A accesses C (3 levels)
IDENTITY = "local.dep_val_level3_top@v1"

DEPENDENCIES = {{
  {{ spec = "local.dep_val_level3_mid@v1", source = "{SPECS_DIR}/dep_val_level3_mid.lua", needed_by = "stage" }},
  -- Must explicitly declare all dependencies we access
  {{ spec = "local.dep_val_level3_base@v1", source = "{SPECS_DIR}/dep_val_level3_base.lua", needed_by = "stage" }}
}}

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
  -- Access transitive dependency 2 levels deep
  envy.package("local.dep_val_level3_base@v1")
end
""",
        )

        result, output = self.install_spec(
            "local.dep_val_level3_top@v1", "dep_val_level3_top.lua"
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertIn("local.dep_val_level3_top@v1", output)

    def test_diamond_dependency(self):
        """Spec accesses dependency via two different paths (diamond) - should succeed."""
        # Base for diamond
        self.write_spec(
            "dep_val_diamond_base.lua",
            """-- Base for diamond dependency test
IDENTITY = "local.dep_val_diamond_base@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end
""",
        )

        # Left path of diamond
        self.write_spec(
            "dep_val_diamond_left.lua",
            """-- Left path for diamond dependency test
IDENTITY = "local.dep_val_diamond_left@v1"

DEPENDENCIES = {{
  {{ spec = "local.dep_val_diamond_base@v1", source = "{SPECS_DIR}/dep_val_diamond_base.lua", needed_by = "stage" }}
}}

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
  envy.package("local.dep_val_diamond_base@v1")
end
""",
        )

        # Right path of diamond
        self.write_spec(
            "dep_val_diamond_right.lua",
            """-- Right path for diamond dependency test
IDENTITY = "local.dep_val_diamond_right@v1"

DEPENDENCIES = {{
  {{ spec = "local.dep_val_diamond_base@v1", source = "{SPECS_DIR}/dep_val_diamond_base.lua", needed_by = "stage" }}
}}

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
  envy.package("local.dep_val_diamond_base@v1")
end
""",
        )

        # Top of diamond - accesses base through both left and right
        self.write_spec(
            "dep_val_diamond_top.lua",
            """-- Top of diamond dependency test
-- Tests: A->B->D, A->C->D, A accesses D via both paths
IDENTITY = "local.dep_val_diamond_top@v1"

DEPENDENCIES = {{
  {{ spec = "local.dep_val_diamond_left@v1", source = "{SPECS_DIR}/dep_val_diamond_left.lua", needed_by = "stage" }},
  {{ spec = "local.dep_val_diamond_right@v1", source = "{SPECS_DIR}/dep_val_diamond_right.lua", needed_by = "stage" }},
  -- Must explicitly declare all dependencies we access
  {{ spec = "local.dep_val_diamond_base@v1", source = "{SPECS_DIR}/dep_val_diamond_base.lua", needed_by = "stage" }}
}}

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
  -- Access diamond base through both left and right paths
  envy.package("local.dep_val_diamond_base@v1")
end
""",
        )

        result, output = self.install_spec(
            "local.dep_val_diamond_top@v1", "dep_val_diamond_top.lua"
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertIn("local.dep_val_diamond_top@v1", output)

    def test_deep_chain_5_levels(self):
        """Spec calls envy.package() on dependency 5 levels deep - should succeed."""
        # Level A (bottom) for 5-level chain
        self.write_spec(
            "dep_val_chain5_a.lua",
            """-- Level A (bottom) for 5-level chain test
IDENTITY = "local.dep_val_chain5_a@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end
""",
        )

        # Level B
        self.write_spec(
            "dep_val_chain5_b.lua",
            """-- Level B for 5-level chain test
IDENTITY = "local.dep_val_chain5_b@v1"

DEPENDENCIES = {{
  {{ spec = "local.dep_val_chain5_a@v1", source = "{SPECS_DIR}/dep_val_chain5_a.lua", needed_by = "stage" }}
}}

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
  envy.package("local.dep_val_chain5_a@v1")
end
""",
        )

        # Level C
        self.write_spec(
            "dep_val_chain5_c.lua",
            """-- Level C for 5-level chain test
IDENTITY = "local.dep_val_chain5_c@v1"

DEPENDENCIES = {{
  {{ spec = "local.dep_val_chain5_b@v1", source = "{SPECS_DIR}/dep_val_chain5_b.lua", needed_by = "stage" }}
}}

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
  envy.package("local.dep_val_chain5_b@v1")
end
""",
        )

        # Level D
        self.write_spec(
            "dep_val_chain5_d.lua",
            """-- Level D for 5-level chain test
IDENTITY = "local.dep_val_chain5_d@v1"

DEPENDENCIES = {{
  {{ spec = "local.dep_val_chain5_c@v1", source = "{SPECS_DIR}/dep_val_chain5_c.lua", needed_by = "stage" }}
}}

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
  envy.package("local.dep_val_chain5_c@v1")
end
""",
        )

        # Level E (top) - accesses A which is 5 levels deep
        self.write_spec(
            "dep_val_chain5_e.lua",
            """-- Level E (top) for 5-level chain test
-- Tests: E->D->C->B->A, E accesses A (5 levels deep)
IDENTITY = "local.dep_val_chain5_e@v1"

DEPENDENCIES = {{
  {{ spec = "local.dep_val_chain5_d@v1", source = "{SPECS_DIR}/dep_val_chain5_d.lua", needed_by = "stage" }},
  -- Must explicitly declare all dependencies we access
  {{ spec = "local.dep_val_chain5_a@v1", source = "{SPECS_DIR}/dep_val_chain5_a.lua", needed_by = "stage" }}
}}

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
  -- Access dependency 4 levels deep
  envy.package("local.dep_val_chain5_a@v1")
end
""",
        )

        result, output = self.install_spec(
            "local.dep_val_chain5_e@v1", "dep_val_chain5_e.lua"
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertIn("local.dep_val_chain5_e@v1", output)

    def test_unrelated_recipe_error(self):
        """Spec calls envy.package() on unrelated spec - should fail."""
        # Base library that unrelated spec tries to access
        self.write_spec(
            "dep_val_lib.lua",
            """-- Dependency validation test: base library (no dependencies)
IDENTITY = "local.dep_val_lib@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  envy.run([[echo "lib built" > lib.txt]])
end
""",
        )

        # Spec that tries to access lib without declaring it
        self.write_spec(
            "dep_val_unrelated.lua",
            """-- Test for unrelated spec error
-- This spec tries to access lib without declaring it
IDENTITY = "local.dep_val_unrelated@v1"

-- Intentionally NOT declaring any dependencies

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
  -- Try to access lib without declaring it - should fail
  envy.package("local.dep_val_lib@v1")
end
""",
        )

        result, _ = self.install_spec(
            "local.dep_val_unrelated@v1", "dep_val_unrelated.lua"
        )

        self.assertNotEqual(result.returncode, 0, "Expected failure for unrelated spec")
        self.assertIn(
            "has no strong dependency on 'local.dep_val_lib@v1'", result.stderr
        )
        self.assertIn("local.dep_val_unrelated@v1", result.stderr)
        self.assertIn("local.dep_val_lib@v1", result.stderr)

    def test_needed_by_direct(self):
        """Spec with needed_by="recipe_fetch" calls envy.package() on direct dep in fetch phase - should succeed."""
        # Base spec for needed_by testing
        self.write_spec(
            "dep_val_needed_by_base.lua",
            """-- Base spec for needed_by testing
IDENTITY = "local.dep_val_needed_by_base@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end
""",
        )

        # Spec with needed_by="fetch" accessing direct dependency in fetch phase
        self.write_spec(
            "dep_val_needed_by_direct.lua",
            """-- Spec with needed_by="fetch" accessing direct dependency in fetch phase
IDENTITY = "local.dep_val_needed_by_direct@v1"

DEPENDENCIES = {{
  {{ spec = "local.dep_val_needed_by_base@v1", source = "{SPECS_DIR}/dep_val_needed_by_base.lua", needed_by = "fetch" }}
}}

FETCH = function(tmp_dir, options)
  -- Access direct dependency in fetch phase
  envy.package("local.dep_val_needed_by_base@v1")
  return "{ARCHIVE_PATH}", "{ARCHIVE_HASH}"
end

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end
""",
        )

        result, output = self.install_spec(
            "local.dep_val_needed_by_direct@v1", "dep_val_needed_by_direct.lua"
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertIn("local.dep_val_needed_by_direct@v1", output)

    def test_needed_by_transitive(self):
        """Spec with needed_by="recipe_fetch" calls envy.package() on transitive dep in fetch phase - should succeed."""
        # Base spec for needed_by testing
        self.write_spec(
            "dep_val_needed_by_base.lua",
            """-- Base spec for needed_by testing
IDENTITY = "local.dep_val_needed_by_base@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end
""",
        )

        # Middle spec for needed_by transitive testing
        self.write_spec(
            "dep_val_needed_by_mid.lua",
            """-- Middle spec for needed_by transitive testing
IDENTITY = "local.dep_val_needed_by_mid@v1"

DEPENDENCIES = {{
  {{ spec = "local.dep_val_needed_by_base@v1", source = "{SPECS_DIR}/dep_val_needed_by_base.lua" }}
}}

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end
""",
        )

        # Spec that uses needed_by="fetch" and accesses transitive dependency in fetch phase
        self.write_spec(
            "dep_val_needed_by_transitive.lua",
            """-- Spec that uses needed_by="fetch" and accesses transitive dependency in fetch phase
IDENTITY = "local.dep_val_needed_by_transitive@v1"

DEPENDENCIES = {{
  {{ spec = "local.dep_val_needed_by_mid@v1", source = "{SPECS_DIR}/dep_val_needed_by_mid.lua", needed_by = "fetch" }},
  -- Must explicitly declare all dependencies we access
  {{ spec = "local.dep_val_needed_by_base@v1", source = "{SPECS_DIR}/dep_val_needed_by_base.lua", needed_by = "fetch" }}
}}

FETCH = function(tmp_dir, options)
  -- Access transitive dependency (mid->base) in fetch phase
  envy.package("local.dep_val_needed_by_base@v1")
  return "{ARCHIVE_PATH}", "{ARCHIVE_HASH}"
end

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end
""",
        )

        result, output = self.install_spec(
            "local.dep_val_needed_by_transitive@v1", "dep_val_needed_by_transitive.lua"
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertIn("local.dep_val_needed_by_transitive@v1", output)

    def test_needed_by_undeclared(self):
        """Spec with needed_by="recipe_fetch" calls envy.package() on undeclared dep - should fail."""
        # Base library that undeclared spec tries to access
        self.write_spec(
            "dep_val_lib.lua",
            """-- Dependency validation test: base library (no dependencies)
IDENTITY = "local.dep_val_lib@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  envy.run([[echo "lib built" > lib.txt]])
end
""",
        )

        # Base spec for needed_by testing
        self.write_spec(
            "dep_val_needed_by_base.lua",
            """-- Base spec for needed_by testing
IDENTITY = "local.dep_val_needed_by_base@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end
""",
        )

        # Middle spec for needed_by transitive testing
        self.write_spec(
            "dep_val_needed_by_mid.lua",
            """-- Middle spec for needed_by transitive testing
IDENTITY = "local.dep_val_needed_by_mid@v1"

DEPENDENCIES = {{
  {{ spec = "local.dep_val_needed_by_base@v1", source = "{SPECS_DIR}/dep_val_needed_by_base.lua" }}
}}

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end
""",
        )

        # Spec that uses needed_by="fetch" but tries to access undeclared dependency
        self.write_spec(
            "dep_val_needed_by_undeclared.lua",
            """-- Spec that uses needed_by="fetch" but tries to access undeclared dependency
IDENTITY = "local.dep_val_needed_by_undeclared@v1"

DEPENDENCIES = {{
  {{ spec = "local.dep_val_needed_by_mid@v1", source = "{SPECS_DIR}/dep_val_needed_by_mid.lua", needed_by = "fetch" }}
}}

FETCH = function(tmp_dir, options)
  -- Try to access lib which is NOT declared as dependency - should fail
  envy.package("local.dep_val_lib@v1")
  return "{ARCHIVE_PATH}", "{ARCHIVE_HASH}"
end

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end
""",
        )

        result, _ = self.install_spec(
            "local.dep_val_needed_by_undeclared@v1", "dep_val_needed_by_undeclared.lua"
        )

        self.assertNotEqual(
            result.returncode, 0, "Expected failure for undeclared dependency"
        )
        self.assertIn(
            "has no strong dependency on 'local.dep_val_lib@v1'", result.stderr
        )
        self.assertIn("local.dep_val_needed_by_undeclared@v1", result.stderr)
        self.assertIn("local.dep_val_lib@v1", result.stderr)

    def test_parallel_validation(self):
        """Multiple specs sharing same base library, all validated in parallel - should all succeed."""
        # Base library for parallel validation testing
        self.write_spec(
            "dep_val_parallel_base.lua",
            """-- Base library for parallel validation testing
IDENTITY = "local.dep_val_parallel_base@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end
""",
        )

        # Generate 10 parallel user specs
        for i in range(1, 11):
            self.write_spec(
                f"dep_val_parallel_user{i}.lua",
                """-- User spec {i} for parallel validation testing
IDENTITY = "local.dep_val_parallel_user{i}@v1"

DEPENDENCIES = {{{{
  {{{{ spec = "local.dep_val_parallel_base@v1", source = "{{SPECS_DIR}}/dep_val_parallel_base.lua", needed_by = "stage" }}}}
}}}}

FETCH = {{{{
  source = "{{ARCHIVE_PATH}}",
  sha256 = "{{ARCHIVE_HASH}}"
}}}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{{{strip = 1}}}})
  -- Access shared base library
  envy.package("local.dep_val_parallel_base@v1")
end
""".format(i=i),
            )

        # Manifest spec that depends on all 10 parallel users
        self.write_spec(
            "dep_val_parallel_manifest.lua",
            """-- Manifest spec that depends on all 10 parallel users
IDENTITY = "local.dep_val_parallel_manifest@v1"

DEPENDENCIES = {{
  {{ spec = "local.dep_val_parallel_user1@v1", source = "{SPECS_DIR}/dep_val_parallel_user1.lua" }},
  {{ spec = "local.dep_val_parallel_user2@v1", source = "{SPECS_DIR}/dep_val_parallel_user2.lua" }},
  {{ spec = "local.dep_val_parallel_user3@v1", source = "{SPECS_DIR}/dep_val_parallel_user3.lua" }},
  {{ spec = "local.dep_val_parallel_user4@v1", source = "{SPECS_DIR}/dep_val_parallel_user4.lua" }},
  {{ spec = "local.dep_val_parallel_user5@v1", source = "{SPECS_DIR}/dep_val_parallel_user5.lua" }},
  {{ spec = "local.dep_val_parallel_user6@v1", source = "{SPECS_DIR}/dep_val_parallel_user6.lua" }},
  {{ spec = "local.dep_val_parallel_user7@v1", source = "{SPECS_DIR}/dep_val_parallel_user7.lua" }},
  {{ spec = "local.dep_val_parallel_user8@v1", source = "{SPECS_DIR}/dep_val_parallel_user8.lua" }},
  {{ spec = "local.dep_val_parallel_user9@v1", source = "{SPECS_DIR}/dep_val_parallel_user9.lua" }},
  {{ spec = "local.dep_val_parallel_user10@v1", source = "{SPECS_DIR}/dep_val_parallel_user10.lua" }}
}}

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end
""",
        )

        # Set ENVY_TEST_JOBS to enable parallel execution
        env = os.environ.copy()
        env["ENVY_TEST_JOBS"] = "8"

        result, output = self.install_spec(
            "local.dep_val_parallel_manifest@v1",
            "dep_val_parallel_manifest.lua",
            env=env,
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertIn("local.dep_val_parallel_manifest@v1", output)

    def test_default_shell_with_dependency(self):
        """default_shell function calls envy.package(), spec declares dependency - should succeed."""
        # Tool spec that provides shell configuration
        self.write_spec(
            "dep_val_shell_tool.lua",
            """-- Tool spec that provides shell configuration
IDENTITY = "local.dep_val_shell_tool@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end
""",
        )

        # Spec with default_shell function that calls envy.package() on declared dependency
        self.write_spec(
            "dep_val_shell_with_dep.lua",
            """-- Spec with default_shell function that calls envy.package() on declared dependency
IDENTITY = "local.dep_val_shell_with_dep@v1"

DEPENDENCIES = {{
  {{ spec = "local.dep_val_shell_tool@v1", source = "{SPECS_DIR}/dep_val_shell_tool.lua" }}
}}

DEFAULT_SHELL = function()
  -- Access declared dependency in default_shell
  envy.package("local.dep_val_shell_tool@v1")
  return ENVY_SHELL.BASH
end

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
  -- Run a command to trigger default_shell evaluation
  envy.run("echo 'test'")
end
""",
        )

        result, output = self.install_spec(
            "local.dep_val_shell_with_dep@v1", "dep_val_shell_with_dep.lua"
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertIn("local.dep_val_shell_with_dep@v1", output)

    def test_deep_chain_parallel(self):
        """Deep transitive chain under parallel execution - validation doesn't race."""
        # Level A (bottom) for 5-level chain
        self.write_spec(
            "dep_val_chain5_a.lua",
            """-- Level A (bottom) for 5-level chain test
IDENTITY = "local.dep_val_chain5_a@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end
""",
        )

        # Level B
        self.write_spec(
            "dep_val_chain5_b.lua",
            """-- Level B for 5-level chain test
IDENTITY = "local.dep_val_chain5_b@v1"

DEPENDENCIES = {{
  {{ spec = "local.dep_val_chain5_a@v1", source = "{SPECS_DIR}/dep_val_chain5_a.lua", needed_by = "stage" }}
}}

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
  envy.package("local.dep_val_chain5_a@v1")
end
""",
        )

        # Level C
        self.write_spec(
            "dep_val_chain5_c.lua",
            """-- Level C for 5-level chain test
IDENTITY = "local.dep_val_chain5_c@v1"

DEPENDENCIES = {{
  {{ spec = "local.dep_val_chain5_b@v1", source = "{SPECS_DIR}/dep_val_chain5_b.lua", needed_by = "stage" }}
}}

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
  envy.package("local.dep_val_chain5_b@v1")
end
""",
        )

        # Level D
        self.write_spec(
            "dep_val_chain5_d.lua",
            """-- Level D for 5-level chain test
IDENTITY = "local.dep_val_chain5_d@v1"

DEPENDENCIES = {{
  {{ spec = "local.dep_val_chain5_c@v1", source = "{SPECS_DIR}/dep_val_chain5_c.lua", needed_by = "stage" }}
}}

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
  envy.package("local.dep_val_chain5_c@v1")
end
""",
        )

        # Level E (top) - accesses A which is 5 levels deep
        self.write_spec(
            "dep_val_chain5_e.lua",
            """-- Level E (top) for 5-level chain test
-- Tests: E->D->C->B->A, E accesses A (5 levels deep)
IDENTITY = "local.dep_val_chain5_e@v1"

DEPENDENCIES = {{
  {{ spec = "local.dep_val_chain5_d@v1", source = "{SPECS_DIR}/dep_val_chain5_d.lua", needed_by = "stage" }},
  -- Must explicitly declare all dependencies we access
  {{ spec = "local.dep_val_chain5_a@v1", source = "{SPECS_DIR}/dep_val_chain5_a.lua", needed_by = "stage" }}
}}

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
  -- Access dependency 4 levels deep
  envy.package("local.dep_val_chain5_a@v1")
end
""",
        )

        # Run the 5-level chain test with parallel jobs
        env = os.environ.copy()
        env["ENVY_TEST_JOBS"] = "8"

        result, output = self.install_spec(
            "local.dep_val_chain5_e@v1", "dep_val_chain5_e.lua", env=env
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertIn("local.dep_val_chain5_e@v1", output)


if __name__ == "__main__":
    unittest.main()
