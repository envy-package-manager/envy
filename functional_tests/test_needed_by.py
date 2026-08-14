"""Functional tests for needed_by phase dependencies.

Tests that needed_by annotation enables fine-grained parallelism by allowing
specs to specify which phase they actually need a dependency for. For example,
A depends on B with needed_by="build" means A's fetch/stage can run in parallel
with B's pipeline, blocking only when A needs to build.
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
from .trace_parser import PkgPhase, TraceParser

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


# Shared dependency spec - simple library for dependency validation tests
DEP_VAL_LIB_SPEC = """-- Simple library for dependency validation tests
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
"""

# Minimal spec for invalid phase test
SIMPLE_SPEC = """-- Minimal test spec
IDENTITY = "local.simple@v1"

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


class TestNeededBy(EnvyTestCase):
    """Tests for needed_by phase coupling."""

    def setUp(self):
        super().setUp()
        self.specs_dir = self.make_temp_dir("specs_dir")

        # Create test archive and get its hash
        self.archive_path = self.specs_dir / "test.tar.gz"
        self.archive_hash = create_test_archive(self.archive_path)

        # Write shared specs
        self.write_spec("dep_val_lib.lua", DEP_VAL_LIB_SPEC)
        self.write_spec("simple.lua", SIMPLE_SPEC)

    def write_spec(self, name: str, content: str) -> None:
        """Write spec file with placeholder substitution."""
        spec_content = content.format(
            ARCHIVE_PATH=self.archive_path.as_posix(),
            ARCHIVE_HASH=self.archive_hash,
            SPECS_DIR=self.specs_dir.as_posix(),
        )
        (self.specs_dir / name).write_text(spec_content, encoding="utf-8")

    def spec_path(self, name: str) -> str:
        """Get path to spec file."""
        return str(self.specs_dir / name)

    def install_spec(
        self, identity: str, spec_name: str, trace: str = "trace.jsonl", **kwargs
    ):
        """Install one spec through a generated manifest; returns (result, parser)."""
        trace_file = self.cache_root / trace
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
        return result, TraceParser(trace_file)

    def test_needed_by_fetch_allows_parallelism(self):
        """Spec A depends on B with needed_by='fetch' - A's early phases run in parallel."""
        # Parent spec - tests needed_by="fetch" dependency
        self.write_spec(
            "needed_by_fetch_parent.lua",
            """-- Tests needed_by="fetch" - dependency completes before parent's fetch phase
IDENTITY = "local.needed_by_fetch_parent@v1"

DEPENDENCIES = {{
  {{ spec = "local.needed_by_fetch_dep@v1", source = "{SPECS_DIR}/needed_by_fetch_dep.lua", needed_by = "fetch" }}
}}

FETCH = function(tmp_dir, options)
  -- Can access dependency in fetch phase
  local dep_path = envy.package("local.needed_by_fetch_dep@v1")
  return "{ARCHIVE_PATH}", "{ARCHIVE_HASH}"
end

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end
""",
        )

        # Dependency spec for fetch test
        self.write_spec(
            "needed_by_fetch_dep.lua",
            """-- Dependency for needed_by="fetch" test
IDENTITY = "local.needed_by_fetch_dep@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end
""",
        )

        result, parser = self.install_spec(
            "local.needed_by_fetch_parent@v1", "needed_by_fetch_parent.lua"
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertIn("local.needed_by_fetch_parent@v1", parser.registered_specs())
        self.assertIn("local.needed_by_fetch_dep@v1", parser.registered_specs())

        # Verify needed_by phase is set correctly
        parser.assert_dependency_needed_by(
            "local.needed_by_fetch_parent@v1",
            "local.needed_by_fetch_dep@v1",
            PkgPhase.PKG_FETCH,
        )

    def test_needed_by_build(self):
        """Spec A depends on B with needed_by='build' - fetch/stage parallel, build waits."""
        # Parent spec - tests needed_by="build" dependency
        self.write_spec(
            "needed_by_build_parent.lua",
            """-- Tests needed_by="build" - dependency completes before parent's build phase
IDENTITY = "local.needed_by_build_parent@v1"

DEPENDENCIES = {{
  {{ spec = "local.needed_by_build_dep@v1", source = "{SPECS_DIR}/needed_by_build_dep.lua", needed_by = "build" }}
}}

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  -- Can access dependency in build phase
  envy.package("local.needed_by_build_dep@v1")
  envy.run("echo 'build complete' > build.txt")
end
""",
        )

        # Dependency spec for build test
        self.write_spec(
            "needed_by_build_dep.lua",
            """-- Dependency for needed_by="build" test
IDENTITY = "local.needed_by_build_dep@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end
""",
        )

        result, parser = self.install_spec(
            "local.needed_by_build_parent@v1", "needed_by_build_parent.lua"
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertIn("local.needed_by_build_parent@v1", parser.registered_specs())
        self.assertIn("local.needed_by_build_dep@v1", parser.registered_specs())

        parser.assert_dependency_needed_by(
            "local.needed_by_build_parent@v1",
            "local.needed_by_build_dep@v1",
            PkgPhase.PKG_BUILD,
        )

    def test_needed_by_stage(self):
        """Spec A depends on B with needed_by='stage' - fetch parallel, stage waits."""
        # Parent spec - tests needed_by="stage" dependency
        self.write_spec(
            "needed_by_stage_parent.lua",
            """-- Tests needed_by="stage" - dependency completes before parent's stage phase
IDENTITY = "local.needed_by_stage_parent@v1"

DEPENDENCIES = {{
  {{ spec = "local.needed_by_stage_dep@v1", source = "{SPECS_DIR}/needed_by_stage_dep.lua", needed_by = "stage" }}
}}

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
  -- Can access dependency in stage phase
  envy.package("local.needed_by_stage_dep@v1")
end
""",
        )

        # Dependency spec for stage test
        self.write_spec(
            "needed_by_stage_dep.lua",
            """-- Dependency for needed_by="stage" test
IDENTITY = "local.needed_by_stage_dep@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end
""",
        )

        result, parser = self.install_spec(
            "local.needed_by_stage_parent@v1", "needed_by_stage_parent.lua"
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertIn("local.needed_by_stage_parent@v1", parser.registered_specs())
        self.assertIn("local.needed_by_stage_dep@v1", parser.registered_specs())

        parser.assert_dependency_needed_by(
            "local.needed_by_stage_parent@v1",
            "local.needed_by_stage_dep@v1",
            PkgPhase.PKG_STAGE,
        )

    def test_needed_by_install(self):
        """Spec A depends on B with needed_by='install' - fetch/stage/build parallel, install waits."""
        # Parent spec - tests needed_by="install" dependency
        self.write_spec(
            "needed_by_install_parent.lua",
            """-- Tests needed_by="install" - dependency completes before parent's install phase
IDENTITY = "local.needed_by_install_parent@v1"

DEPENDENCIES = {{
  {{ spec = "local.needed_by_install_dep@v1", source = "{SPECS_DIR}/needed_by_install_dep.lua", needed_by = "install" }}
}}

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end

INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  -- Can access dependency in install phase
  envy.package("local.needed_by_install_dep@v1")
end
""",
        )

        # Dependency spec for install test
        self.write_spec(
            "needed_by_install_dep.lua",
            """-- Dependency for needed_by="install" test
IDENTITY = "local.needed_by_install_dep@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end
""",
        )

        result, parser = self.install_spec(
            "local.needed_by_install_parent@v1", "needed_by_install_parent.lua"
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertIn("local.needed_by_install_parent@v1", parser.registered_specs())
        self.assertIn("local.needed_by_install_dep@v1", parser.registered_specs())

        parser.assert_dependency_needed_by(
            "local.needed_by_install_parent@v1",
            "local.needed_by_install_dep@v1",
            PkgPhase.PKG_INSTALL,
        )

    def test_needed_by_check(self):
        """Spec A depends on B with needed_by='check' - check waits, rest runs parallel."""
        # Parent spec - tests needed_by="check" dependency
        self.write_spec(
            "needed_by_check_parent.lua",
            """-- Tests needed_by="check" - dependency completes before parent's check phase
IDENTITY = "local.needed_by_check_parent@v1"

DEPENDENCIES = {{
  {{ spec = "local.needed_by_check_dep@v1", source = "{SPECS_DIR}/needed_by_check_dep.lua", needed_by = "check" }}
}}

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
  -- Dependency is available by now since check already completed
  envy.package("local.needed_by_check_dep@v1")
end
""",
        )

        # Dependency spec for check test
        self.write_spec(
            "needed_by_check_dep.lua",
            """-- Dependency for needed_by="check" test
IDENTITY = "local.needed_by_check_dep@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end
""",
        )

        result, parser = self.install_spec(
            "local.needed_by_check_parent@v1", "needed_by_check_parent.lua"
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertIn("local.needed_by_check_parent@v1", parser.registered_specs())
        self.assertIn("local.needed_by_check_dep@v1", parser.registered_specs())

        parser.assert_dependency_needed_by(
            "local.needed_by_check_parent@v1",
            "local.needed_by_check_dep@v1",
            PkgPhase.PKG_CHECK,
        )

    def test_needed_by_default_to_build(self):
        """Spec A depends on B without needed_by - defaults to build phase."""
        # Parent spec - tests default needed_by behavior (should default to "build")
        self.write_spec(
            "needed_by_default_parent.lua",
            """-- Tests default needed_by behavior - should default to "build" phase
IDENTITY = "local.needed_by_default_parent@v1"

DEPENDENCIES = {{
  -- No needed_by specified - should default to build
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
  -- Dependency should be available by build phase
  envy.package("local.dep_val_lib@v1")
end
""",
        )

        result, parser = self.install_spec(
            "local.needed_by_default_parent@v1", "needed_by_default_parent.lua"
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertIn("local.needed_by_default_parent@v1", parser.registered_specs())
        self.assertIn("local.dep_val_lib@v1", parser.registered_specs())

        parser.assert_dependency_needed_by(
            "local.needed_by_default_parent@v1",
            "local.dep_val_lib@v1",
            PkgPhase.PKG_BUILD,
        )

    def test_needed_by_invalid_phase_name(self):
        """Spec with needed_by='nonexistent' fails during parsing."""
        # Spec with invalid needed_by phase name - should fail during parsing
        self.write_spec(
            "needed_by_invalid.lua",
            """-- Tests invalid needed_by phase name - should fail during parsing
IDENTITY = "local.needed_by_invalid@v1"

DEPENDENCIES = {{
  {{ spec = "local.simple@v1", source = "{SPECS_DIR}/simple.lua", needed_by = "nonexistent" }}
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

        result, _ = self.install_spec(
            "local.needed_by_invalid@v1", "needed_by_invalid.lua"
        )

        self.assertNotEqual(result.returncode, 0, "Expected invalid phase name to fail")
        # Error message check - keep stderr check for error messages
        self.assertIn("needed_by", result.stderr.lower())

    def test_needed_by_multi_level_chain(self):
        """Multi-level chain: A->B->C with different needed_by phases."""
        # Chain A - top node (depends on B with needed_by="stage")
        self.write_spec(
            "needed_by_chain_a.lua",
            """-- Tests multi-level chain - top node (depends on B with needed_by="stage")
IDENTITY = "local.needed_by_chain_a@v1"

DEPENDENCIES = {{
  {{ spec = "local.needed_by_chain_b@v1", source = "{SPECS_DIR}/needed_by_chain_b.lua", needed_by = "stage" }},
  -- Must declare transitive dependency C if we access it
  {{ spec = "local.needed_by_chain_c@v1", source = "{SPECS_DIR}/needed_by_chain_c.lua" }}
}}

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
  -- Can access chain_b in stage phase
  envy.package("local.needed_by_chain_b@v1")
  -- Can access chain_c (transitively available)
  envy.package("local.needed_by_chain_c@v1")
end
""",
        )

        # Chain B - middle node (depends on C with needed_by="build")
        self.write_spec(
            "needed_by_chain_b.lua",
            """-- Tests multi-level chain - middle node (depends on C with needed_by="build")
IDENTITY = "local.needed_by_chain_b@v1"

DEPENDENCIES = {{
  {{ spec = "local.needed_by_chain_c@v1", source = "{SPECS_DIR}/needed_by_chain_c.lua", needed_by = "build" }}
}}

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  -- Can access chain_c in build phase
  envy.package("local.needed_by_chain_c@v1")
end
""",
        )

        # Chain C - leaf node
        self.write_spec(
            "needed_by_chain_c.lua",
            """-- Tests multi-level chain - leaf node
IDENTITY = "local.needed_by_chain_c@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end
""",
        )

        result, parser = self.install_spec(
            "local.needed_by_chain_a@v1", "needed_by_chain_a.lua"
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertIn("local.needed_by_chain_a@v1", parser.registered_specs())
        self.assertIn("local.needed_by_chain_b@v1", parser.registered_specs())
        self.assertIn("local.needed_by_chain_c@v1", parser.registered_specs())

        # Verify all three specs completed successfully
        for spec in [
            "local.needed_by_chain_a@v1",
            "local.needed_by_chain_b@v1",
            "local.needed_by_chain_c@v1",
        ]:
            completes = parser.filter_by_spec_and_event(spec, "phase_complete")
            self.assertGreater(len(completes), 0, f"Expected {spec} to complete phases")

    def test_needed_by_diamond(self):
        """Diamond: A depends on B+C with different needed_by phases."""
        # Diamond A - depends on B (needed_by="fetch") and C (needed_by="build")
        self.write_spec(
            "needed_by_diamond_a.lua",
            """-- Tests diamond with mixed needed_by phases
-- A depends on B (needed_by="fetch") and C (needed_by="build")
IDENTITY = "local.needed_by_diamond_a@v1"

DEPENDENCIES = {{
  {{ spec = "local.needed_by_diamond_b@v1", source = "{SPECS_DIR}/needed_by_diamond_b.lua", needed_by = "fetch" }},
  {{ spec = "local.needed_by_diamond_c@v1", source = "{SPECS_DIR}/needed_by_diamond_c.lua", needed_by = "build" }}
}}

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  -- Can access both B and C in build phase
  envy.package("local.needed_by_diamond_b@v1")
  envy.package("local.needed_by_diamond_c@v1")
end
""",
        )

        # Diamond B - left side (needed_by="fetch")
        self.write_spec(
            "needed_by_diamond_b.lua",
            """-- Tests diamond with mixed needed_by - left side (needed_by="fetch")
IDENTITY = "local.needed_by_diamond_b@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end
""",
        )

        # Diamond C - right side (needed_by="build")
        self.write_spec(
            "needed_by_diamond_c.lua",
            """-- Tests diamond with mixed needed_by - right side (needed_by="build")
IDENTITY = "local.needed_by_diamond_c@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end
""",
        )

        result, parser = self.install_spec(
            "local.needed_by_diamond_a@v1", "needed_by_diamond_a.lua"
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertIn("local.needed_by_diamond_a@v1", parser.registered_specs())
        self.assertIn("local.needed_by_diamond_b@v1", parser.registered_specs())
        self.assertIn("local.needed_by_diamond_c@v1", parser.registered_specs())

        # Verify all three specs completed
        for spec in [
            "local.needed_by_diamond_a@v1",
            "local.needed_by_diamond_b@v1",
            "local.needed_by_diamond_c@v1",
        ]:
            completes = parser.filter_by_spec_and_event(spec, "phase_complete")
            self.assertGreater(len(completes), 0, f"Expected {spec} to complete phases")

    def test_needed_by_race_condition(self):
        """Dependency completes before parent discovers it - late edge addition handled."""
        # Race condition test - uses simple/fast dependency to increase race chance
        self.write_spec(
            "needed_by_race_parent.lua",
            """-- Tests race condition where dependency completes before parent discovers it
-- Uses a very simple/fast dependency to increase chance of race
IDENTITY = "local.needed_by_race_parent@v1"

DEPENDENCIES = {{
  {{ spec = "local.dep_val_lib@v1", source = "{SPECS_DIR}/dep_val_lib.lua", needed_by = "stage" }}
}}

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
  -- Access simple which may have already completed
  envy.package("local.dep_val_lib@v1")
end
""",
        )

        # This tests the if (dep_acc->second.completed) { try_put() } logic
        # Run with parallel jobs to increase chance of race
        env = os.environ.copy()
        env["ENVY_TEST_JOBS"] = "8"

        result, parser = self.install_spec(
            "local.needed_by_race_parent@v1", "needed_by_race_parent.lua", env=env
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertIn("local.needed_by_race_parent@v1", parser.registered_specs())

        completes = parser.filter_by_spec_and_event(
            "local.needed_by_race_parent@v1", "phase_complete"
        )
        self.assertGreater(len(completes), 0, "Expected parent to complete phases")

    def test_needed_by_with_cache_hit(self):
        """Spec with needed_by where dependency is already cached."""
        # Cache test - dependency should be cached on second run
        self.write_spec(
            "needed_by_cached_parent.lua",
            """-- Tests needed_by with cache hits - dependency should be cached on second run
IDENTITY = "local.needed_by_cached_parent@v1"

DEPENDENCIES = {{
  {{ spec = "local.dep_val_lib@v1", source = "{SPECS_DIR}/dep_val_lib.lua", needed_by = "build" }}
}}

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  -- Access simple in build phase
  envy.package("local.dep_val_lib@v1")
end
""",
        )

        result1, parser1 = self.install_spec(
            "local.needed_by_cached_parent@v1",
            "needed_by_cached_parent.lua",
            trace="trace1.jsonl",
        )

        self.assertEqual(result1.returncode, 0, f"stderr: {result1.stderr}")

        # Verify first run had cache miss for dependency
        cache_misses = parser1.filter_by_event("cache_miss")
        self.assertGreater(len(cache_misses), 0, "Expected cache misses on first run")

        # Second run: cache hit (should still respect needed_by)
        result2, parser2 = self.install_spec(
            "local.needed_by_cached_parent@v1",
            "needed_by_cached_parent.lua",
            trace="trace2.jsonl",
        )

        self.assertEqual(result2.returncode, 0, f"stderr: {result2.stderr}")
        self.assertIn("local.needed_by_cached_parent@v1", parser2.registered_specs())

        # Verify second run had cache hits
        cache_hits = parser2.filter_by_event("cache_hit")
        self.assertGreater(len(cache_hits), 0, "Expected cache hits on second run")

    def test_needed_by_all_phases(self):
        """Spec with multiple dependencies using different needed_by phases."""
        # Write the dependency specs first (reuse from other tests)
        self.write_spec(
            "needed_by_fetch_dep.lua",
            """-- Dependency for needed_by="fetch" test
IDENTITY = "local.needed_by_fetch_dep@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end
""",
        )

        self.write_spec(
            "needed_by_check_dep.lua",
            """-- Dependency for needed_by="check" test
IDENTITY = "local.needed_by_check_dep@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end
""",
        )

        self.write_spec(
            "needed_by_stage_dep.lua",
            """-- Dependency for needed_by="stage" test
IDENTITY = "local.needed_by_stage_dep@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end
""",
        )

        self.write_spec(
            "needed_by_build_dep.lua",
            """-- Dependency for needed_by="build" test
IDENTITY = "local.needed_by_build_dep@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end
""",
        )

        self.write_spec(
            "needed_by_install_dep.lua",
            """-- Dependency for needed_by="install" test
IDENTITY = "local.needed_by_install_dep@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end
""",
        )

        # All phases spec - multiple dependencies with different needed_by phases
        self.write_spec(
            "needed_by_all_phases.lua",
            """-- Tests spec with multiple dependencies using different needed_by phases
IDENTITY = "local.needed_by_all_phases@v1"

DEPENDENCIES = {{
  {{ spec = "local.needed_by_fetch_dep@v1", source = "{SPECS_DIR}/needed_by_fetch_dep.lua", needed_by = "fetch" }},
  {{ spec = "local.needed_by_check_dep@v1", source = "{SPECS_DIR}/needed_by_check_dep.lua", needed_by = "check" }},
  {{ spec = "local.needed_by_stage_dep@v1", source = "{SPECS_DIR}/needed_by_stage_dep.lua", needed_by = "stage" }},
  {{ spec = "local.needed_by_build_dep@v1", source = "{SPECS_DIR}/needed_by_build_dep.lua", needed_by = "build" }},
  {{ spec = "local.needed_by_install_dep@v1", source = "{SPECS_DIR}/needed_by_install_dep.lua", needed_by = "install" }}
}}

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  -- Access all dependencies
  envy.package("local.needed_by_fetch_dep@v1")
  envy.package("local.needed_by_check_dep@v1")
  envy.package("local.needed_by_stage_dep@v1")
  envy.package("local.needed_by_build_dep@v1")
end

INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  envy.package("local.needed_by_install_dep@v1")
end
""",
        )

        result, parser = self.install_spec(
            "local.needed_by_all_phases@v1", "needed_by_all_phases.lua"
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertIn("local.needed_by_all_phases@v1", parser.registered_specs())

        # Verify spec completed and has multiple dependencies
        deps = parser.get_dependency_added_events("local.needed_by_all_phases@v1")
        self.assertGreater(len(deps), 1, "Expected multiple dependencies")


if __name__ == "__main__":
    unittest.main()
