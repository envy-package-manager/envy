"""Tests for envy shell command and shell hook deployment."""

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from . import test_config
from .env import EnvyTestCase


class TestShellCommand(EnvyTestCase):
    """Test the envy shell command."""

    def setUp(self) -> None:
        self._temp_dir = self.make_temp_dir("_temp_dir")
        self._cache_dir = self._temp_dir / "cache"
        self._project_dir = self._temp_dir / "project"
        self._bin_dir = self._temp_dir / "bin"
        self._envy = test_config.get_envy_production_executable()

    def tearDown(self) -> None:
        if hasattr(self, "_temp_dir") and self._temp_dir.exists():
            shutil.rmtree(self._temp_dir, ignore_errors=True)

    def _run_envy(self, *args) -> subprocess.CompletedProcess[str]:
        env = test_config.get_test_env()
        env["ENVY_CACHE_ROOT"] = str(self._cache_dir)
        cmd = [str(self._envy), *args]
        return test_config.run(cmd, capture_output=True, text=True, env=env, timeout=30)

    def _trigger_self_deploy(self) -> None:
        """Run init to trigger self-deploy (which writes hook files)."""
        result = self._run_envy("init", str(self._project_dir), str(self._bin_dir))
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")

    def test_shell_bash_prints_source_line(self) -> None:
        self._trigger_self_deploy()
        result = self._run_envy("shell", "bash")
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn("source", result.stderr)
        self.assertIn("hook.bash", result.stderr)
        self.assertIn("~/.bashrc", result.stderr)

    def test_shell_zsh_prints_source_line(self) -> None:
        self._trigger_self_deploy()
        result = self._run_envy("shell", "zsh")
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn("source", result.stderr)
        self.assertIn("hook.zsh", result.stderr)
        self.assertIn("~/.zshrc", result.stderr)

    def test_shell_fish_prints_source_line(self) -> None:
        self._trigger_self_deploy()
        result = self._run_envy("shell", "fish")
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn("source", result.stderr)
        self.assertIn("hook.fish", result.stderr)
        self.assertIn("config.fish", result.stderr)

    def test_shell_powershell_prints_source_line(self) -> None:
        self._trigger_self_deploy()
        result = self._run_envy("shell", "powershell")
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn("hook.ps1", result.stderr)
        self.assertIn("$PROFILE", result.stderr)

    def test_shell_invalid_shell_rejected(self) -> None:
        result = self._run_envy("shell", "csh")
        self.assertNotEqual(0, result.returncode)

    def test_shell_no_arg_rejected(self) -> None:
        result = self._run_envy("shell")
        self.assertNotEqual(0, result.returncode)

    def test_shell_warns_on_custom_cache(self) -> None:
        """When using a non-default cache, warn about cache dependency."""
        self._trigger_self_deploy()
        result = self._run_envy("shell", "bash")
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        # ENVY_CACHE_ROOT is set to a custom temp dir, so warning should appear
        self.assertIn("Moving or deleting", result.stderr)

    @unittest.skipIf(sys.platform == "win32", "HOME-based path test for Unix")
    def test_shell_bash_uses_dollar_home(self) -> None:
        """Bash source line should use $HOME, not ${env:HOME}."""
        self._trigger_self_deploy()
        result = self._run_envy("shell", "bash")
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertNotIn("${env:HOME}", result.stderr)

    @unittest.skipIf(sys.platform == "win32", "HOME-based path test for Unix")
    def test_shell_fish_uses_dollar_home(self) -> None:
        """Fish source line should use $HOME, not ${env:HOME}."""
        self._trigger_self_deploy()
        result = self._run_envy("shell", "fish")
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertNotIn("${env:HOME}", result.stderr)


