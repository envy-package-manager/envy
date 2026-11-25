"""Tests for platform utilities including exe path detection."""

import unittest
from pathlib import Path

from . import test_config


class TestPlatformExePath(unittest.TestCase):
    """Verify get_exe_path returns correct path."""

    def _extract_exe_path(self, output: str) -> str:
        """Extract exe path from version output format: 'envy version X.X.X (path)'."""
        for line in output.splitlines():
            if line.startswith("envy version ") and "(" in line and line.endswith(")"):
                start = line.rfind("(") + 1
                return line[start:-1]
        raise ValueError("No 'envy version ... (path)' line found in output")

    def test_exe_path_matches_invoked_binary(self):
        """Exe path should match the binary we invoked."""
        envy_binary = test_config.get_envy_executable()
        result = test_config.run(
            [str(envy_binary), "version"],
            capture_output=True,
            text=True,
            check=True,
        )

        reported_path = Path(self._extract_exe_path(result.stderr))
        expected_path = envy_binary.resolve()

        self.assertEqual(reported_path, expected_path)

    def test_exe_path_is_absolute(self):
        """Reported exe path should be absolute."""
        envy_binary = test_config.get_envy_executable()
        result = test_config.run(
            [str(envy_binary), "version"],
            capture_output=True,
            text=True,
            check=True,
        )

        reported_path = Path(self._extract_exe_path(result.stderr))
        self.assertTrue(reported_path.is_absolute())

    def test_exe_path_exists(self):
        """Reported exe path should exist."""
        envy_binary = test_config.get_envy_executable()
        result = test_config.run(
            [str(envy_binary), "version"],
            capture_output=True,
            text=True,
            check=True,
        )

        reported_path = Path(self._extract_exe_path(result.stderr))
        self.assertTrue(reported_path.exists())


if __name__ == "__main__":
    unittest.main()
