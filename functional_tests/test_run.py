"""Functional tests for `envy run` command."""

import os
import shutil
import stat
import sys
import tempfile
import unittest
from pathlib import Path

from . import test_config
from .env import EnvyTestCase


class _RunTestBase(EnvyTestCase):
    """Shared setUp/tearDown and helpers for envy run tests."""

    def setUp(self) -> None:
        self._temp_dir = self.make_temp_dir("_temp_dir").resolve()
        self._envy = test_config.get_envy_executable()

        self._project = self._temp_dir / "project"
        self._project.mkdir()
        self._bin_dir = self._project / "tools"
        self._bin_dir.mkdir()

        (self._project / "envy.lua").write_text(
            '-- @envy bin "tools"\nPACKAGES = {}\n'
        )

        self._outside = self._temp_dir / "outside"
        self._outside.mkdir()

    def tearDown(self) -> None:
        if hasattr(self, "_temp_dir") and self._temp_dir.exists():
            shutil.rmtree(self._temp_dir, ignore_errors=True)

    def _run_envy(self, args: list[str], **kwargs):
        return test_config.run(
            [str(self._envy)] + args,
            capture_output=True,
            text=True,
            timeout=10,
            **kwargs,
        )


class TestRunErrors(_RunTestBase):
    """Platform-independent error-case tests."""

    def test_no_command_errors(self) -> None:
        result = self._run_envy(["run"], cwd=self._project)
        self.assertNotEqual(0, result.returncode)

    def test_no_manifest_errors(self) -> None:
        result = self._run_envy(["run", "echo", "hi"], cwd=self._outside)
        self.assertNotEqual(0, result.returncode)

    def test_missing_bin_dir_errors(self) -> None:
        broken = self._temp_dir / "broken"
        broken.mkdir()
        (broken / "envy.lua").write_text(
            '-- @envy bin "nonexistent"\nPACKAGES = {}\n'
        )
        result = self._run_envy(["run", "echo", "hi"], cwd=broken)
        self.assertNotEqual(0, result.returncode)

    def test_no_bin_directive_errors(self) -> None:
        nobin = self._temp_dir / "nobin"
        nobin.mkdir()
        (nobin / "envy.lua").write_text('PACKAGES = {}\n')
        result = self._run_envy(["run", "echo", "hi"], cwd=nobin)
        self.assertNotEqual(0, result.returncode)


