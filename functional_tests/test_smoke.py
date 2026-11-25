from __future__ import annotations

import os
import re
import subprocess
import sys
import unittest

from . import test_config
from pathlib import Path


class EnvyBinarySmokeTest(unittest.TestCase):
    def setUp(self) -> None:
        self._project_root = Path(__file__).resolve().parent.parent
        self._envy_binary = test_config.get_envy_executable()

    def test_envy_help_executes(self) -> None:

        env = os.environ.copy()
        env.setdefault("ENVY_CACHE_DIR", str(self._project_root / "out" / "cache"))
        result = test_config.run(
            [str(self._envy_binary), "version"],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=env,
        )

        self.assertEqual("", result.stdout.strip())
        self.assertIn("envy version", result.stderr)

    def test_version_reports_pinned_components(self) -> None:
        """Each library reports the version cmake/Dependencies.cmake pins, so a stale cached
        source tree (e.g. restored from an older CI cache) can't ship in its place."""
        catalog = (self._project_root / "cmake" / "Dependencies.cmake").read_text()
        pins = dict(re.findall(r'set\(ENVY_(\w+)_VERSION "([^"]+)"\)', catalog))
        components = {
            "libgit2": "LIBGIT2", "libssh2": "LIBSSH2", "libarchive": "LIBARCHIVE",
            "Lua": "LUA", "BLAKE3": "BLAKE3", "zlib": "ZLIB", "bzip2": "LIBBZ2",
            "zstd": "LIBZSTD", "liblzma": "LIBLZMA", "nanosemver": "NANOSEMVER",
        }
        if sys.platform != "win32":
            components["mbedTLS"] = "MBEDTLS"
        if sys.platform.startswith("linux"):  # macOS links the system libcurl
            components["libcurl"] = "LIBCURL"

        result = test_config.run(
            [str(self._envy_binary), "version"], check=True, capture_output=True, text=True
        )
        reported = dict(re.findall(r"^  ([\w-]+): (.*)$", result.stderr, re.MULTILINE))
        for component, pin in components.items():
            with self.subTest(component=component):
                self.assertIn(pins[pin], re.split(r"[\s,()]+", reported[component]))


if __name__ == "__main__":
    unittest.main()
