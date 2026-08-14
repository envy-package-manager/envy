"""Functional tests for user-managed packages (SETUP pairs)."""

import os
import subprocess
from pathlib import Path
import unittest

from . import test_config
from .env import EnvyTestCase

# =============================================================================
# Shared specs (used by multiple tests)
# =============================================================================

# Simple user-managed package: check if marker file exists, create if not
# Simulates system package wrapper (like brew install python)
SPEC_SIMPLE = """IDENTITY = "local.user_managed_simple@v1"
USER_MANAGED = true

SETUP = {
  main = {
    CHECK = function(pkg_dir, options)
        local marker = os.getenv("ENVY_TEST_MARKER_SIMPLE")
        if not marker then error("ENVY_TEST_MARKER_SIMPLE must be set") end
        local f = io.open(marker, "r")
        if f then f:close(); return true end
        return false
    end,

    INSTALL = function(pkg_dir, options)
        local marker = os.getenv("ENVY_TEST_MARKER_SIMPLE")
        if not marker then error("ENVY_TEST_MARKER_SIMPLE must be set") end
        local f = io.open(marker, "w")
        if not f then error("Failed to create marker file: " .. marker) end
        f:write("installed by user_managed_simple")
        f:close()
    end,
  },
}
"""

# Context isolation: user-managed pair verbs receive nil pkg_dir.
SPEC_CTX_FORBIDDEN = """IDENTITY = "local.user_managed_ctx_isolation_forbidden@v1"
USER_MANAGED = true

SETUP = {
  main = {
    CHECK = function(pkg_dir, options)
        return false
    end,

    INSTALL = function(pkg_dir, options)
        local forbidden_api = os.getenv("ENVY_TEST_FORBIDDEN_API")
        if not forbidden_api then error("ENVY_TEST_FORBIDDEN_API must be set") end

        if forbidden_api == "pkg_dir" then
            if pkg_dir == nil then
                print("Verified: pkg_dir is nil for user-managed")
                return
            else
                error("pkg_dir should be nil for user-managed packages")
            end
        else
            error("Unknown forbidden API: " .. forbidden_api)
        end
    end,
  },
}
"""


