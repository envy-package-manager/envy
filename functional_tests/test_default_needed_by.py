"""Tests for default needed_by phase behavior."""

import hashlib
import io
import subprocess
import tarfile
import unittest
from pathlib import Path

from . import test_config
from .env import EnvyTestCase
from .trace_parser import PkgPhase, TraceParser

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


class TestDefaultNeededBy(EnvyTestCase):
    """Tests verifying default needed_by phase is asset_build."""

    def setUp(self):
        super().setUp()
        self.specs_dir = self.make_temp_dir("specs_dir")

        # Create test archive and get its hash
        self.archive_path = self.specs_dir / "test.tar.gz"
        self.archive_hash = create_test_archive(self.archive_path)

        # Write inline specs
        self._write_specs()

    def _write_specs(self):
        """Write all inline specs to the specs directory."""
        archive_lua_path = self.archive_path.as_posix()

        specs = {
            "dep_val_lib.lua": f'''-- Dependency validation test: base library (no dependencies)
IDENTITY = "local.dep_val_lib@v1"

FETCH = {{
  source = "{archive_lua_path}",
  sha256 = "{self.archive_hash}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  envy.run([[echo "lib built" > lib.txt]])
end
''',
            "default_needed_by_parent.lua": f'''-- Tests default needed_by - no explicit needed_by specified
IDENTITY = "local.default_needed_by_parent@v1"

DEPENDENCIES = {{
  {{ spec = "local.dep_val_lib@v1", source = "dep_val_lib.lua" }}
  -- No needed_by specified - should default to "build"
}}

FETCH = function(tmp_dir, options)
  return "{archive_lua_path}", "{self.archive_hash}"
end

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  -- Dependency should be available here by default
  local dep_path = envy.package("local.dep_val_lib@v1")
end
''',
            "explicit_check_parent.lua": f'''-- Tests explicit needed_by="check" - dependency completes before parent's check phase
IDENTITY = "local.explicit_check_parent@v1"

DEPENDENCIES = {{
  {{ spec = "local.dep_val_lib@v1", source = "dep_val_lib.lua", needed_by = "check" }}
}}

FETCH = function(tmp_dir, options)
  -- Dependency will complete by check phase (before fetch)
  return "{archive_lua_path}", "{self.archive_hash}"
end

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end
''',
            "explicit_fetch_parent.lua": f'''-- Tests explicit needed_by="fetch" - dependency completes before parent's fetch phase
IDENTITY = "local.explicit_fetch_parent@v1"

DEPENDENCIES = {{
  {{ spec = "local.dep_val_lib@v1", source = "dep_val_lib.lua", needed_by = "fetch" }}
}}

FETCH = function(tmp_dir, options)
  -- Dependency will complete before this phase due to explicit needed_by
  return "{archive_lua_path}", "{self.archive_hash}"
end

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end
''',
        }

        for name, content in specs.items():
            (self.specs_dir / name).write_text(content, encoding="utf-8")

    def test_default_needed_by_is_build_not_check(self):
        """Verify default needed_by is asset_build (phase 4), not asset_check."""
        trace_file = self.cache_root / "trace.jsonl"

        spec = self.specs_dir / "default_needed_by_parent.lua"
        manifest = test_config.write_spec_manifest(
            self.specs_dir, [("local.default_needed_by_parent@v1", spec)]
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
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertTrue(trace_file.exists(), "Trace file not created")

        parser = TraceParser(trace_file)

        # Verify dependency was added with correct needed_by phase
        parser.assert_dependency_needed_by(
            "local.default_needed_by_parent@v1",
            "local.dep_val_lib@v1",
            PkgPhase.PKG_BUILD,
        )

        # Verify phase sequence - parent should execute through build phase
        parent_sequence = parser.get_phase_sequence("local.default_needed_by_parent@v1")
        self.assertIn(
            PkgPhase.PKG_BUILD,
            parent_sequence,
            "Parent should reach build phase where dependency is needed",
        )

    def test_explicit_needed_by_check_still_works(self):
        """Verify explicit needed_by='check' still works correctly."""
        trace_file = self.cache_root / "trace.jsonl"

        spec = self.specs_dir / "explicit_check_parent.lua"
        manifest = test_config.write_spec_manifest(
            self.specs_dir, [("local.explicit_check_parent@v1", spec)]
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
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertTrue(trace_file.exists(), "Trace file not created")

        parser = TraceParser(trace_file)

        # Verify dependency was added with explicit needed_by=check (phase 1)
        parser.assert_dependency_needed_by(
            "local.explicit_check_parent@v1",
            "local.dep_val_lib@v1",
            PkgPhase.PKG_CHECK,
        )

    def test_explicit_needed_by_fetch_works(self):
        """Verify explicit needed_by='fetch' works correctly."""
        trace_file = self.cache_root / "trace.jsonl"

        spec = self.specs_dir / "explicit_fetch_parent.lua"
        manifest = test_config.write_spec_manifest(
            self.specs_dir, [("local.explicit_fetch_parent@v1", spec)]
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
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertTrue(trace_file.exists(), "Trace file not created")

        parser = TraceParser(trace_file)

        # Verify dependency was added with explicit needed_by=fetch (phase 2)
        parser.assert_dependency_needed_by(
            "local.explicit_fetch_parent@v1",
            "local.dep_val_lib@v1",
            PkgPhase.PKG_FETCH,
        )


if __name__ == "__main__":
    unittest.main()
