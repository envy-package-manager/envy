from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest

from . import test_config
from pathlib import Path


class EnvyLuaTests(unittest.TestCase):
    def setUp(self) -> None:
        self._project_root = Path(__file__).resolve().parent.parent
        self._envy_binary = test_config.get_envy_executable()

    def _run_envy(self, *args: str) -> subprocess.CompletedProcess[str]:
        env = os.environ.copy()
        env.setdefault("ENVY_CACHE_DIR", str(self._project_root / "out" / "cache"))
        return test_config.run(
            [str(self._envy_binary), *args],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=env,
        )

    def test_missing_script_returns_error(self) -> None:
        result = self._run_envy("lua", "missing.lua")

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("File does not exist", result.stderr)
        self.assertEqual("", result.stdout.strip())

    def test_invalid_lua_returns_error(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            script = Path(tmpdir) / "invalid.lua"
            script.write_text("this is invalid\n", encoding="utf-8")

            result = self._run_envy("lua", str(script))

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("invalid.lua", result.stderr)
        self.assertEqual("", result.stdout.strip())

    def test_valid_script_executes(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            script = Path(tmpdir) / "hello.lua"
            script.write_text("envy.stdout('hello')\n", encoding="utf-8")

            result = self._run_envy("lua", str(script))

        self.assertEqual(0, result.returncode)
        self.assertEqual("hello", result.stdout.strip())
        self.assertEqual("", result.stderr.strip())

    def test_abspath_from_spec(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            script = Path(tmpdir) / "spec.lua"
            script.write_text(
                "envy.stdout(envy.abspath('file.txt'))\n", encoding="utf-8"
            )

            result = self._run_envy("lua", str(script))

        self.assertEqual(0, result.returncode)
        expected = str(Path(tmpdir) / "file.txt")
        self.assertEqual(expected, result.stdout.strip())
        self.assertEqual("", result.stderr.strip())

    def test_abspath_from_required_helper(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            # Create subdirectory with helper
            subdir = Path(tmpdir) / "helpers"
            subdir.mkdir()
            helper = subdir / "helper.lua"
            helper.write_text(
                "local M = {}\nfunction M.get_path() return envy.abspath('file.txt') end\nreturn M\n",
                encoding="utf-8",
            )

            # Main script requires helper
            # Use forward slashes for Lua compatibility (avoids escape sequence issues on Windows)
            lua_tmpdir = tmpdir.replace("\\", "/")
            script = Path(tmpdir) / "main.lua"
            script.write_text(
                f"package.path = '{lua_tmpdir}/?.lua;' .. package.path\n"
                "local helper = require('helpers.helper')\n"
                "envy.stdout(helper.get_path())\n",
                encoding="utf-8",
            )

            result = self._run_envy("lua", str(script))

        self.assertEqual(0, result.returncode)
        # Should resolve relative to helper's directory, not main's
        expected = str(subdir / "file.txt")
        self.assertEqual(expected, result.stdout.strip())
        self.assertEqual("", result.stderr.strip())

    def test_abspath_from_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            manifest = Path(tmpdir) / "manifest.lua"
            manifest.write_text(
                "envy.stdout(envy.abspath('local/path.txt'))\n", encoding="utf-8"
            )

            result = self._run_envy("lua", str(manifest))

        self.assertEqual(0, result.returncode)
        expected = str(Path(tmpdir) / "local" / "path.txt")
        self.assertEqual(expected, result.stdout.strip())
        self.assertEqual("", result.stderr.strip())

    def test_abspath_rejects_absolute_path(self) -> None:
        # Use platform-appropriate absolute path
        # Windows path needs doubled backslashes for Lua string escaping
        abs_path = r"C:\\abs\\path" if sys.platform == "win32" else "/abs/path"
        with tempfile.TemporaryDirectory() as tmpdir:
            script = Path(tmpdir) / "test.lua"
            script.write_text(
                f"envy.stdout(envy.abspath('{abs_path}'))\n", encoding="utf-8"
            )

            result = self._run_envy("lua", str(script))

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("must be relative", result.stderr)

    def test_decode_upstream_metadata_files(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            (root / "channel.toml").write_text(
                "[pkg.rust-std.target.aarch64-apple-darwin]\n"
                'xz_url = "https://example.com/rust-std.tar.xz"\n',
                encoding="utf-8",
            )
            (root / "index.json").write_text(
                '[{"version": "v24.11.1", "lts": "Krypton"}]', encoding="utf-8"
            )
            script = root / "spec.lua"
            script.write_text(
                "local function slurp(p)\n"
                "  local f = assert(io.open(envy.abspath(p), 'rb'))\n"
                "  local s = f:read('a')\n"
                "  f:close()\n"
                "  return s\n"
                "end\n"
                "local m = envy.toml.decode(slurp('channel.toml'))\n"
                "local r = envy.json.decode(slurp('index.json'))\n"
                "envy.stdout(m.pkg['rust-std'].target['aarch64-apple-darwin'].xz_url\n"
                "  .. ' ' .. r[1].version .. ' ' .. r[1].lts)\n",
                encoding="utf-8",
            )

            result = self._run_envy("lua", str(script))

        self.assertEqual(0, result.returncode, result.stderr)
        self.assertEqual(
            "https://example.com/rust-std.tar.xz v24.11.1 Krypton", result.stdout.strip()
        )

    def test_decode_error_names_decoder(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            script = Path(tmpdir) / "bad.lua"
            script.write_text("envy.toml.decode('a = ')\n", encoding="utf-8")

            result = self._run_envy("lua", str(script))

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("envy.toml.decode: ", result.stderr)


if __name__ == "__main__":
    unittest.main()
