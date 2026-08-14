"""Functional tests for error output visibility.

Verifies that when shell commands fail during BUILD, STAGE, INSTALL, or
envy.run(), the full stdout and stderr from the failed command are printed
to the process output (via tui::error) — not silently swallowed by the
3-line-limited progress tracker.
"""

import hashlib
import io
import os
import sys
import tarfile
from pathlib import Path
import unittest

from . import test_config
from .env import EnvyTestCase

# Minimal archive so FETCH/STAGE have something to work with
TEST_ARCHIVE_FILES = {
    "root/file1.txt": "content\n",
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


def _posix_fail_script(stdout_markers, stderr_markers):
    """Build a bash script that emits markers then exits non-zero."""
    lines = []
    for m in stdout_markers:
        lines.append(f'echo "{m}"')
    for m in stderr_markers:
        lines.append(f'echo "{m}" >&2')
    lines.append("exit 1")
    return "\n".join(lines)


def _windows_fail_script(stdout_markers, stderr_markers):
    """Build a PowerShell script that emits markers then exits non-zero."""
    lines = []
    for m in stdout_markers:
        lines.append(f'Write-Output "{m}"')
    for m in stderr_markers:
        lines.append(f'[Console]::Error.WriteLine("{m}")')
    lines.append("exit 1")
    return "\n".join(lines)


def _lua_fail_script(stdout_markers, stderr_markers, shell_clause=""):
    """Build a Lua string literal containing a failing shell script.

    Returns a Lua expression suitable for use as a string verb value
    or as a return value from a function verb.
    """
    if sys.platform == "win32":
        script = _windows_fail_script(stdout_markers, stderr_markers)
        if not shell_clause:
            shell_clause = "shell = ENVY_SHELL.POWERSHELL, "
    else:
        script = _posix_fail_script(stdout_markers, stderr_markers)
    return script, shell_clause


class TestErrorOutput(EnvyTestCase):
    """Verify full stdout/stderr from failed commands appears in process output."""

    def setUp(self):
        super().setUp()
        self.specs_dir = self.make_temp_dir("specs_dir")
        self.trace_flag = ["--trace"] if os.environ.get("ENVY_TEST_TRACE") else []

        self.archive_path = self.specs_dir / "test.tar.gz"
        self.archive_hash = create_test_archive(self.archive_path)

    def write_spec(self, name: str, content: str) -> str:
        spec_content = content.format(
            ARCHIVE_PATH=self.archive_path.as_posix(),
            ARCHIVE_HASH=self.archive_hash,
        )
        path = self.specs_dir / f"{name}.lua"
        path.write_text(spec_content, encoding="utf-8")
        return str(path)

    def run_spec_fail(self, name: str, identity: str):
        """Install the named spec, assert it fails, return combined output."""
        manifest = test_config.write_spec_manifest(
            self.specs_dir, [(identity, self.specs_dir / f"{name}.lua")]
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
            result.returncode,
            0,
            f"Spec {name} should have failed but succeeded.\n"
            f"stderr: {result.stderr}",
        )

        return result.stdout + result.stderr

    def assert_markers_visible(self, output, markers, phase):
        """Assert every marker string appears in the combined output."""
        for marker in markers:
            self.assertIn(
                marker,
                output,
                f"{phase}: marker '{marker}' not found in error output.\n"
                f"Full output:\n{output}",
            )

    # =========================================================================
    # BUILD string
    # =========================================================================

    def test_build_string_error_output(self):
        """BUILD string: stdout+stderr from failed script appear in output."""
        stdout_markers = [
            "ENVY_BUILD_STR_OUT_LINE1",
            "ENVY_BUILD_STR_OUT_LINE2",
            "ENVY_BUILD_STR_OUT_LINE3",
            "ENVY_BUILD_STR_OUT_LINE4",
            "ENVY_BUILD_STR_OUT_LINE5",
        ]
        stderr_markers = [
            "ENVY_BUILD_STR_ERR_LINE1",
            "ENVY_BUILD_STR_ERR_LINE2",
        ]
        script, _ = _lua_fail_script(stdout_markers, stderr_markers)

        if sys.platform == "win32":
            build_expr = (
                f'if envy.PLATFORM == "windows" then\n  return [[{script}]]\nend'
            )
            spec = f"""IDENTITY = "local.build_str_errout@v1"

FETCH = {{{{
  source = "{{ARCHIVE_PATH}}",
  sha256 = "{{ARCHIVE_HASH}}"
}}}}

STAGE = {{{{strip = 1}}}}

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  {build_expr}
end
"""
        else:
            spec = f"""IDENTITY = "local.build_str_errout@v1"

FETCH = {{{{
  source = "{{ARCHIVE_PATH}}",
  sha256 = "{{ARCHIVE_HASH}}"
}}}}

STAGE = {{{{strip = 1}}}}

BUILD = [[
{script}
]]
"""
        self.write_spec("build_str_errout", spec)
        output = self.run_spec_fail("build_str_errout", "local.build_str_errout@v1")
        self.assert_markers_visible(output, stdout_markers, "BUILD string")
        self.assert_markers_visible(output, stderr_markers, "BUILD string")

    # =========================================================================
    # BUILD function returning string
    # =========================================================================

    def test_build_func_return_error_output(self):
        """BUILD function returning string: stdout+stderr appear in output."""
        stdout_markers = [
            "ENVY_BUILD_FN_OUT_LINE1",
            "ENVY_BUILD_FN_OUT_LINE2",
            "ENVY_BUILD_FN_OUT_LINE3",
            "ENVY_BUILD_FN_OUT_LINE4",
            "ENVY_BUILD_FN_OUT_LINE5",
        ]
        stderr_markers = [
            "ENVY_BUILD_FN_ERR_LINE1",
            "ENVY_BUILD_FN_ERR_LINE2",
        ]
        script, _ = _lua_fail_script(stdout_markers, stderr_markers)

        if sys.platform == "win32":
            return_line = (
                f'if envy.PLATFORM == "windows" then\n    return [[{script}]]\n  end'
            )
        else:
            return_line = f"return [[{script}]]"

        spec = f"""IDENTITY = "local.build_fn_errout@v1"

FETCH = {{{{
  source = "{{ARCHIVE_PATH}}",
  sha256 = "{{ARCHIVE_HASH}}"
}}}}

STAGE = {{{{strip = 1}}}}

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  {return_line}
end
"""
        self.write_spec("build_fn_errout", spec)
        output = self.run_spec_fail("build_fn_errout", "local.build_fn_errout@v1")
        self.assert_markers_visible(output, stdout_markers, "BUILD function")
        self.assert_markers_visible(output, stderr_markers, "BUILD function")

    # =========================================================================
    # STAGE string
    # =========================================================================

    def test_stage_string_error_output(self):
        """STAGE string: stdout+stderr from failed script appear in output."""
        stdout_markers = [
            "ENVY_STAGE_STR_OUT_LINE1",
            "ENVY_STAGE_STR_OUT_LINE2",
            "ENVY_STAGE_STR_OUT_LINE3",
            "ENVY_STAGE_STR_OUT_LINE4",
            "ENVY_STAGE_STR_OUT_LINE5",
        ]
        stderr_markers = [
            "ENVY_STAGE_STR_ERR_LINE1",
            "ENVY_STAGE_STR_ERR_LINE2",
        ]
        script, _ = _lua_fail_script(stdout_markers, stderr_markers)

        if sys.platform == "win32":
            stage_body = (
                f"function STAGE(fetch_dir, stage_dir, tmp_dir, options)\n"
                f"  envy.run([[{script}]], {{{{ shell = ENVY_SHELL.POWERSHELL, check = true }}}})\n"
                f"end"
            )
        else:
            spec = f"""IDENTITY = "local.stage_str_errout@v1"

FETCH = {{{{
  source = "{{ARCHIVE_PATH}}",
  sha256 = "{{ARCHIVE_HASH}}"
}}}}

STAGE = [[
{script}
]]
"""
            self.write_spec("stage_str_errout", spec)
            output = self.run_spec_fail("stage_str_errout", "local.stage_str_errout@v1")
            self.assert_markers_visible(output, stdout_markers, "STAGE string")
            self.assert_markers_visible(output, stderr_markers, "STAGE string")
            return

        # Windows: STAGE string uses default shell, use function + envy.run instead
        spec = f"""IDENTITY = "local.stage_str_errout@v1"

FETCH = {{{{
  source = "{{ARCHIVE_PATH}}",
  sha256 = "{{ARCHIVE_HASH}}"
}}}}

{stage_body}
"""
        self.write_spec("stage_str_errout", spec)
        output = self.run_spec_fail("stage_str_errout", "local.stage_str_errout@v1")
        self.assert_markers_visible(output, stdout_markers, "STAGE string")
        self.assert_markers_visible(output, stderr_markers, "STAGE string")

    # =========================================================================
    # INSTALL string
    # =========================================================================

    def test_install_string_error_output(self):
        """INSTALL string: stdout+stderr from failed script appear in output."""
        stdout_markers = [
            "ENVY_INSTALL_STR_OUT_LINE1",
            "ENVY_INSTALL_STR_OUT_LINE2",
            "ENVY_INSTALL_STR_OUT_LINE3",
            "ENVY_INSTALL_STR_OUT_LINE4",
            "ENVY_INSTALL_STR_OUT_LINE5",
        ]
        stderr_markers = [
            "ENVY_INSTALL_STR_ERR_LINE1",
            "ENVY_INSTALL_STR_ERR_LINE2",
        ]
        script, _ = _lua_fail_script(stdout_markers, stderr_markers)

        if sys.platform == "win32":
            install_body = (
                f"function INSTALL(install_dir, stage_dir, fetch_dir, tmp_dir, options)\n"
                f"  envy.run([[{script}]], {{{{ shell = ENVY_SHELL.POWERSHELL, check = true }}}})\n"
                f"end"
            )
        else:
            spec = f"""IDENTITY = "local.install_str_errout@v1"

FETCH = {{{{
  source = "{{ARCHIVE_PATH}}",
  sha256 = "{{ARCHIVE_HASH}}"
}}}}

STAGE = {{{{strip = 1}}}}

INSTALL = [[
{script}
]]
"""
            self.write_spec("install_str_errout", spec)
            output = self.run_spec_fail(
                "install_str_errout", "local.install_str_errout@v1"
            )
            self.assert_markers_visible(output, stdout_markers, "INSTALL string")
            self.assert_markers_visible(output, stderr_markers, "INSTALL string")
            return

        # Windows: INSTALL string uses default shell, use function + envy.run instead
        spec = f"""IDENTITY = "local.install_str_errout@v1"

FETCH = {{{{
  source = "{{ARCHIVE_PATH}}",
  sha256 = "{{ARCHIVE_HASH}}"
}}}}

STAGE = {{{{strip = 1}}}}

{install_body}
"""
        self.write_spec("install_str_errout", spec)
        output = self.run_spec_fail("install_str_errout", "local.install_str_errout@v1")
        self.assert_markers_visible(output, stdout_markers, "INSTALL string")
        self.assert_markers_visible(output, stderr_markers, "INSTALL string")

    # =========================================================================
    # INSTALL function returning string
    # =========================================================================

    def test_install_func_return_error_output(self):
        """INSTALL function returning string: stdout+stderr appear in output."""
        stdout_markers = [
            "ENVY_INSTALL_FN_OUT_LINE1",
            "ENVY_INSTALL_FN_OUT_LINE2",
            "ENVY_INSTALL_FN_OUT_LINE3",
            "ENVY_INSTALL_FN_OUT_LINE4",
            "ENVY_INSTALL_FN_OUT_LINE5",
        ]
        stderr_markers = [
            "ENVY_INSTALL_FN_ERR_LINE1",
            "ENVY_INSTALL_FN_ERR_LINE2",
        ]
        script, _ = _lua_fail_script(stdout_markers, stderr_markers)

        if sys.platform == "win32":
            return_line = (
                f'if envy.PLATFORM == "windows" then\n    return [[{script}]]\n  end'
            )
        else:
            return_line = f"return [[{script}]]"

        spec = f"""IDENTITY = "local.install_fn_errout@v1"

FETCH = {{{{
  source = "{{ARCHIVE_PATH}}",
  sha256 = "{{ARCHIVE_HASH}}"
}}}}

STAGE = {{{{strip = 1}}}}

INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  {return_line}
end
"""
        self.write_spec("install_fn_errout", spec)
        output = self.run_spec_fail("install_fn_errout", "local.install_fn_errout@v1")
        self.assert_markers_visible(output, stdout_markers, "INSTALL function")
        self.assert_markers_visible(output, stderr_markers, "INSTALL function")

    # =========================================================================
    # envy.run() with check=true
    # =========================================================================

    def test_envy_run_error_output(self):
        """envy.run() with check=true: stdout+stderr appear in output."""
        stdout_markers = [
            "ENVY_RUN_OUT_LINE1",
            "ENVY_RUN_OUT_LINE2",
            "ENVY_RUN_OUT_LINE3",
            "ENVY_RUN_OUT_LINE4",
            "ENVY_RUN_OUT_LINE5",
        ]
        stderr_markers = [
            "ENVY_RUN_ERR_LINE1",
            "ENVY_RUN_ERR_LINE2",
        ]
        script, shell_clause = _lua_fail_script(stdout_markers, stderr_markers)

        spec = f"""IDENTITY = "local.run_errout@v1"

FETCH = {{{{
  source = "{{ARCHIVE_PATH}}",
  sha256 = "{{ARCHIVE_HASH}}"
}}}}

STAGE = {{{{strip = 1}}}}

INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  envy.run([[
{script}
]], {{{{ {shell_clause}check = true }}}})
end
"""
        self.write_spec("run_errout", spec)
        output = self.run_spec_fail("run_errout", "local.run_errout@v1")
        self.assert_markers_visible(output, stdout_markers, "envy.run")
        self.assert_markers_visible(output, stderr_markers, "envy.run")

    # =========================================================================
    # FETCH function (envy.run inside programmatic fetch)
    # =========================================================================

    def test_fetch_func_envy_run_error_output(self):
        """FETCH function calling envy.run: stdout+stderr appear in output."""
        stdout_markers = [
            "ENVY_FETCH_FN_OUT_LINE1",
            "ENVY_FETCH_FN_OUT_LINE2",
            "ENVY_FETCH_FN_OUT_LINE3",
            "ENVY_FETCH_FN_OUT_LINE4",
            "ENVY_FETCH_FN_OUT_LINE5",
        ]
        stderr_markers = [
            "ENVY_FETCH_FN_ERR_LINE1",
            "ENVY_FETCH_FN_ERR_LINE2",
        ]
        script, shell_clause = _lua_fail_script(stdout_markers, stderr_markers)

        spec = f"""IDENTITY = "local.fetch_fn_errout@v1"

FETCH = function(tmp_dir, options)
  envy.run([[
{script}
]], {{{{ {shell_clause}check = true }}}})
end
"""
        self.write_spec("fetch_fn_errout", spec)
        output = self.run_spec_fail("fetch_fn_errout", "local.fetch_fn_errout@v1")
        self.assert_markers_visible(output, stdout_markers, "FETCH function")
        self.assert_markers_visible(output, stderr_markers, "FETCH function")