class TestShellHookDeployment(EnvyTestCase):
    """Test that shell hook files are created during self-deploy."""

    def setUp(self) -> None:
        self._temp_dir = self.make_temp_dir("_temp_dir")
        self._cache_dir = self._temp_dir / "cache"
        self._project_dir = self._temp_dir / "project"
        self._bin_dir = self._temp_dir / "bin"
        self._envy = test_config.get_envy_production_executable()

    def tearDown(self) -> None:
        if hasattr(self, "_temp_dir") and self._temp_dir.exists():
            shutil.rmtree(self._temp_dir, ignore_errors=True)

    def _run_envy(self, *args) -> subprocess.CompletedProcess[str]:
        env = test_config.get_test_env()
        env["ENVY_CACHE_ROOT"] = str(self._cache_dir)
        cmd = [str(self._envy), *args]
        return test_config.run(cmd, capture_output=True, text=True, env=env, timeout=30)

    def _trigger_self_deploy(self) -> None:
        result = self._run_envy("init", str(self._project_dir), str(self._bin_dir))
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")

    def test_self_deploy_creates_all_hook_files(self) -> None:
        self._trigger_self_deploy()
        shell_dir = self._cache_dir / "shell"
        self.assertTrue(shell_dir.exists(), f"Shell dir not created at {shell_dir}")
        for ext in ("bash", "zsh", "fish", "ps1"):
            hook = shell_dir / f"hook.{ext}"
            self.assertTrue(hook.exists(), f"Hook not created at {hook}")

    def test_hook_files_contain_version_stamp(self) -> None:
        self._trigger_self_deploy()
        shell_dir = self._cache_dir / "shell"
        for ext in ("bash", "zsh", "fish", "ps1"):
            hook = shell_dir / f"hook.{ext}"
            content = hook.read_text(encoding="utf-8")
            self.assertIn("_ENVY_HOOK_VERSION", content)

    def test_hook_version_is_numeric_not_placeholder(self) -> None:
        """Verify the @@ENVY_HOOK_VERSION@@ placeholder was replaced."""
        import re

        self._trigger_self_deploy()
        shell_dir = self._cache_dir / "shell"
        versions: dict[str, int] = {}
        for ext in ("bash", "zsh", "fish", "ps1"):
            hook = shell_dir / f"hook.{ext}"
            content = hook.read_text(encoding="utf-8")
            self.assertNotIn(
                "@@ENVY_HOOK_VERSION@@",
                content,
                f"Placeholder not replaced in hook.{ext}",
            )
            m = re.search(r"_ENVY_HOOK_VERSION\s*=?\s*(\d+)", content)
            self.assertIsNotNone(m, f"No numeric version stamp in hook.{ext}")
            versions[ext] = int(m.group(1))  # type: ignore[union-attr]
            self.assertGreater(
                versions[ext], 0, f"Version must be positive in hook.{ext}"
            )
        # All hooks must agree on the version
        unique = set(versions.values())
        self.assertEqual(len(unique), 1, f"Version mismatch across hooks: {versions}")

    def test_hook_files_contain_managed_comment(self) -> None:
        self._trigger_self_deploy()
        shell_dir = self._cache_dir / "shell"
        for ext in ("bash", "zsh", "fish", "ps1"):
            hook = shell_dir / f"hook.{ext}"
            content = hook.read_text(encoding="utf-8")
            self.assertIn("managed by envy", content)

    @unittest.skipUnless(
        sys.platform != "win32", "bash syntax check not applicable on Windows"
    )
    def test_bash_hook_is_syntactically_valid(self) -> None:
        self._trigger_self_deploy()
        hook = self._cache_dir / "shell" / "hook.bash"
        result = test_config.run(
            ["bash", "-n", str(hook)], capture_output=True, text=True, timeout=10
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")

    @unittest.skipUnless(shutil.which("zsh"), "zsh not installed")
    def test_zsh_hook_is_syntactically_valid(self) -> None:
        self._trigger_self_deploy()
        hook = self._cache_dir / "shell" / "hook.zsh"
        result = test_config.run(
            ["zsh", "-n", str(hook)], capture_output=True, text=True, timeout=10
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")

    @unittest.skipUnless(shutil.which("fish"), "fish not installed")
    def test_fish_hook_is_syntactically_valid(self) -> None:
        self._trigger_self_deploy()
        hook = self._cache_dir / "shell" / "hook.fish"
        result = test_config.run(
            ["fish", "--no-execute", str(hook)],
            capture_output=True,
            text=True,
            timeout=10,
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")

    @unittest.skipUnless(shutil.which("pwsh"), "pwsh not installed")
    def test_ps1_hook_is_syntactically_valid(self) -> None:
        self._trigger_self_deploy()
        hook = self._cache_dir / "shell" / "hook.ps1"
        # Parse only — check for syntax errors via ast. run_pwsh retries the
        # nondeterministic .NET-runtime startup aborts seen on CI runners.
        result = test_config.run_pwsh(
            [
                "pwsh",
                "-NoProfile",
                "-Command",
                f"$errors = $null; "
                f"$null = [System.Management.Automation.Language.Parser]"
                f"::ParseFile('{hook}', [ref]$null, [ref]$errors); "
                f"if ($errors) {{ $errors | ForEach-Object {{ Write-Error $_ }}; exit 1 }}",
            ],
            capture_output=True,
            text=True,
            timeout=10,
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")

    def test_stale_hooks_are_updated(self) -> None:
        """Writing an old version stamp then running envy should update the hook."""
        self._trigger_self_deploy()
        hook = self._cache_dir / "shell" / "hook.bash"
        self.assertTrue(hook.exists())

        # Write a stale hook with version 0
        hook.write_text("# envy shell hook v0\n_ENVY_HOOK_VERSION=0\n# stale content\n")

        # Run envy again — should update
        self._project_dir = self._temp_dir / "project2"
        self._bin_dir = self._temp_dir / "bin2"
        self._trigger_self_deploy()

        content = hook.read_text(encoding="utf-8")
        self.assertNotIn("stale content", content)
        self.assertIn("_envy_hook", content)


if __name__ == "__main__":
    unittest.main()
