"""Tests for .luarc.json maintenance across envy commands."""

import json
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from . import test_config
from .env import EnvyTestCase


def _get_envy_version() -> str:
    """Get the baked-in version from the envy binary."""
    result = subprocess.run(
        [str(test_config.get_envy_production_executable()), "version"],
        capture_output=True,
        text=True,
        timeout=10,
    )
    for line in result.stderr.splitlines():
        if line.startswith("envy version "):
            return line.split()[2]
    raise RuntimeError("Could not parse envy version from: " + result.stderr)


_SEMVER_RE = re.compile(
    r"/envy/(\d+\.\d+\.\d+(?:-[A-Za-z0-9.-]+)?(?:\+[A-Za-z0-9.-]+)?)$"
)


def _find_envy_entries(library: list[str]) -> list[str]:
    """Find all envy types entries in workspace.library by semver pattern."""
    return [e for e in library if _SEMVER_RE.search(e)]


class TestLuarcTypesPathUpdate(EnvyTestCase):
    """Test that sync/deploy maintain .luarc.json envy types paths."""

    def setUp(self) -> None:
        self._temp_dir = self.make_temp_dir("_temp_dir")
        self._project_dir = self._temp_dir / "project"
        self._bin_dir = self._temp_dir / "project" / "tools"
        self._cache_dir = self._temp_dir / "cache"
        self._envy = test_config.get_envy_production_executable()
        self._version = _get_envy_version()

    def tearDown(self) -> None:
        if hasattr(self, "_temp_dir") and self._temp_dir.exists():
            shutil.rmtree(self._temp_dir, ignore_errors=True)

    def _run_init(self) -> subprocess.CompletedProcess[str]:
        env = test_config.get_test_env()
        env["ENVY_CACHE_ROOT"] = str(self._cache_dir)
        cmd = [str(self._envy), "init", str(self._project_dir), str(self._bin_dir)]
        return test_config.run(cmd, capture_output=True, text=True, env=env, timeout=30)

    def _run_sync(self) -> subprocess.CompletedProcess[str]:
        env = test_config.get_test_env()
        env["ENVY_CACHE_ROOT"] = str(self._cache_dir)
        manifest = self._project_dir / "envy.lua"
        cmd = [str(self._envy), "sync", "--manifest", str(manifest)]
        return test_config.run(cmd, capture_output=True, text=True, env=env, timeout=30)

    def _run_deploy(self) -> subprocess.CompletedProcess[str]:
        env = test_config.get_test_env()
        env["ENVY_CACHE_ROOT"] = str(self._cache_dir)
        manifest = self._project_dir / "envy.lua"
        cmd = [str(self._envy), "deploy", "--manifest", str(manifest)]
        return test_config.run(cmd, capture_output=True, text=True, env=env, timeout=30)

    def _make_stale_luarc(self) -> None:
        """Replace the envy version in .luarc.json with a fake old version."""
        luarc = self._project_dir / ".luarc.json"
        content = luarc.read_text()
        stale = content.replace(self._version, "0.0.0-stale")
        luarc.write_text(stale)

    # -- stale path updates --

    def test_sync_updates_stale_luarc(self) -> None:
        """envy sync updates stale .luarc.json envy types path."""
        result = self._run_init()
        self.assertEqual(0, result.returncode, f"init stderr: {result.stderr}")

        self._make_stale_luarc()

        luarc = self._project_dir / ".luarc.json"
        self.assertIn("0.0.0-stale", luarc.read_text())

        result = self._run_sync()
        self.assertEqual(0, result.returncode, f"sync stderr: {result.stderr}")

        data = json.loads(luarc.read_text())
        library = data["workspace.library"]
        envy_entries = _find_envy_entries(library)
        self.assertEqual(
            4, len(envy_entries), f"Expected 4 canonical entries: {library}"
        )
        for entry in envy_entries:
            self.assertIn(self._version, entry)
            self.assertNotIn("0.0.0-stale", entry)

    def test_deploy_updates_stale_luarc(self) -> None:
        """envy deploy updates stale .luarc.json envy types path."""
        result = self._run_init()
        self.assertEqual(0, result.returncode, f"init stderr: {result.stderr}")

        self._make_stale_luarc()

        result = self._run_deploy()
        self.assertEqual(0, result.returncode, f"deploy stderr: {result.stderr}")

        luarc = self._project_dir / ".luarc.json"
        data = json.loads(luarc.read_text())
        library = data["workspace.library"]
        envy_entries = _find_envy_entries(library)
        self.assertEqual(
            4, len(envy_entries), f"Expected 4 canonical entries: {library}"
        )
        for entry in envy_entries:
            self.assertIn(self._version, entry)

    # -- preserving custom content --

    def test_sync_preserves_other_luarc_content(self) -> None:
        """envy sync preserves non-envy content in .luarc.json."""
        result = self._run_init()
        self.assertEqual(0, result.returncode, f"init stderr: {result.stderr}")

        luarc = self._project_dir / ".luarc.json"
        data = json.loads(luarc.read_text())
        data["workspace.library"].append("/my/custom/lib")
        data["custom.setting"] = "preserved"
        luarc.write_text(json.dumps(data))

        content = luarc.read_text()
        luarc.write_text(content.replace(self._version, "0.0.0-stale"))

        result = self._run_sync()
        self.assertEqual(0, result.returncode, f"sync stderr: {result.stderr}")

        data = json.loads(luarc.read_text())
        self.assertIn("/my/custom/lib", data["workspace.library"])
        self.assertEqual("preserved", data.get("custom.setting"))

    def test_sync_preserves_custom_entries_when_readding(self) -> None:
        """envy sync preserves all custom library entries when re-adding envy."""
        result = self._run_init()
        self.assertEqual(0, result.returncode, f"init stderr: {result.stderr}")

        luarc = self._project_dir / ".luarc.json"
        data = json.loads(luarc.read_text())
        data["workspace.library"] = ["/lib/a", "/lib/b", "/lib/c"]
        luarc.write_text(json.dumps(data))

        result = self._run_sync()
        self.assertEqual(0, result.returncode, f"sync stderr: {result.stderr}")

        data = json.loads(luarc.read_text())
        library = data["workspace.library"]
        self.assertIn("/lib/a", library)
        self.assertIn("/lib/b", library)
        self.assertIn("/lib/c", library)
        envy_entries = _find_envy_entries(library)
        self.assertEqual(4, len(envy_entries))

    # -- file absent --

    def test_sync_noop_when_luarc_absent(self) -> None:
        """envy sync is a no-op when .luarc.json doesn't exist."""
        result = self._run_init()
        self.assertEqual(0, result.returncode, f"init stderr: {result.stderr}")

        luarc = self._project_dir / ".luarc.json"
        luarc.unlink()

        result = self._run_sync()
        self.assertEqual(0, result.returncode, f"sync stderr: {result.stderr}")

        self.assertFalse(luarc.exists())

    def test_sync_noop_when_luarc_has_no_workspace_library(self) -> None:
        """envy sync leaves .luarc.json alone if it has no workspace.library key."""
        result = self._run_init()
        self.assertEqual(0, result.returncode, f"init stderr: {result.stderr}")

        luarc = self._project_dir / ".luarc.json"
        custom = {"diagnostics.globals": ["envy"], "custom.key": 42}
        luarc.write_text(json.dumps(custom))

        result = self._run_sync()
        self.assertEqual(0, result.returncode, f"sync stderr: {result.stderr}")

        data = json.loads(luarc.read_text())
        self.assertNotIn("workspace.library", data)
        self.assertEqual(42, data["custom.key"])

    # -- idempotency --

    def test_sync_idempotent_no_rewrite(self) -> None:
        """envy sync does not rewrite .luarc.json when already correct."""
        result = self._run_init()
        self.assertEqual(0, result.returncode, f"init stderr: {result.stderr}")

        luarc = self._project_dir / ".luarc.json"
        content_before = luarc.read_text()

        result = self._run_sync()
        self.assertEqual(0, result.returncode, f"sync stderr: {result.stderr}")

        content_after = luarc.read_text()
        self.assertEqual(content_before, content_after)
        self.assertNotIn("Updated", result.stderr)

    # -- deleted envy entry re-added --

    def test_sync_readds_deleted_envy_entry(self) -> None:
        """envy sync re-adds envy types entry if user deleted it."""
        result = self._run_init()
        self.assertEqual(0, result.returncode, f"init stderr: {result.stderr}")

        luarc = self._project_dir / ".luarc.json"
        data = json.loads(luarc.read_text())
        data["workspace.library"] = ["/my/custom/lib"]
        luarc.write_text(json.dumps(data))

        result = self._run_sync()
        self.assertEqual(0, result.returncode, f"sync stderr: {result.stderr}")

        data = json.loads(luarc.read_text())
        library = data["workspace.library"]
        self.assertIn("/my/custom/lib", library)
        envy_entries = _find_envy_entries(library)
        self.assertEqual(
            4, len(envy_entries), f"Envy entries should be re-added: {library}"
        )
        for entry in envy_entries:
            self.assertIn(self._version, entry)

    def test_deploy_readds_deleted_envy_entry(self) -> None:
        """envy deploy re-adds envy types entry if user deleted it."""
        result = self._run_init()
        self.assertEqual(0, result.returncode, f"init stderr: {result.stderr}")

        luarc = self._project_dir / ".luarc.json"
        data = json.loads(luarc.read_text())
        data["workspace.library"] = []
        luarc.write_text(json.dumps(data))

        result = self._run_deploy()
        self.assertEqual(0, result.returncode, f"deploy stderr: {result.stderr}")

        data = json.loads(luarc.read_text())
        library = data["workspace.library"]
        envy_entries = _find_envy_entries(library)
        self.assertEqual(
            4, len(envy_entries), f"Envy entries should be re-added: {library}"
        )


if __name__ == "__main__":
    unittest.main()