@unittest.skipIf(sys.platform == "win32", "POSIX shell tests")
class TestRunPosix(_RunTestBase):
    """POSIX-specific tests using sh and shell scripts."""

    def _write_script(self, name: str, body: str) -> Path:
        script = self._bin_dir / name
        script.write_text(f"#!/bin/sh\n{body}\n")
        script.chmod(script.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
        return script

    def test_bin_dir_on_path(self) -> None:
        result = self._run_envy(["run", "sh", "-c", "echo $PATH"], cwd=self._project)
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        entries = result.stdout.strip().split(":")
        self.assertEqual(str(self._bin_dir), entries[0])

    def test_project_root_set(self) -> None:
        result = self._run_envy(
            ["run", "sh", "-c", "echo $ENVY_PROJECT_ROOT"], cwd=self._project
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(str(self._project), result.stdout.strip())

    def test_exit_code_forwarded(self) -> None:
        result = self._run_envy(["run", "sh", "-c", "exit 42"], cwd=self._project)
        self.assertEqual(42, result.returncode)

    def test_run_product_script_from_project_root(self) -> None:
        self._write_script("my-tool", 'echo "tool-output:$ENVY_PROJECT_ROOT"')
        result = self._run_envy(["run", "my-tool"], cwd=self._project)
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(f"tool-output:{self._project}", result.stdout.strip())

    def test_run_product_script_from_subdirectory(self) -> None:
        self._write_script("greet", 'echo "hello from $ENVY_PROJECT_ROOT"')
        sub = self._project / "src" / "deep"
        sub.mkdir(parents=True)
        result = self._run_envy(["run", "greet"], cwd=sub)
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(f"hello from {self._project}", result.stdout.strip())

    def test_run_product_script_not_on_system_path(self) -> None:
        unique_name = "envy-test-unique-tool-xyz"
        self._write_script(unique_name, 'echo "found-me"')
        self.assertIsNone(shutil.which(unique_name))
        result = self._run_envy(["run", unique_name], cwd=self._project)
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual("found-me", result.stdout.strip())


@unittest.skipUnless(sys.platform == "win32", "Windows-specific tests")
class TestRunWindows(_RunTestBase):
    """Windows-specific tests using cmd.exe and .bat files."""

    def _write_bat(self, name: str, body: str) -> Path:
        bat = self._bin_dir / f"{name}.bat"
        bat.write_text(f"@echo off\r\n{body}\r\n")
        return bat

    def test_bin_dir_on_path(self) -> None:
        result = self._run_envy(["run", "cmd", "/c", "echo %PATH%"], cwd=self._project)
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        entries = result.stdout.strip().split(";")
        self.assertEqual(str(self._bin_dir), entries[0])

    def test_project_root_set(self) -> None:
        result = self._run_envy(
            ["run", "cmd", "/c", "echo %ENVY_PROJECT_ROOT%"], cwd=self._project
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(str(self._project), result.stdout.strip())

    def test_exit_code_forwarded(self) -> None:
        result = self._run_envy(["run", "cmd", "/c", "exit /b 42"], cwd=self._project)
        self.assertEqual(42, result.returncode)

    def test_run_bat_from_project_root(self) -> None:
        self._write_bat("my-tool", "echo tool-output:%ENVY_PROJECT_ROOT%")
        result = self._run_envy(["run", "my-tool"], cwd=self._project)
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(f"tool-output:{self._project}", result.stdout.strip())

    def test_run_bat_from_subdirectory(self) -> None:
        self._write_bat("greet", "echo hello from %ENVY_PROJECT_ROOT%")
        sub = self._project / "src" / "deep"
        sub.mkdir(parents=True)
        result = self._run_envy(["run", "greet"], cwd=sub)
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(f"hello from {self._project}", result.stdout.strip())

    def test_run_bat_not_on_system_path(self) -> None:
        unique_name = "envy-test-unique-tool-xyz"
        self._write_bat(unique_name, "echo found-me")
        self.assertIsNone(shutil.which(unique_name))
        result = self._run_envy(["run", unique_name], cwd=self._project)
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual("found-me", result.stdout.strip())


@unittest.skipIf(sys.platform == "win32", "POSIX sentinel tests")
class TestRunSentinelPosix(_RunTestBase):
    """Tests for `--` sentinel manifest discovery from script location."""

    def _make_script(self, path: Path, body: str) -> Path:
        path.write_text(f"#!/bin/sh\n{body}\n")
        path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
        return path

    def test_sentinel_discovers_manifest_from_script_dir(self) -> None:
        script = self._make_script(
            self._project / "helper.sh",
            'echo "root=$ENVY_PROJECT_ROOT"',
        )
        result = self._run_envy(["run", "sh", "--", str(script)], cwd=self._outside)
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(f"root={self._project}", result.stdout.strip())

    def test_sentinel_sets_project_root(self) -> None:
        script = self._make_script(
            self._project / "check-root.sh",
            'echo "$ENVY_PROJECT_ROOT"',
        )
        result = self._run_envy(["run", "sh", "--", str(script)], cwd=self._outside)
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(str(self._project), result.stdout.strip())

    def test_sentinel_missing_script_errors(self) -> None:
        result = self._run_envy(["run", "sh", "--"], cwd=self._project)
        self.assertNotEqual(0, result.returncode)

    def test_sentinel_nonexistent_script_errors(self) -> None:
        result = self._run_envy(
            ["run", "sh", "--", "/nonexistent/script.sh"], cwd=self._project
        )
        self.assertNotEqual(0, result.returncode)
        self.assertIn("script not found", result.stderr)


class _DeepBinTestBase(_RunTestBase):
    """A project whose bin dir sits four levels under the manifest."""

    DEEP = "a/b/c/bin"

    def setUp(self) -> None:
        super().setUp()
        self._deep = self._temp_dir / "deep"
        self._deep_bin = self._deep / "a" / "b" / "c" / "bin"
        self._deep_bin.mkdir(parents=True)
        (self._deep / "envy.lua").write_text(
            f'-- @envy bin "{self.DEEP}"\nPACKAGES = {{}}\n'
        )


@unittest.skipIf(sys.platform == "win32", "POSIX shell tests")
class TestRunDeepBinDirPosix(_DeepBinTestBase):
    """`envy run` joins '@envy bin' onto the manifest dir whatever its depth."""

    def test_deep_bin_dir_on_path(self) -> None:
        result = self._run_envy(["run", "sh", "-c", "echo $PATH"], cwd=self._deep)
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        entries = result.stdout.strip().split(":")
        self.assertEqual(str(self._deep_bin), entries[0])

    def test_project_root_is_the_manifest_dir_not_an_intermediate(self) -> None:
        result = self._run_envy(
            ["run", "sh", "-c", "echo $ENVY_PROJECT_ROOT"], cwd=self._deep
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(str(self._deep), result.stdout.strip())

    def test_a_tool_in_the_deep_bin_dir_is_found(self) -> None:
        script = self._deep_bin / "envy-test-deep-tool-xyz"
        script.write_text('#!/bin/sh\necho "found-deep:$ENVY_PROJECT_ROOT"\n')
        script.chmod(script.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
        self.assertIsNone(shutil.which(script.name))
        result = self._run_envy(["run", script.name], cwd=self._deep)
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(f"found-deep:{self._deep}", result.stdout.strip())

    def test_a_cwd_inside_the_deep_bin_dir_still_resolves_the_owner(self) -> None:
        """The upward walk passes three intermediates before reaching the manifest."""
        result = self._run_envy(
            ["run", "sh", "-c", "echo $ENVY_PROJECT_ROOT"], cwd=self._deep_bin
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(str(self._deep), result.stdout.strip())


@unittest.skipUnless(sys.platform == "win32", "Windows-specific tests")
class TestRunDeepBinDirWindows(_DeepBinTestBase):
    """The same, through cmd.exe: '/' in the directive joins onto a '\\' path."""

    def test_deep_bin_dir_on_path(self) -> None:
        result = self._run_envy(["run", "cmd", "/c", "echo %PATH%"], cwd=self._deep)
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        entries = result.stdout.strip().split(";")
        self.assertEqual(str(self._deep_bin), entries[0])

    def test_project_root_is_the_manifest_dir_not_an_intermediate(self) -> None:
        result = self._run_envy(
            ["run", "cmd", "/c", "echo %ENVY_PROJECT_ROOT%"], cwd=self._deep
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(str(self._deep), result.stdout.strip())

    def test_a_tool_in_the_deep_bin_dir_is_found(self) -> None:
        bat = self._deep_bin / "envy-test-deep-tool-xyz.bat"
        bat.write_text("@echo off\r\necho found-deep:%ENVY_PROJECT_ROOT%\r\n")
        self.assertIsNone(shutil.which(bat.stem))
        result = self._run_envy(["run", bat.stem], cwd=self._deep)
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(f"found-deep:{self._deep}", result.stdout.strip())

    def test_a_cwd_inside_the_deep_bin_dir_still_resolves_the_owner(self) -> None:
        result = self._run_envy(
            ["run", "cmd", "/c", "echo %ENVY_PROJECT_ROOT%"], cwd=self._deep_bin
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(str(self._deep), result.stdout.strip())


if __name__ == "__main__":
    unittest.main()
