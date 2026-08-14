"""Functional tests for engine spec loading and validation.

Tests the spec fetch phase: loading recipes, validating identity field,
verifying spec SHA256, and checking basic structure requirements.
"""

import hashlib
import os
import tempfile
from pathlib import Path
import unittest

from . import test_config
from .env import EnvyTestCase
from .trace_parser import TraceParser


class TestEngineSpecLoading(EnvyTestCase):
    """Tests for spec loading and validation phase."""

    def setUp(self):
        super().setUp()
        self.specs_dir = self.make_temp_dir("specs_dir")
        # Enable trace for all tests if ENVY_TEST_TRACE is set
        self.trace_flag = ["--trace"] if os.environ.get("ENVY_TEST_TRACE") else []

    def write_spec(self, name: str, content: str) -> Path:
        """Write a spec file to the temp specs directory."""
        path = self.specs_dir / name
        path.write_text(content, encoding="utf-8")
        return path

    def get_file_hash(self, filepath):
        """Get SHA256 hash of file using envy hash command."""
        result = test_config.run(
            [str(self.envy), "hash", str(filepath)],
            capture_output=True,
            text=True,
            check=True,
        )
        return result.stdout.strip().split("  ", 1)[0]

    def test_single_local_spec_no_deps(self):
        """Engine loads single local spec with no dependencies."""
        # Minimal test spec - no dependencies
        simple_spec = """-- Minimal test spec - no dependencies
IDENTITY = "local.simple@v1"
DEPENDENCIES = {}

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic package - no cache interaction
    end,
  },
}

"""
        spec_path = self.write_spec("simple.lua", simple_spec)

        manifest = test_config.write_spec_manifest(
            self.specs_dir, [("local.simple@v1", spec_path)]
        )
        trace_file = self.cache_root / "trace.jsonl"

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

        # The spec is the only package the engine resolved
        self.assertEqual(
            TraceParser(trace_file).registered_specs(), {"local.simple@v1"}
        )

    def test_validation_no_phases(self):
        """Engine rejects spec with no phases."""
        # Invalid spec - no phases defined
        no_phases_spec = """-- Invalid spec - no phases defined
IDENTITY = "local.nophases@v1"
DEPENDENCIES = {}
"""
        spec_path = self.write_spec("no_phases.lua", no_phases_spec)

        manifest = test_config.write_spec_manifest(
            self.specs_dir, [("local.nophases@v1", spec_path)]
        )

        result = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                *self.trace_flag,
                "install",
                "--manifest",
                str(manifest),
            ],
            capture_output=True,
            text=True,
        )

        self.assertNotEqual(
            result.returncode, 0, "Expected validation to cause failure"
        )
        # Error should mention missing phases
        stderr_lower = result.stderr.lower()
        self.assertTrue(
            "check" in stderr_lower
            or "install" in stderr_lower
            or "fetch" in stderr_lower,
            f"Expected phase validation error, got: {result.stderr}",
        )

    def test_sha256_verification_success(self):
        """Spec with correct SHA256 succeeds."""
        # Remote spec with no dependencies (dependency target)
        remote_child_spec = """-- remote.child@v1
-- Remote spec with no dependencies

IDENTITY = "remote.child@v1"

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      envy.info("Installing remote child recipe")
    end,
  },
}

"""
        child_spec_path = self.write_spec("remote_child.lua", remote_child_spec)

        # Compute actual SHA256 of remote_child.lua
        with open(child_spec_path, "rb") as f:
            actual_sha256 = hashlib.sha256(f.read()).hexdigest()

        # Create a temporary spec that depends on remote_child with correct SHA256
        with tempfile.NamedTemporaryFile(mode="w", suffix=".lua", delete=False) as tmp:
            tmp.write(f"""
-- test.sha256_ok@v1
IDENTITY = "test.sha256_ok@v1"
DEPENDENCIES = {{
  {{
    spec = "remote.child@v1",
    source = "{child_spec_path.as_posix()}",
    sha256 = "{actual_sha256}"
  }}
}}

USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      envy.info("SHA256 verification succeeded")
    end,
  }},
}}

""")
            tmp_path = tmp.name

        try:
            manifest = test_config.write_spec_manifest(
                self.specs_dir, [("test.sha256_ok@v1", tmp_path)]
            )
            trace_file = self.cache_root / "trace.jsonl"

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
            self.assertEqual(
                TraceParser(trace_file).registered_specs(),
                {"test.sha256_ok@v1", "remote.child@v1"},
            )
        finally:
            Path(tmp_path).unlink()

    def test_sha256_verification_failure(self):
        """Spec with incorrect SHA256 fails."""
        # Remote spec with no dependencies (dependency target)
        remote_child_spec = """-- remote.child@v1
-- Remote spec with no dependencies

IDENTITY = "remote.child@v1"

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      envy.info("Installing remote child recipe")
    end,
  },
}

"""
        child_spec_path = self.write_spec("remote_child.lua", remote_child_spec)
        wrong_sha256 = (
            "0000000000000000000000000000000000000000000000000000000000000000"
        )

        # Create a temporary spec that depends on remote_child with wrong SHA256
        with tempfile.NamedTemporaryFile(mode="w", suffix=".lua", delete=False) as tmp:
            tmp.write(f"""
-- test.sha256_fail@v1
IDENTITY = "test.sha256_fail@v1"
DEPENDENCIES = {{
  {{
    spec = "remote.child@v1",
    source = "{child_spec_path.as_posix()}",
    sha256 = "{wrong_sha256}"
  }}
}}

USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      envy.info("This should not execute")
    end,
  }},
}}

""")
            tmp_path = tmp.name

        try:
            manifest = test_config.write_spec_manifest(
                self.specs_dir, [("test.sha256_fail@v1", tmp_path)]
            )

            result = test_config.run(
                [
                    str(self.envy),
                    f"--cache-root={self.cache_root}",
                    *self.trace_flag,
                    "install",
                    "--manifest",
                    str(manifest),
                ],
                capture_output=True,
                text=True,
            )

            self.assertNotEqual(
                result.returncode, 0, "Expected SHA256 mismatch to cause failure"
            )
            self.assertIn(
                "SHA256 mismatch",
                result.stderr,
                f"Expected SHA256 error, got: {result.stderr}",
            )
            self.assertIn(
                wrong_sha256,
                result.stderr,
                f"Expected wrong hash in error, got: {result.stderr}",
            )
        finally:
            Path(tmp_path).unlink()

    def test_identity_validation_correct(self):
        """Spec with correct identity declaration succeeds."""
        # Spec with correct identity declaration (valid)
        identity_correct_spec = """-- Spec with correct identity declaration (valid)
IDENTITY = "local.identity_correct@v1"
DEPENDENCIES = {}

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      envy.info("Identity validation passed")
    end,
  },
}

"""
        spec_path = self.write_spec("identity_correct.lua", identity_correct_spec)

        manifest = test_config.write_spec_manifest(
            self.specs_dir, [("local.identity_correct@v1", spec_path)]
        )
        trace_file = self.cache_root / "trace.jsonl"

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
        self.assertEqual(
            TraceParser(trace_file).registered_specs(), {"local.identity_correct@v1"}
        )

    def test_identity_validation_missing(self):
        """Spec missing identity field fails with clear error."""
        # Spec missing identity declaration (invalid)
        identity_missing_spec = """-- Spec missing identity declaration (invalid)
DEPENDENCIES = {}

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- This should never execute
    end,
  },
}

"""
        spec_path = self.write_spec("identity_missing.lua", identity_missing_spec)

        manifest = test_config.write_spec_manifest(
            self.specs_dir, [("local.identity_missing@v1", spec_path)]
        )

        result = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                *self.trace_flag,
                "install",
                "--manifest",
                str(manifest),
            ],
            capture_output=True,
            text=True,
        )

        self.assertNotEqual(
            result.returncode, 0, "Expected missing identity to cause failure"
        )
        self.assertIn(
            "must define 'identity' global as a string",
            result.stderr.lower(),
            f"Expected identity field error, got: {result.stderr}",
        )
        self.assertIn(
            "local.identity_missing@v1",
            result.stderr,
            f"Expected spec identity in error, got: {result.stderr}",
        )

    def test_identity_validation_mismatch(self):
        """Spec with wrong identity fails with clear error."""
        # Spec with wrong identity declaration (mismatch)
        identity_mismatch_spec = """-- Spec with wrong identity declaration (mismatch)
IDENTITY = "local.wrong_identity@v1"
DEPENDENCIES = {}

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- This should never execute
    end,
  },
}

"""
        spec_path = self.write_spec("identity_mismatch.lua", identity_mismatch_spec)

        manifest = test_config.write_spec_manifest(
            self.specs_dir, [("local.identity_expected@v1", spec_path)]
        )

        result = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                *self.trace_flag,
                "install",
                "--manifest",
                str(manifest),
            ],
            capture_output=True,
            text=True,
        )

        self.assertNotEqual(
            result.returncode, 0, "Expected identity mismatch to cause failure"
        )
        self.assertIn(
            "identity mismatch",
            result.stderr.lower(),
            f"Expected identity mismatch error, got: {result.stderr}",
        )
        self.assertIn(
            "local.identity_expected@v1",
            result.stderr,
            f"Expected expected identity in error, got: {result.stderr}",
        )
        self.assertIn(
            "local.wrong_identity@v1",
            result.stderr,
            f"Expected declared identity in error, got: {result.stderr}",
        )

    def test_identity_validation_wrong_type(self):
        """Spec with identity as wrong type fails with clear error."""
        # Spec with identity as wrong type (table instead of string)
        identity_wrong_type_spec = """-- Spec with identity as wrong type (table instead of string)
IDENTITY = { name = "wrong" }
DEPENDENCIES = {}

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- This should never execute
    end,
  },
}

"""
        spec_path = self.write_spec("identity_wrong_type.lua", identity_wrong_type_spec)

        manifest = test_config.write_spec_manifest(
            self.specs_dir, [("local.identity_wrong_type@v1", spec_path)]
        )

        result = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                *self.trace_flag,
                "install",
                "--manifest",
                str(manifest),
            ],
            capture_output=True,
            text=True,
        )

        self.assertNotEqual(
            result.returncode, 0, "Expected wrong type to cause failure"
        )
        self.assertIn(
            "must define 'identity' global as a string",
            result.stderr.lower(),
            f"Expected type error, got: {result.stderr}",
        )
        self.assertIn(
            "local.identity_wrong_type@v1",
            result.stderr,
            f"Expected spec identity in error, got: {result.stderr}",
        )

    def test_identity_validation_local_spec(self):
        """Local specs also require identity validation (no exemption)."""
        # Create temp local spec without identity
        with tempfile.NamedTemporaryFile(mode="w", suffix=".lua", delete=False) as tmp:
            tmp.write("""
-- Missing identity in local spec
DEPENDENCIES = {}
USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options) return false end,
    INSTALL = function(pkg_dir, options) end,
  },
}

""")
            tmp_path = tmp.name

        try:
            manifest = test_config.write_spec_manifest(
                self.specs_dir, [("local.temp_no_identity@v1", tmp_path)]
            )

            result = test_config.run(
                [
                    str(self.envy),
                    f"--cache-root={self.cache_root}",
                    *self.trace_flag,
                    "install",
                    "--manifest",
                    str(manifest),
                ],
                capture_output=True,
                text=True,
            )

            self.assertNotEqual(
                result.returncode, 0, "Expected local spec without identity to fail"
            )
            self.assertIn(
                "must define 'identity' global as a string",
                result.stderr.lower(),
                f"Expected identity field error for local spec, got: {result.stderr}",
            )
        finally:
            Path(tmp_path).unlink()

    # -- OPTIONS table form tests --

    def test_options_table_required_present_succeeds(self):
        """OPTIONS table with required option present succeeds."""
        spec = """IDENTITY = "test.options_table_ok@v1"

OPTIONS = { version = { required = true } }

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options) return true end,
    INSTALL = function(pkg_dir, options) end,
  },
}

"""
        spec_path = self.write_spec("options_table_ok.lua", spec)

        entry = test_config.spec_entry(
            "test.options_table_ok@v1", spec_path, options='{ version = "1.0" }'
        )
        manifest = test_config.write_spec_manifest(self.specs_dir, [entry])

        result = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                *self.trace_flag,
                "install",
                "--manifest",
                str(manifest),
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

    def test_options_table_required_missing_fails(self):
        """OPTIONS table with required option missing fails."""
        spec = """IDENTITY = "test.options_table_missing@v1"

OPTIONS = { version = { required = true } }

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options) return true end,
    INSTALL = function(pkg_dir, options) end,
  },
}

"""
        spec_path = self.write_spec("options_table_missing.lua", spec)

        manifest = test_config.write_spec_manifest(
            self.specs_dir, [("test.options_table_missing@v1", spec_path)]
        )

        result = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                *self.trace_flag,
                "install",
                "--manifest",
                str(manifest),
            ],
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("'version' is required", result.stderr)

    def test_options_table_unknown_option_fails(self):
        """OPTIONS table rejects unknown options."""
        spec = """IDENTITY = "test.options_table_unknown@v1"

OPTIONS = { version = {} }

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options) return true end,
    INSTALL = function(pkg_dir, options) end,
  },
}

"""
        spec_path = self.write_spec("options_table_unknown.lua", spec)

        entry = test_config.spec_entry(
            "test.options_table_unknown@v1",
            spec_path,
            options='{ version = "1", typo = "x" }',
        )
        manifest = test_config.write_spec_manifest(self.specs_dir, [entry])

        result = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                *self.trace_flag,
                "install",
                "--manifest",
                str(manifest),
            ],
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unknown option", result.stderr)

    def test_options_table_semver_valid_succeeds(self):
        """OPTIONS table with type='semver' and valid semver succeeds."""
        spec = """IDENTITY = "test.options_semver_ok@v1"

OPTIONS = { version = { type = "semver" } }

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options) return true end,
    INSTALL = function(pkg_dir, options) end,
  },
}

"""
        spec_path = self.write_spec("options_semver_ok.lua", spec)

        entry = test_config.spec_entry(
            "test.options_semver_ok@v1", spec_path, options='{ version = "1.2.3" }'
        )
        manifest = test_config.write_spec_manifest(self.specs_dir, [entry])

        result = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                *self.trace_flag,
                "install",
                "--manifest",
                str(manifest),
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

    def test_options_table_semver_invalid_fails(self):
        """OPTIONS table with type='semver' and invalid semver fails."""
        spec = """IDENTITY = "test.options_semver_bad@v1"

OPTIONS = { version = { type = "semver" } }

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options) return true end,
    INSTALL = function(pkg_dir, options) end,
  },
}

"""
        spec_path = self.write_spec("options_semver_bad.lua", spec)

        entry = test_config.spec_entry(
            "test.options_semver_bad@v1",
            spec_path,
            options='{ version = "not-semver" }',
        )
        manifest = test_config.write_spec_manifest(self.specs_dir, [entry])

        result = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                *self.trace_flag,
                "install",
                "--manifest",
                str(manifest),
            ],
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not valid semver", result.stderr)

    def test_options_table_semver_range_pass(self):
        """OPTIONS table semver range match succeeds."""
        spec = """IDENTITY = "test.options_range_ok@v1"

OPTIONS = { version = { type = "semver", range = ">=1.0.0 <2.0.0" } }

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options) return true end,
    INSTALL = function(pkg_dir, options) end,
  },
}

"""
        spec_path = self.write_spec("options_range_ok.lua", spec)

        entry = test_config.spec_entry(
            "test.options_range_ok@v1", spec_path, options='{ version = "1.5.0" }'
        )
        manifest = test_config.write_spec_manifest(self.specs_dir, [entry])

        result = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                *self.trace_flag,
                "install",
                "--manifest",
                str(manifest),
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

    def test_options_table_semver_range_fail(self):
        """OPTIONS table semver range mismatch fails."""
        spec = """IDENTITY = "test.options_range_bad@v1"

OPTIONS = { version = { type = "semver", range = ">=1.0.0 <2.0.0" } }

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options) return true end,
    INSTALL = function(pkg_dir, options) end,
  },
}

"""
        spec_path = self.write_spec("options_range_bad.lua", spec)

        entry = test_config.spec_entry(
            "test.options_range_bad@v1", spec_path, options='{ version = "3.0.0" }'
        )
        manifest = test_config.write_spec_manifest(self.specs_dir, [entry])

        result = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                *self.trace_flag,
                "install",
                "--manifest",
                str(manifest),
            ],
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("does not satisfy range", result.stderr)

    def test_options_table_numeric_range_pass(self):
        """OPTIONS table numeric range match succeeds."""
        spec = """IDENTITY = "test.options_numrange_ok@v1"

OPTIONS = { count = { range = ">=1 <10" } }

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options) return true end,
    INSTALL = function(pkg_dir, options) end,
  },
}

"""
        spec_path = self.write_spec("options_numrange_ok.lua", spec)

        entry = test_config.spec_entry(
            "test.options_numrange_ok@v1", spec_path, options="{ count = 5 }"
        )
        manifest = test_config.write_spec_manifest(self.specs_dir, [entry])

        result = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                *self.trace_flag,
                "install",
                "--manifest",
                str(manifest),
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

    def test_options_table_numeric_range_fail(self):
        """OPTIONS table numeric range mismatch fails."""
        spec = """IDENTITY = "test.options_numrange_bad@v1"

OPTIONS = { count = { range = ">=1 <10" } }

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options) return true end,
    INSTALL = function(pkg_dir, options) end,
  },
}

"""
        spec_path = self.write_spec("options_numrange_bad.lua", spec)

        entry = test_config.spec_entry(
            "test.options_numrange_bad@v1", spec_path, options="{ count = 15 }"
        )
        manifest = test_config.write_spec_manifest(self.specs_dir, [entry])

        result = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                *self.trace_flag,
                "install",
                "--manifest",
                str(manifest),
            ],
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("does not satisfy range", result.stderr)

    def test_options_table_custom_validate_pass(self):
        """OPTIONS table custom validate returning nil succeeds."""
        spec = """IDENTITY = "test.options_cv_ok@v1"

OPTIONS = { mode = { validate = function(v) end } }

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options) return true end,
    INSTALL = function(pkg_dir, options) end,
  },
}

"""
        spec_path = self.write_spec("options_cv_ok.lua", spec)

        entry = test_config.spec_entry(
            "test.options_cv_ok@v1", spec_path, options='{ mode = "install" }'
        )
        manifest = test_config.write_spec_manifest(self.specs_dir, [entry])

        result = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                *self.trace_flag,
                "install",
                "--manifest",
                str(manifest),
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

    def test_options_table_custom_validate_fail(self):
        """OPTIONS table custom validate returning string fails."""
        spec = """IDENTITY = "test.options_cv_bad@v1"

OPTIONS = { mode = { validate = function(v) return "bad mode: " .. v end } }

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options) return true end,
    INSTALL = function(pkg_dir, options) end,
  },
}

"""
        spec_path = self.write_spec("options_cv_bad.lua", spec)

        entry = test_config.spec_entry(
            "test.options_cv_bad@v1", spec_path, options='{ mode = "x" }'
        )
        manifest = test_config.write_spec_manifest(self.specs_dir, [entry])

        result = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                *self.trace_flag,
                "install",
                "--manifest",
                str(manifest),
            ],
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("bad mode", result.stderr)

    def test_options_table_optional_missing_succeeds(self):
        """OPTIONS table with optional (no required) and absent succeeds."""
        spec = """IDENTITY = "test.options_opt_ok@v1"

OPTIONS = { version = {} }

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options) return true end,
    INSTALL = function(pkg_dir, options) end,
  },
}

"""
        spec_path = self.write_spec("options_opt_ok.lua", spec)

        manifest = test_config.write_spec_manifest(
            self.specs_dir, [("test.options_opt_ok@v1", spec_path)]
        )

        result = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                *self.trace_flag,
                "install",
                "--manifest",
                str(manifest),
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

    def test_options_table_empty_schema_no_opts_succeeds(self):
        """OPTIONS = {} with empty opts succeeds."""
        spec = """IDENTITY = "test.options_empty@v1"

OPTIONS = {}

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options) return true end,
    INSTALL = function(pkg_dir, options) end,
  },
}

"""
        spec_path = self.write_spec("options_empty.lua", spec)

        manifest = test_config.write_spec_manifest(
            self.specs_dir, [("test.options_empty@v1", spec_path)]
        )

        result = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                *self.trace_flag,
                "install",
                "--manifest",
                str(manifest),
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

    # -- OPTIONS type constraint tests --

    def test_options_table_type_string_pass(self):
        """OPTIONS table with type='string' and valid string succeeds."""
        spec = """IDENTITY = "test.options_type_str_ok@v1"

OPTIONS = { name = { type = "string" } }

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options) return true end,
    INSTALL = function(pkg_dir, options) end,
  },
}

"""
        spec_path = self.write_spec("options_type_str_ok.lua", spec)

        entry = test_config.spec_entry(
            "test.options_type_str_ok@v1", spec_path, options='{ name = "hello" }'
        )
        manifest = test_config.write_spec_manifest(self.specs_dir, [entry])

        result = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                *self.trace_flag,
                "install",
                "--manifest",
                str(manifest),
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

    def test_options_table_type_string_fail(self):
        """OPTIONS table with type='string' and number fails."""
        spec = """IDENTITY = "test.options_type_str_bad@v1"

OPTIONS = { name = { type = "string" } }

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options) return true end,
    INSTALL = function(pkg_dir, options) end,
  },
}

"""
        spec_path = self.write_spec("options_type_str_bad.lua", spec)

        entry = test_config.spec_entry(
            "test.options_type_str_bad@v1", spec_path, options="{ name = 42 }"
        )
        manifest = test_config.write_spec_manifest(self.specs_dir, [entry])

        result = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                *self.trace_flag,
                "install",
                "--manifest",
                str(manifest),
            ],
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("must be type 'string'", result.stderr)

    def test_options_table_type_list_pass(self):
        """OPTIONS table with type='list' and valid list succeeds."""
        spec = """IDENTITY = "test.options_type_list_ok@v1"

OPTIONS = { items = { type = "list" } }

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options) return true end,
    INSTALL = function(pkg_dir, options) end,
  },
}

"""
        spec_path = self.write_spec("options_type_list_ok.lua", spec)

        entry = test_config.spec_entry(
            "test.options_type_list_ok@v1",
            spec_path,
            options='{ items = { "a", "b" } }',
        )
        manifest = test_config.write_spec_manifest(self.specs_dir, [entry])

        result = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                *self.trace_flag,
                "install",
                "--manifest",
                str(manifest),
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

    def test_options_table_type_list_fail(self):
        """OPTIONS table with type='list' and non-sequential table fails."""
        spec = """IDENTITY = "test.options_type_list_bad@v1"

OPTIONS = { items = { type = "list" } }

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options) return true end,
    INSTALL = function(pkg_dir, options) end,
  },
}

"""
        spec_path = self.write_spec("options_type_list_bad.lua", spec)

        entry = test_config.spec_entry(
            "test.options_type_list_bad@v1", spec_path, options="{ items = { a = 1 } }"
        )
        manifest = test_config.write_spec_manifest(self.specs_dir, [entry])

        result = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                *self.trace_flag,
                "install",
                "--manifest",
                str(manifest),
            ],
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("non-sequential table", result.stderr)

    def test_options_table_choices_pass(self):
        """OPTIONS table with choices and valid value succeeds."""
        spec = """IDENTITY = "test.options_choices_ok@v1"

OPTIONS = { mode = { choices = { "install", "extract" } } }

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options) return true end,
    INSTALL = function(pkg_dir, options) end,
  },
}

"""
        spec_path = self.write_spec("options_choices_ok.lua", spec)

        entry = test_config.spec_entry(
            "test.options_choices_ok@v1", spec_path, options='{ mode = "install" }'
        )
        manifest = test_config.write_spec_manifest(self.specs_dir, [entry])

        result = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                *self.trace_flag,
                "install",
                "--manifest",
                str(manifest),
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

    def test_options_table_choices_fail(self):
        """OPTIONS table with choices and invalid value fails."""
        spec = """IDENTITY = "test.options_choices_bad@v1"

OPTIONS = { mode = { choices = { "install", "extract" } } }

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options) return true end,
    INSTALL = function(pkg_dir, options) end,
  },
}

"""
        spec_path = self.write_spec("options_choices_bad.lua", spec)

        entry = test_config.spec_entry(
            "test.options_choices_bad@v1", spec_path, options='{ mode = "debug" }'
        )
        manifest = test_config.write_spec_manifest(self.specs_dir, [entry])

        result = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                *self.trace_flag,
                "install",
                "--manifest",
                str(manifest),
            ],
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not in {install, extract}", result.stderr)

    # -- OPTIONS function form tests --

    def test_options_function_nil_return_succeeds(self):
        """OPTIONS function returning nil succeeds."""
        spec = """IDENTITY = "test.options_fn_nil@v1"

OPTIONS = function(opts) end

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options) return true end,
    INSTALL = function(pkg_dir, options) end,
  },
}

"""
        spec_path = self.write_spec("options_fn_nil.lua", spec)

        manifest = test_config.write_spec_manifest(
            self.specs_dir, [("test.options_fn_nil@v1", spec_path)]
        )

        result = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                *self.trace_flag,
                "install",
                "--manifest",
                str(manifest),
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

    def test_options_function_true_return_succeeds(self):
        """OPTIONS function returning true succeeds."""
        spec = """IDENTITY = "test.options_fn_true@v1"

OPTIONS = function(opts) return true end

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options) return true end,
    INSTALL = function(pkg_dir, options) end,
  },
}

"""
        spec_path = self.write_spec("options_fn_true.lua", spec)

        manifest = test_config.write_spec_manifest(
            self.specs_dir, [("test.options_fn_true@v1", spec_path)]
        )

        result = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                *self.trace_flag,
                "install",
                "--manifest",
                str(manifest),
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

    def test_options_function_false_return_fails(self):
        """OPTIONS function returning false fails."""
        spec = """IDENTITY = "test.options_fn_false@v1"

OPTIONS = function(opts) return false end

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options) return true end,
    INSTALL = function(pkg_dir, options) end,
  },
}

"""
        spec_path = self.write_spec("options_fn_false.lua", spec)

        manifest = test_config.write_spec_manifest(
            self.specs_dir, [("test.options_fn_false@v1", spec_path)]
        )

        result = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                *self.trace_flag,
                "install",
                "--manifest",
                str(manifest),
            ],
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("returned false", result.stderr)

    def test_options_function_string_return_fails(self):
        """OPTIONS function returning string fails with message."""
        spec = """IDENTITY = "test.options_fn_string@v1"

OPTIONS = function(opts) return "nope" end

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options) return true end,
    INSTALL = function(pkg_dir, options) end,
  },
}

"""
        spec_path = self.write_spec("options_fn_string.lua", spec)

        manifest = test_config.write_spec_manifest(
            self.specs_dir, [("test.options_fn_string@v1", spec_path)]
        )

        result = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                *self.trace_flag,
                "install",
                "--manifest",
                str(manifest),
            ],
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("nope", result.stderr)

    def test_options_function_invalid_return_type_fails(self):
        """OPTIONS function returning number fails."""
        spec = """IDENTITY = "test.options_fn_type@v1"

OPTIONS = function(opts) return 123 end

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options) return true end,
    INSTALL = function(pkg_dir, options) end,
  },
}

"""
        spec_path = self.write_spec("options_fn_type.lua", spec)

        manifest = test_config.write_spec_manifest(
            self.specs_dir, [("test.options_fn_type@v1", spec_path)]
        )

        result = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                *self.trace_flag,
                "install",
                "--manifest",
                str(manifest),
            ],
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("OPTIONS must return", result.stderr)

    def test_options_function_runtime_error_fails(self):
        """OPTIONS function error bubbles with context."""
        spec = """IDENTITY = "test.options_fn_error@v1"

OPTIONS = function(opts) error("boom") end

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options) return true end,
    INSTALL = function(pkg_dir, options) end,
  },
}

"""
        spec_path = self.write_spec("options_fn_error.lua", spec)

        manifest = test_config.write_spec_manifest(
            self.specs_dir, [("test.options_fn_error@v1", spec_path)]
        )

        result = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                *self.trace_flag,
                "install",
                "--manifest",
                str(manifest),
            ],
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("boom", result.stderr)

    def test_options_function_calls_envy_options_succeeds(self):
        """OPTIONS function calling envy.options() with valid opts succeeds."""
        spec = """IDENTITY = "test.options_fn_envy_ok@v1"

OPTIONS = function(opts)
  envy.options({ version = { required = true } })
end

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options) return true end,
    INSTALL = function(pkg_dir, options) end,
  },
}

"""
        spec_path = self.write_spec("options_fn_envy_ok.lua", spec)

        entry = test_config.spec_entry(
            "test.options_fn_envy_ok@v1", spec_path, options='{ version = "1.0" }'
        )
        manifest = test_config.write_spec_manifest(self.specs_dir, [entry])

        result = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                *self.trace_flag,
                "install",
                "--manifest",
                str(manifest),
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

    def test_options_function_calls_envy_options_fails(self):
        """OPTIONS function calling envy.options() with invalid opts fails."""
        spec = """IDENTITY = "test.options_fn_envy_bad@v1"

OPTIONS = function(opts)
  envy.options({ version = { required = true } })
end

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options) return true end,
    INSTALL = function(pkg_dir, options) end,
  },
}

"""
        spec_path = self.write_spec("options_fn_envy_bad.lua", spec)

        manifest = test_config.write_spec_manifest(
            self.specs_dir, [("test.options_fn_envy_bad@v1", spec_path)]
        )

        result = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                *self.trace_flag,
                "install",
                "--manifest",
                str(manifest),
            ],
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("'version' is required", result.stderr)

    # -- Error tests --

    def test_options_wrong_type_rejected(self):
        """OPTIONS = 42 is rejected."""
        spec = """IDENTITY = "test.options_badtype@v1"

OPTIONS = 42

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options) return true end,
    INSTALL = function(pkg_dir, options) end,
  },
}

"""
        spec_path = self.write_spec("options_badtype.lua", spec)

        manifest = test_config.write_spec_manifest(
            self.specs_dir, [("test.options_badtype@v1", spec_path)]
        )

        result = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                *self.trace_flag,
                "install",
                "--manifest",
                str(manifest),
            ],
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("must be a table or function", result.stderr)

    def test_options_string_type_rejected(self):
        """OPTIONS = "string" is rejected."""
        spec = """IDENTITY = "test.options_strtype@v1"

OPTIONS = "string"

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options) return true end,
    INSTALL = function(pkg_dir, options) end,
  },
}

"""
        spec_path = self.write_spec("options_strtype.lua", spec)

        manifest = test_config.write_spec_manifest(
            self.specs_dir, [("test.options_strtype@v1", spec_path)]
        )

        result = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                *self.trace_flag,
                "install",
                "--manifest",
                str(manifest),
            ],
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("must be a table or function", result.stderr)

    def test_spec_source_not_found_in_manifest(self):
        """Missing spec source from manifest entry gives clear error."""
        # Create manifest referencing non-existent source
        with tempfile.NamedTemporaryFile(
            mode="w", suffix=".lua", delete=False, dir=self.cache_root
        ) as tmp:
            tmp.write("""
-- @envy bin-dir "tools"
PACKAGES = {
  { spec = "local.missing@v1", source = "nonexistent_source.lua" }
}
""")
            manifest_path = tmp.name

        try:
            result = test_config.run(
                [
                    str(self.envy),
                    f"--cache-root={self.cache_root}",
                    *self.trace_flag,
                    "install",
                    "--manifest",
                    manifest_path,
                ],
                capture_output=True,
                text=True,
            )

            self.assertNotEqual(
                result.returncode, 0, "Expected missing source file to cause failure"
            )
            self.assertIn(
                "Spec source not found",
                result.stderr,
                f"Expected 'Spec source not found' error, got: {result.stderr}",
            )
            self.assertIn(
                "nonexistent_source.lua",
                result.stderr,
                f"Expected source path in error, got: {result.stderr}",
            )
            self.assertIn(
                "local.missing@v1",
                result.stderr,
                f"Expected spec identity in error, got: {result.stderr}",
            )
        finally:
            Path(manifest_path).unlink()


if __name__ == "__main__":
    unittest.main()