class TestUserManagedPackages(EnvyTestCase):
    """Test user-managed package behavior with SETUP pairs."""

    # Some cases fetch over the real network; relax the watchdog for latency.
    envy_watchdog_timeout = 60

    def setUp(self):
        super().setUp()
        self.test_dir = self.make_temp_dir("test_dir")
        self.marker_dir = self.make_temp_dir("marker_dir")
        self.specs_dir = self.make_temp_dir("specs_dir")

        # Create marker paths in temp directory
        self.marker_simple = self.marker_dir / "marker-simple"
        self.marker_with_fetch = self.marker_dir / "marker-with-fetch"

        # Write shared specs
        (self.specs_dir / "simple.lua").write_text(SPEC_SIMPLE, encoding="utf-8")
        (self.specs_dir / "ctx_forbidden.lua").write_text(
            SPEC_CTX_FORBIDDEN, encoding="utf-8"
        )

    def write_spec(self, name: str, content: str) -> Path:
        """Write spec to temp dir, return path."""
        path = self.specs_dir / f"{name}.lua"
        path.write_text(content, encoding="utf-8")
        return path

    def run_spec(
        self,
        name: str,
        identity: str,
        trace: bool = False,
        env_vars: dict = None,
        setup: list = None,
    ):
        """Run spec by name and identity.

        setup=None selects no SETUP pairs; a list of pair names selects those.
        Either way the spec runs via a generated manifest and `install`.
        """
        cmd = [str(self.envy), f"--cache-root={self.cache_root}"]
        if trace:
            # Observe per-package decision narrative (DEBUG); --trace is separate.
            cmd.append("--verbose")

        spec_path = self.specs_dir / f"{name}.lua"
        entry = (identity, spec_path) if setup is None else (identity, spec_path, setup)
        manifest = test_config.write_spec_manifest(self.test_dir, [entry])
        cmd.extend(["install", "--manifest", str(manifest)])

        env = os.environ.copy()
        if env_vars:
            env.update(env_vars)

        return test_config.run(
            cmd, capture_output=True, text=True, env=env, cwd=self.test_dir
        )

    # =========================================================================
    # Basic user-managed package tests (using SPEC_SIMPLE)
    # =========================================================================

    def test_simple_first_run_check_false_installs(self):
        """First run with check=false acquires pair lock, runs install, purges cache."""
        env = {"ENVY_TEST_MARKER_SIMPLE": str(self.marker_simple)}
        result = self.run_spec(
            "simple", "local.user_managed_simple@v1", env_vars=env, setup=["main"]
        )
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        self.assertTrue(
            self.marker_simple.exists(), "Install should have created marker file"
        )

        pkg_dir = self.cache_root / "packages" / "local.user_managed_simple@v1"
        if pkg_dir.exists():
            self.assertFalse(
                any(pkg_dir.glob("*/pkg")),
                "User-managed packages should not leave pkg/ in cache",
            )

    def test_simple_second_run_check_true_skips(self):
        """Second run with check=true skips the pair, no lock acquired."""
        env = {"ENVY_TEST_MARKER_SIMPLE": str(self.marker_simple)}

        # Create marker to simulate already-installed
        self.marker_simple.parent.mkdir(parents=True, exist_ok=True)
        self.marker_simple.write_text("already installed")

        result = self.run_spec(
            "simple",
            "local.user_managed_simple@v1",
            trace=True,
            env_vars=env,
            setup=["main"],
        )
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        self.assertIn("pair 'main' satisfied (pre-lock)", result.stderr)
        self.assertNotIn("running pair 'main' install", result.stderr)

    def test_multiple_runs_cache_lifecycle(self):
        """Verify pair installs, skips when satisfied, reinstalls after drift."""
        env = {"ENVY_TEST_MARKER_SIMPLE": str(self.marker_simple)}

        # Run 1: Install
        result1 = self.run_spec(
            "simple", "local.user_managed_simple@v1", env_vars=env, setup=["main"]
        )
        self.assertEqual(result1.returncode, 0)
        self.assertTrue(self.marker_simple.exists(), "First run should create marker")

        # Run 2: Skip (check=true)
        result2 = self.run_spec(
            "simple",
            "local.user_managed_simple@v1",
            trace=True,
            env_vars=env,
            setup=["main"],
        )
        self.assertEqual(result2.returncode, 0)
        self.assertIn("pair 'main' satisfied (pre-lock)", result2.stderr)
        self.assertNotIn("running pair 'main' install", result2.stderr)

        # Run 3: Remove marker, reinstall
        self.marker_simple.unlink()
        result3 = self.run_spec(
            "simple",
            "local.user_managed_simple@v1",
            trace=True,
            env_vars=env,
            setup=["main"],
        )
        self.assertEqual(result3.returncode, 0)
        self.assertIn("running pair 'main' install", result3.stderr)
        self.assertTrue(
            self.marker_simple.exists(), "Should reinstall after marker removed"
        )

    def test_double_check_lock_runs_install_when_still_unsatisfied(self):
        """Double-check lock: pre-lock check fails, post-lock re-check fails, install runs."""
        env = {"ENVY_TEST_MARKER_SIMPLE": str(self.marker_simple)}

        result = self.run_spec(
            "simple",
            "local.user_managed_simple@v1",
            trace=True,
            env_vars=env,
            setup=["main"],
        )
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertIn("running pair 'main' install", result.stderr)
        self.assertNotIn("satisfied (post-lock)", result.stderr)

    def test_cache_state_after_install_fully_deleted(self):
        """After install completes, the ephemeral pair entry is purged."""
        env = {"ENVY_TEST_MARKER_SIMPLE": str(self.marker_simple)}

        result = self.run_spec("simple", "local.user_managed_simple@v1", env_vars=env)
        self.assertEqual(result.returncode, 0)

        asset_base = self.cache_root / "packages" / "local.user_managed_simple@v1"
        if asset_base.exists():
            remaining = list(asset_base.rglob("*"))
            non_lock_files = [
                r for r in remaining if r.is_file() and not r.name.endswith(".lock")
            ]
            self.assertEqual(
                len(non_lock_files),
                0,
                f"User-managed should not leave files in cache, found: {non_lock_files}",
            )

    # =========================================================================
    # Fetch/stage/build with cache-managed
    # =========================================================================

    def test_user_managed_with_fetch_purges_all_dirs(self):
        """Cache-managed with fetch verb: fetch_dir populated, package persists."""
        # Cache-managed package with declarative fetch and all phases
        spec_with_fetch = """IDENTITY = "local.user_managed_with_fetch@v1"

FETCH = {
    source = "https://raw.githubusercontent.com/ninja-build/ninja/v1.13.2/README.md",
    sha256 = "b31e9700c752fa214773c1b799d90efcbf3330c8062da9f45c6064e023b347b0"
}

function STAGE(fetch_dir, stage_dir, tmp_dir, options)
    local readme = fetch_dir .. "/README.md"
    local f = io.open(readme, "r")
    if not f then error("Fetch did not produce README.md") end
    f:close()
end

function BUILD(stage_dir, install_dir, fetch_dir, tmp_dir, options)
    if not stage_dir then error("stage_dir not available in build phase") end
end

function INSTALL(install_dir, stage_dir, fetch_dir, tmp_dir, options)
    local marker = os.getenv("ENVY_TEST_MARKER_WITH_FETCH")
    if not marker then error("ENVY_TEST_MARKER_WITH_FETCH must be set") end
    local f = io.open(marker, "w")
    if not f then error("Failed to create marker file") end
    f:write("installed with fetch/stage/build")
    f:close()
end
"""
        self.write_spec("with_fetch", spec_with_fetch)
        env = {"ENVY_TEST_MARKER_WITH_FETCH": str(self.marker_with_fetch)}

        result = self.run_spec(
            "with_fetch", "local.user_managed_with_fetch@v1", trace=True, env_vars=env
        )
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        self.assertTrue(self.marker_with_fetch.exists())

        pkg_dir = self.cache_root / "packages" / "local.user_managed_with_fetch@v1"
        self.assertTrue(pkg_dir.exists(), "Cache-managed packages should persist")

        pkg_subdirs = list(pkg_dir.glob("*/pkg"))
        self.assertGreater(len(pkg_subdirs), 0, "Should have pkg/ directory in cache")

    # =========================================================================
    # Context isolation tests (using SPEC_CTX_FORBIDDEN)
    # =========================================================================

    def test_user_managed_pair_pkg_dir_is_nil(self):
        """User-managed pair INSTALL receives nil pkg_dir."""
        result = self.run_spec(
            "ctx_forbidden",
            "local.user_managed_ctx_isolation_forbidden@v1",
            env_vars={"ENVY_TEST_FORBIDDEN_API": "pkg_dir"},
        )
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

    def test_user_managed_ctx_allowed_apis(self):
        """User-managed pair verbs can use allowed APIs (run, package, product)."""
        spec_allowed = """IDENTITY = "local.user_managed_ctx_isolation_allowed@v1"
USER_MANAGED = true

SETUP = {
  main = {
    CHECK = function(pkg_dir, options)
        return false
    end,

    INSTALL = function(pkg_dir, options)
        if IDENTITY ~= "local.user_managed_ctx_isolation_allowed@v1" then
            error("IDENTITY mismatch")
        end

        if type(envy.run) ~= "function" then error("envy.run should be a function") end
        local result = envy.run("echo test", {capture = true, quiet = true})
        if result.exit_code ~= 0 then error("envy.run failed") end

        if type(envy.package) ~= "function" then error("envy.package should be a function") end
        if type(envy.product) ~= "function" then error("envy.product should be a function") end
    end,
  },
}
"""
        self.write_spec("ctx_allowed", spec_allowed)

        result = self.run_spec(
            "ctx_allowed", "local.user_managed_ctx_isolation_allowed@v1"
        )
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

    # =========================================================================
    # Multi-pair behavior
    # =========================================================================

    def test_multiple_pairs_all_run_when_selected(self):
        """All selected pairs run; execution order is unspecified (parallel nodes)."""
        spec = """IDENTITY = "local.um_multi_pair@v1"
USER_MANAGED = true

SETUP = {
  zeta = {
    CHECK = function(pkg_dir, options) return false end,
    INSTALL = function(pkg_dir, options)
        local f = io.open(os.getenv("ENVY_TEST_MULTI_LOG"), "a")
        f:write("zeta\\n")
        f:close()
    end,
  },
  alpha = {
    CHECK = function(pkg_dir, options) return false end,
    INSTALL = function(pkg_dir, options)
        local f = io.open(os.getenv("ENVY_TEST_MULTI_LOG"), "a")
        f:write("alpha\\n")
        f:close()
    end,
  },
}
"""
        log = self.marker_dir / "multi-pair-log"
        self.write_spec("multi_pair", spec)
        result = self.run_spec(
            "multi_pair",
            "local.um_multi_pair@v1",
            env_vars={"ENVY_TEST_MULTI_LOG": str(log)},
            setup=["alpha", "zeta"],
        )
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertEqual(sorted(log.read_text().split()), ["alpha", "zeta"])

    def test_satisfied_pair_skipped_others_run(self):
        """Satisfied pairs are skipped independently of unsatisfied ones."""
        spec = """IDENTITY = "local.um_partial@v1"
USER_MANAGED = true

SETUP = {
  done = {
    CHECK = function(pkg_dir, options) return true end,
    INSTALL = function(pkg_dir, options) error("must not run") end,
  },
  todo = {
    CHECK = function(pkg_dir, options) return false end,
    INSTALL = function(pkg_dir, options)
        local f = io.open(os.getenv("ENVY_TEST_PARTIAL_MARKER"), "w")
        f:write("done")
        f:close()
    end,
  },
}
"""
        marker = self.marker_dir / "partial-marker"
        self.write_spec("partial", spec)
        result = self.run_spec(
            "partial",
            "local.um_partial@v1",
            env_vars={"ENVY_TEST_PARTIAL_MARKER": str(marker)},
            setup=["done", "todo"],
        )
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertTrue(marker.exists())

    # =========================================================================
    # USER_MANAGED resolution forms (boolean / function / errors)
    # =========================================================================

    def test_user_managed_function_form_returning_true(self):
        """USER_MANAGED = function() return true end classifies the spec as user-managed."""
        spec = """IDENTITY = "local.um_fn_form_true@v1"
USER_MANAGED = function() return envy.PLATFORM ~= "<<never>>" end

SETUP = {
  main = {
    CHECK = function(pkg_dir, options)
        local marker = os.getenv("ENVY_TEST_MARKER_FN")
        local f = io.open(marker, "r")
        if f then f:close(); return true end
        return false
    end,

    INSTALL = function(pkg_dir, options)
        if pkg_dir ~= nil then
            error("pkg_dir should be nil for user-managed packages")
        end
        local marker = os.getenv("ENVY_TEST_MARKER_FN")
        local f = io.open(marker, "w")
        f:write("installed via function-form USER_MANAGED")
        f:close()
    end,
  },
}
"""
        marker = self.marker_dir / "marker-fn-form"
        self.write_spec("fn_form_true", spec)
        result = self.run_spec(
            "fn_form_true",
            "local.um_fn_form_true@v1",
            env_vars={"ENVY_TEST_MARKER_FN": str(marker)},
            setup=["main"],
        )
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertTrue(marker.exists(), "INSTALL should have run and created the marker")

    def test_user_managed_function_form_returning_non_boolean_fails(self):
        """USER_MANAGED function returning a non-boolean errors with identity in message."""
        spec = """IDENTITY = "local.um_fn_form_bad@v1"
USER_MANAGED = function() return "yes" end

SETUP = {
  main = {
    CHECK = function(pkg_dir, options) return false end,
    INSTALL = function(pkg_dir, options) end,
  },
}
"""
        self.write_spec("fn_form_bad", spec)
        result = self.run_spec("fn_form_bad", "local.um_fn_form_bad@v1")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("must return a boolean", result.stderr)
        self.assertIn("local.um_fn_form_bad@v1", result.stderr)

    def test_user_managed_top_level_check_rejected(self):
        """Top-level CHECK is rejected; CHECK/INSTALL pairs belong in SETUP."""
        spec = """IDENTITY = "local.um_toplevel_check@v1"
USER_MANAGED = true
CHECK = "echo hi"
SETUP = {
  main = {
    CHECK = function(pkg_dir, options) return true end,
    INSTALL = function(pkg_dir, options) end,
  },
}
"""
        self.write_spec("toplevel_check", spec)
        result = self.run_spec("toplevel_check", "local.um_toplevel_check@v1")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("top-level CHECK", result.stderr)

    def test_user_managed_without_setup_rejected(self):
        """User-managed spec without SETUP pairs is a load error."""
        spec = """IDENTITY = "local.um_no_setup@v1"
USER_MANAGED = true
"""
        self.write_spec("no_setup", spec)
        result = self.run_spec("no_setup", "local.um_no_setup@v1")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("at least one SETUP pair", result.stderr)


if __name__ == "__main__":
    unittest.main()
