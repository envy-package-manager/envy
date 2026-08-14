"""Functional tests for SETUP pair check/install runtime behavior.

Tests comprehensive functionality of SETUP pair CHECK and INSTALL verbs plus
cache-managed INSTALL, including:
- Check string silent success behavior
- envy.run with quiet/capture options inside pair CHECK
- Install cwd behavior for cache-managed packages vs SETUP pairs
- Default shell configuration
- Table field access patterns
- Shell error types
- Ephemeral pair workspace lifecycle
"""

import os
import subprocess
import sys
from pathlib import Path
import unittest

from . import test_config
from .env import EnvyTestCase


def um_spec(identity: str, check_body: str, install_body: str = "") -> str:
    """Build a user-managed spec with one SETUP pair named `main`."""
    return f"""
IDENTITY = "{identity}"
USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
{check_body}
    end,
    INSTALL = function(pkg_dir, options)
{install_body}
    end,
  }},
}}
"""


class TestCheckInstallRuntime(EnvyTestCase):
    """Tests for SETUP pair check/install runtime behavior."""

    def setUp(self):
        super().setUp()
        self.test_dir = self.make_temp_dir("test_dir")
        self.specs_dir = self.make_temp_dir("specs_dir")
        self.trace_flag = ["--trace"] if os.environ.get("ENVY_TEST_TRACE") else []

    def run_spec(
        self,
        spec_name,
        spec_content,
        identity,
        should_fail=False,
        env_vars=None,
        verbose=False,
        setup=None,
    ):
        """Run a spec with given content and identity, return subprocess result.

        setup=None selects no SETUP pairs; a list of pair names selects those.
        Either way the spec runs via a generated manifest and `install`.
        """
        spec_path = self.specs_dir / f"{spec_name}.lua"
        spec_path.write_text(spec_content, encoding="utf-8")

        cmd = [str(self.envy), f"--cache-root={self.cache_root}"]
        cmd.extend(self.trace_flag)
        if verbose:
            cmd.append("--verbose")
        entry = (identity, spec_path) if setup is None else (identity, spec_path, setup)
        manifest = test_config.write_spec_manifest(self.test_dir, [entry])
        cmd.extend(["install", "--manifest", str(manifest)])

        env = os.environ.copy()
        if env_vars:
            env.update(env_vars)

        result = test_config.run(
            cmd, capture_output=True, text=True, env=env, cwd=self.test_dir
        )

        if should_fail:
            self.assertNotEqual(
                result.returncode,
                0,
                f"Expected failure but succeeded: {result.stderr}",
            )
        else:
            self.assertEqual(
                result.returncode,
                0,
                f"stderr: {result.stderr}",
            )

        return result

    # ===== Check String Success Behavior =====

    def test_check_string_success_silent(self):
        """Pair CHECK string success produces no TUI output."""
        spec = """
IDENTITY = "local.check_string_success@v1"
USER_MANAGED = true
SETUP = {
  main = {
    CHECK = "echo 'test'",
    INSTALL = function(pkg_dir, options) end,
  },
}
"""
        result = self.run_spec(
            "check_string_silent", spec, "local.check_string_success@v1"
        )
        self.assertNotIn("python", result.stderr.lower())

    def test_check_string_success_silent_with_verbose(self):
        """Pair CHECK string success produces no TUI output even with --verbose."""
        spec = """
IDENTITY = "local.check_string_success@v1"
USER_MANAGED = true
SETUP = {
  main = {
    CHECK = "echo 'test'",
    INSTALL = function(pkg_dir, options) end,
  },
}
"""
        result = self.run_spec(
            "check_string_silent", spec, "local.check_string_success@v1", verbose=True
        )
        self.assertNotIn("python", result.stderr.lower())

    # ===== envy.run Quiet/Capture Behavior =====

    def test_ctx_run_quiet_success(self):
        """envy.run quiet=true success returns exit_code, no TUI output."""
        spec = um_spec(
            "local.check_ctx_run_quiet@v1",
            """
    local res = envy.run("echo 'test output'", {quiet = true})
    assert(res.exit_code ~= nil, "exit_code field should exist")
    assert(res.exit_code == 0, "exit_code should be 0")
    return true
""",
        )
        self.run_spec("ctx_run_quiet_success", spec, "local.check_ctx_run_quiet@v1")

    def test_ctx_run_quiet_failure(self):
        """envy.run quiet=true failure throws with no TUI output."""
        spec = um_spec(
            "local.check_ctx_run_quiet_fail@v1",
            """
    local res = envy.run("exit 1", {quiet = true})
    error("Should have thrown on non-zero exit")
""",
        )
        result = self.run_spec(
            "ctx_run_quiet_failure",
            spec,
            "local.check_ctx_run_quiet_fail@v1",
            should_fail=True,
            setup=["main"],
        )
        self.assertIn("error", result.stderr.lower())

    def test_ctx_run_capture(self):
        """envy.run capture=true returns table with stdout, stderr, exit_code."""
        spec = um_spec(
            "local.check_ctx_run_capture@v1",
            """
    local cmd = envy.PLATFORM == "windows"
        and "Write-Output 'stdout text'; [Console]::Error.WriteLine('stderr text')"
        or "echo 'stdout text' && echo 'stderr text' >&2"
    local res = envy.run(cmd, {capture = true})
    assert(res.stdout ~= nil, "stdout field should exist")
    assert(res.stderr ~= nil, "stderr field should exist")
    assert(res.exit_code ~= nil, "exit_code field should exist")
    assert(res.stdout:match("stdout text"), "stdout should contain expected text")
    assert(res.exit_code == 0, "exit_code should be 0")
    return true
""",
        )
        self.run_spec("ctx_run_capture", spec, "local.check_ctx_run_capture@v1")

    def test_ctx_run_no_capture(self):
        """envy.run capture=false returns table with only exit_code field."""
        spec = um_spec(
            "local.check_ctx_run_no_capture@v1",
            """
    local res = envy.run("echo 'test'", {capture = false})
    assert(res.exit_code ~= nil, "exit_code field should exist")
    assert(res.stdout == nil, "stdout field should not exist when capture=false")
    assert(res.stderr == nil, "stderr field should not exist when capture=false")
    assert(res.exit_code == 0, "exit_code should be 0")
    return true
""",
        )
        self.run_spec("ctx_run_no_capture", spec, "local.check_ctx_run_no_capture@v1")

    def test_ctx_run_default(self):
        """envy.run default (no flags) streams, throws on non-zero, returns exit_code."""
        spec = um_spec(
            "local.check_ctx_run_default@v1",
            """
    local res = envy.run("echo 'default test'")
    assert(res.exit_code ~= nil, "exit_code field should exist")
    assert(res.exit_code == 0, "exit_code should be 0")
    assert(res.stdout == nil, "stdout not captured by default")
    assert(res.stderr == nil, "stderr not captured by default")
    return true
""",
        )
        self.run_spec("ctx_run_default", spec, "local.check_ctx_run_default@v1")

    def test_ctx_run_quiet_capture_combinations(self):
        """Test all 4 combinations of quiet/capture flags."""
        spec_neither = um_spec(
            "local.check_combo_neither@v1",
            """
    local res = envy.run("echo 'combo test'")
    assert(res.exit_code == 0)
    assert(res.stdout == nil, "stdout not captured")
    assert(res.stderr == nil, "stderr not captured")
    return true
""",
        )
        spec_quiet = um_spec(
            "local.check_combo_quiet@v1",
            """
    local res = envy.run("echo 'quiet test'", {quiet = true})
    assert(res.exit_code == 0)
    assert(res.stdout == nil, "stdout not captured")
    assert(res.stderr == nil, "stderr not captured")
    return true
""",
        )
        spec_capture = um_spec(
            "local.check_combo_capture@v1",
            """
    local res = envy.run("echo 'capture test'", {capture = true})
    assert(res.exit_code == 0)
    assert(res.stdout ~= nil, "stdout should be captured")
    assert(res.stderr ~= nil, "stderr should be captured")
    assert(res.stdout:match("capture test"))
    return true
""",
        )
        spec_both = um_spec(
            "local.check_combo_both@v1",
            """
    local res = envy.run("echo 'both test'", {quiet = true, capture = true})
    assert(res.exit_code == 0)
    assert(res.stdout ~= nil, "stdout should be captured")
    assert(res.stderr ~= nil, "stderr should be captured")
    assert(res.stdout:match("both test"))
    return true
""",
        )
        test_cases = [
            ("ctx_run_combo_neither", spec_neither, "local.check_combo_neither@v1"),
            ("ctx_run_combo_quiet", spec_quiet, "local.check_combo_quiet@v1"),
            ("ctx_run_combo_capture", spec_capture, "local.check_combo_capture@v1"),
            ("ctx_run_combo_both", spec_both, "local.check_combo_both@v1"),
        ]
        for spec_name, spec_content, identity in test_cases:
            with self.subTest(spec=spec_name):
                self.run_spec(spec_name, spec_content, identity)

    # ===== CWD and Shell Configuration =====

    def test_check_cwd_manifest_directory(self):
        """Pair CHECK cwd = manifest directory."""
        spec = um_spec(
            "local.check_cwd_manifest@v1",
            """
    if envy.PLATFORM == "windows" then
        envy.run('"cwd_test" | Out-File -FilePath cwd_check_marker.txt')
    else
        envy.run("echo 'cwd_test' > cwd_check_marker.txt")
    end
    local test_cmd = envy.PLATFORM == "windows"
        and 'if (Test-Path cwd_check_marker.txt) { exit 0 } else { exit 1 }'
        or "test -f cwd_check_marker.txt"
    local res = envy.run(test_cmd, {quiet = true})
    if envy.PLATFORM == "windows" then
        envy.run('Remove-Item -Force -ErrorAction SilentlyContinue cwd_check_marker.txt', {quiet = true})
    else
        envy.run("rm -f cwd_check_marker.txt", {quiet = true})
    end
    if res.exit_code ~= 0 then error("Could not create marker file relative to cwd") end
    return true
""",
        )
        self.run_spec("check_cwd_manifest", spec, "local.check_cwd_manifest@v1")

    def test_install_cwd_cache_managed(self):
        """Install cwd = stage_dir for cache-managed packages."""
        spec = """
IDENTITY = "local.install_cwd_cache@v1"
function FETCH(tmp_dir, options) end
function INSTALL(install_dir, stage_dir, fetch_dir, tmp_dir, options)
    if envy.PLATFORM == "windows" then
        envy.run('"test" | Out-File -FilePath cwd_marker.txt')
    else
        envy.run("echo 'test' > cwd_marker.txt")
    end
    local test_cmd = envy.PLATFORM == "windows"
        and 'if (Test-Path cwd_marker.txt) { exit 0 } else { exit 1 }'
        or "test -f cwd_marker.txt"
    local res = envy.run(test_cmd, {quiet = true})
    if res.exit_code ~= 0 then error("Marker file not accessible via relative path - cwd issue") end
    local marker_path = stage_dir .. (envy.PLATFORM == "windows" and "\\\\cwd_marker.txt" or "/cwd_marker.txt")
    local test_cmd2 = envy.PLATFORM == "windows"
        and ('if (Test-Path \\'' .. marker_path .. '\\') { exit 0 } else { exit 1 }')
        or ("test -f '" .. marker_path .. "'")
    local res2 = envy.run(test_cmd2, {quiet = true})
    if res2.exit_code ~= 0 then error("Marker file not in stage_dir - cwd was not stage_dir") end
end
"""
        self.run_spec("install_cwd_cache", spec, "local.install_cwd_cache@v1")

    def test_install_cwd_pair(self):
        """Pair INSTALL cwd = manifest directory."""
        spec = um_spec(
            "local.install_cwd_user@v1",
            """
    local marker = os.getenv("ENVY_TEST_INSTALL_MARKER")
    if not marker then error("ENVY_TEST_INSTALL_MARKER not set") end
    local test_cmd = envy.PLATFORM == "windows"
        and ('if (Test-Path \\'' .. marker .. '\\') { exit 0 } else { exit 1 }')
        or ("test -f '" .. marker .. "'")
    local success, res = pcall(function() return envy.run(test_cmd, {quiet = true}) end)
    return success and res.exit_code == 0
""",
            """
    local marker = os.getenv("ENVY_TEST_INSTALL_MARKER")
    if not marker then error("ENVY_TEST_INSTALL_MARKER not set") end
    if envy.PLATFORM == "windows" then
        envy.run('"user_managed_cwd_test" | Out-File -FilePath user_install_cwd_marker.txt')
    else
        envy.run("echo 'user_managed_cwd_test' > user_install_cwd_marker.txt")
    end
    local test_cmd = envy.PLATFORM == "windows"
        and 'if (Test-Path user_install_cwd_marker.txt) { exit 0 } else { exit 1 }'
        or "test -f user_install_cwd_marker.txt"
    local res = envy.run(test_cmd, {quiet = true})
    if envy.PLATFORM == "windows" then
        envy.run('Remove-Item -Force -ErrorAction SilentlyContinue user_install_cwd_marker.txt', {quiet = true})
    else
        envy.run("rm -f user_install_cwd_marker.txt", {quiet = true})
    end
    if res.exit_code ~= 0 then error("Could not create file with relative path - cwd issue") end
    if envy.PLATFORM == "windows" then
        envy.run('New-Item -ItemType File -Force -Path \\'' .. marker .. '\\' | Out-Null')
    else
        envy.run("touch '" .. marker .. "'")
    end
""",
        )
        marker_file = self.test_dir / "install_marker"
        self.run_spec(
            "install_cwd_user",
            spec,
            "local.install_cwd_user@v1",
            env_vars={"ENVY_TEST_INSTALL_MARKER": str(marker_file)},
        )

    def test_default_shell_in_check(self):
        """Manifest default_shell is respected in pair CHECK string."""
        spec = """
IDENTITY = "local.check_default_shell@v1"
USER_MANAGED = true
SETUP = {
  main = {
    CHECK = "echo 'shell test'",
    INSTALL = function(pkg_dir, options) end,
  },
}
"""
        self.run_spec("check_shell", spec, "local.check_default_shell@v1")

    def test_default_shell_in_install(self):
        """Manifest default_shell is respected in install string."""
        spec = """
IDENTITY = "local.install_default_shell@v1"
function FETCH(tmp_dir, options) end
INSTALL = "echo 'install shell test' > output.txt"
"""
        self.run_spec("install_shell", spec, "local.install_default_shell@v1")

    def test_pair_install_string_form(self):
        """Pair INSTALL string form runs via shell with cwd = manifest directory."""
        spec = """
IDENTITY = "local.pair_install_string@v1"
USER_MANAGED = true
SETUP = {
  main = {
    CHECK = "test -f pair_install_string_marker.txt",
    INSTALL = "echo 'made by pair install' > pair_install_string_marker.txt",
  },
}
"""
        self.run_spec(
            "pair_install_string", spec, "local.pair_install_string@v1", setup=["main"]
        )
        marker = self.test_dir / "pair_install_string_marker.txt"
        self.assertTrue(marker.exists(), "string INSTALL should write in project root")

    # ===== Table Field Access Patterns =====

    def test_table_field_direct_access(self):
        """Direct table field access: res.exit_code, res.stdout."""
        spec = um_spec(
            "local.check_table_fields@v1",
            """
    local res = envy.run("echo 'test output'", {capture = true})
    local code = res.exit_code
    local out = res.stdout
    local err = res.stderr
    assert(code == 0, "exit_code should be 0")
    assert(out ~= nil, "stdout should exist")
    assert(err ~= nil, "stderr should exist")
    assert(out:match("test output"), "stdout should contain expected text")
    return true
""",
        )
        self.run_spec("table_field_access", spec, "local.check_table_fields@v1")

    def test_table_field_chained_access(self):
        """Chained table field access: envy.run(...).stdout."""
        spec = um_spec(
            "local.check_table_chained@v1",
            """
    local out = envy.run("echo 'chained'", {capture = true}).stdout
    assert(out:match("chained"), "chained stdout access should work")
    local code = envy.run("echo 'test'", {quiet = true}).exit_code
    assert(code == 0, "chained exit_code access should work")
    return true
""",
        )
        self.run_spec("table_chained_access", spec, "local.check_table_chained@v1")

    # ===== Shell Error Types =====

    def test_shell_error_command_not_found(self):
        """Shell error: command not found."""
        spec = um_spec(
            "local.check_error_not_found@v1",
            """
    local res = envy.run("nonexistent_command_12345", {quiet = true, check = true})
    error("Should have thrown on command not found")
""",
        )
        result = self.run_spec(
            "error_cmd_not_found",
            spec,
            "local.check_error_not_found@v1",
            should_fail=True,
            setup=["main"],
        )
        stderr_lower = result.stderr.lower()
        if sys.platform == "win32":
            self.assertIn("exit code 1", stderr_lower)
        else:
            self.assertIn("exit code 127", stderr_lower)

    def test_shell_error_syntax(self):
        """Shell error: syntax error."""
        spec = um_spec(
            "local.check_error_syntax@v1",
            """
    local res = envy.run("echo 'unclosed quote", {quiet = true})
    error("Should have thrown on syntax error")
""",
        )
        result = self.run_spec(
            "error_syntax",
            spec,
            "local.check_error_syntax@v1",
            should_fail=True,
            setup=["main"],
        )
        self.assertIn("error", result.stderr.lower())

    # ===== Concurrent Output =====

    def test_concurrent_large_stdout_stderr(self):
        """Concurrent large stdout+stderr with capture (no pipe deadlock)."""
        spec = um_spec(
            "local.check_concurrent@v1",
            """
    local cmd
    if envy.PLATFORM == "windows" then
        cmd = [[for ($i=1; $i -le 1000; $i++) { Write-Output "stdout line $i"; [Console]::Error.WriteLine("stderr line $i") }]]
    else
        cmd = [[for i in $(seq 1 1000); do echo "stdout line $i"; echo "stderr line $i" >&2; done]]
    end
    local res = envy.run(cmd, {capture = true})
    assert(res.exit_code == 0, "command should succeed")
    assert(res.stdout:match("stdout line"), "should have stdout")
    assert(res.stderr:match("stderr line"), "should have stderr")
    return true
""",
        )
        self.run_spec("concurrent_output", spec, "local.check_concurrent@v1")

    # ===== Empty Output =====

    def test_empty_outputs_in_failure(self):
        """Empty outputs in failure messages are clarified."""
        spec = um_spec(
            "local.check_empty_output@v1",
            """
    local res = envy.run("exit 42", {quiet = true})
    error("Should have thrown on non-zero exit")
""",
        )
        result = self.run_spec(
            "empty_output_failure",
            spec,
            "local.check_empty_output@v1",
            should_fail=True,
            setup=["main"],
        )
        self.assertIn("error", result.stderr.lower())

    # ===== Ephemeral Pair Workspace Lifecycle =====

    def test_pair_entry_dir_deleted(self):
        """Pair install: ephemeral cache entry deleted after completion."""
        spec = um_spec(
            "local.user_cleanup@v1",
            """
    local marker = os.getenv("ENVY_TEST_CLEANUP_MARKER")
    if not marker then error("ENVY_TEST_CLEANUP_MARKER not set") end
    local success, res = pcall(function()
        return envy.run("test -f '" .. marker .. "'", {quiet = true})
    end)
    return success and res.exit_code == 0
""",
            """
    local marker = os.getenv("ENVY_TEST_CLEANUP_MARKER")
    if not marker then error("ENVY_TEST_CLEANUP_MARKER not set") end
    envy.run("touch '" .. marker .. "'")
""",
        )
        marker_file = self.test_dir / "cleanup_marker"
        self.run_spec(
            "user_cleanup",
            spec,
            "local.user_cleanup@v1",
            env_vars={"ENVY_TEST_CLEANUP_MARKER": str(marker_file)},
        )
        pkg_dir = self.cache_root / "packages" / "local.user_cleanup@v1"
        if pkg_dir.exists():
            pkg_subdirs = list(pkg_dir.glob("*/pkg"))
            self.assertEqual(
                len(pkg_subdirs),
                0,
                "User-managed packages should not leave pkg/ in cache",
            )


if __name__ == "__main__":
    unittest.main()
