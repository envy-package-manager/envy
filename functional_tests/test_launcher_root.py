"""Tests for launcher script root directive discovery.

These tests verify that the shell launcher scripts (envy, envy.bat) correctly
implement root-aware manifest discovery matching the C++ manifest::discover().

Test matrix (15 scenarios) using 3 levels: child -> parent -> grandparent
Notation: F=root false, T=root true, A=absent (defaults to true), -=no manifest

| # | child | parent | grandparent | Expected |
|---|-------|--------|-------------|----------|
| 1 | F     | F      | F           | grandparent (topmost non-root) |
| 2 | F     | F      | T           | grandparent (root found) |
| 3 | F     | F      | A           | grandparent (absent=root) |
| 4 | F     | T      | F           | parent (root found, stops) |
| 5 | F     | T      | T           | parent (root found) |
| 6 | F     | T      | A           | parent (root found) |
| 7 | F     | A      | F           | parent (absent=root, stops) |
| 8 | F     | A      | T           | parent (absent=root) |
| 9 | F     | A      | A           | parent (absent=root) |
| 10| T     | F      | F           | child (root found immediately) |
| 11| T     | T      | T           | child (root found immediately) |
| 12| A     | F      | F           | child (absent=root, stops) |
| 13| F     | F      | -           | parent (no grandparent manifest) |
| 14| F     | -      | F           | grandparent (parent has no manifest) |
| 15| F     | -      | -           | child (only manifest in tree) |
"""

import os
import shutil
import subprocess
import sys
import tempfile
import unittest

from . import test_config
from .env import EnvyTestCase
from pathlib import Path


def _make_manifest(root_value: str | None) -> str:
    """Create minimal manifest content with specified root directive."""
    lines = ['-- @envy bin "tools"']
    if root_value is not None:
        lines.append(f'-- @envy root "{root_value}"')
    lines.append("PACKAGES = {}")
    return "\n".join(lines)


# `root "false"` sitting under the first line of code. Not a directive -- parse_envy_meta
# stops the header there -- so both launchers must read this manifest as root, the same way
# the binary does. The launchers used to scan a flat line count instead, which took it.
_ROOT_FALSE_BELOW_CODE = "\n".join(
    ['-- @envy bin "tools"', "PACKAGES = {}", '-- @envy root "false"']
)

# `root "false"` past the 20th line, under a preamble of comments and blanks. A directive, so
# both launchers must walk past this manifest. The old line cap stopped short of it and read
# the manifest as root, disagreeing with the binary about which project it was in.
_ROOT_FALSE_AFTER_PREAMBLE = "\n".join(
    ['-- @envy bin "tools"']
    + [f"-- preamble line {i}" if i % 5 else "" for i in range(1, 26)]
    + ['-- @envy root "false"', "PACKAGES = {}"]
)

# Tab-indented, behind a tab-indented plain comment. Still the header for parse_envy_meta,
# which skips spaces and tabs alike, so both launchers must walk past this manifest.
# envy.bat's `for /f` gave `delims=` space alone, leaving the tab in the first token, so the
# comment read as code and ended the scan before the directive under it.
_ROOT_FALSE_TAB_INDENTED = "\n".join(
    [
        '-- @envy bin "tools"',
        "\t-- a tab-indented comment",
        '\t-- @envy root "false"',
        "PACKAGES = {}",
    ]
)

# A `;`-led line is code -- Lua's empty statement prefixing a statement -- so the header ends
# on it and the `root "false"` below is not a directive. envy.bat's `for /f` treats `;` as a
# comment marker unless given `eol=`, and skipped the line instead of stopping at it.
_ROOT_FALSE_BELOW_SEMICOLON = "\n".join(
    ['-- @envy bin "tools"', ";PACKAGES = {}", '-- @envy root "false"']
)

# `root` twice in one header. parse_envy_meta assigns per match, so the last wins and this
# manifest is not a root; both launchers overwrite the same way and must walk past it.
_ROOT_REPEATED_LAST_WINS = "\n".join(
    ['-- @envy root "true"', '-- @envy root "false"', "PACKAGES = {}"]
)


_LAUNCHER_DIR = Path(__file__).resolve().parent.parent / "src" / "resources"


def _splice(text: str, anchor: str, replacement: str) -> str:
    """Replace `anchor` in `text`, failing loudly if the shipped script no longer has it.

    The harnesses below graft a start-directory override and a print onto the real launcher
    rather than restating its walk. A silent no-op replace would leave the 15 scenarios
    exercising a script that never reaches find_manifest, reporting green.
    """
    if anchor not in text:
        raise AssertionError(f"launcher no longer contains {anchor!r}; update this harness")
    return text.replace(anchor, replacement, 1)


def _get_bash_find_manifest_script() -> str:
    """Return the shipped bash launcher, cut short after find_manifest and told to print it.

    Derived from src/resources/envy, never restated: a hand-copied walk drifts from the
    script that ships, and then the matrix below certifies the copy. Everything from the
    real script's first use of the result onward is dropped, so nothing downloads.
    """
    src = (_LAUNCHER_DIR / "envy").read_text(encoding="utf-8")
    anchor = "MANIFEST=$(find_manifest)"
    if anchor not in src:
        raise AssertionError(f"launcher no longer contains {anchor!r}; update this harness")
    # resolve_script_dir takes no arguments, so the start directory arrives in a global.
    return src.split(anchor)[0] + (
        'START_DIR="$1"\nresolve_script_dir() { echo "$START_DIR"; }\nfind_manifest\n'
    )


@unittest.skipIf(sys.platform == "win32", "Bash tests skipped on Windows")
class TestBashLauncherRootDiscovery(EnvyTestCase):
    """Test bash launcher's root-aware manifest discovery."""

    def setUp(self) -> None:
        # Use resolve() to get canonical path (handles /var -> /private/var on macOS)
        self._temp_dir = self.make_temp_dir("_temp_dir").resolve()
        self._grandparent = self._temp_dir / "grandparent"
        self._parent = self._grandparent / "parent"
        self._child = self._parent / "child"
        self._child.mkdir(parents=True)

        # Create the test script
        self._script = self._temp_dir / "test_find_manifest.sh"
        self._script.write_text(_get_bash_find_manifest_script())
        self._script.chmod(0o755)

    def tearDown(self) -> None:
        if hasattr(self, "_temp_dir") and self._temp_dir.exists():
            shutil.rmtree(self._temp_dir, ignore_errors=True)

    def _write_manifest(self, directory: Path, root_value: str | None) -> None:
        """Write a manifest to the given directory."""
        manifest = directory / "envy.lua"
        manifest.write_text(_make_manifest(root_value))

    def _run_find_manifest(self, start_dir: Path) -> Path | None:
        """Run find_manifest from given directory, return discovered manifest path."""
        result = test_config.run(
            [str(self._script), str(start_dir)],
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            return None
        return Path(result.stdout.strip())

    def _assert_manifest_at(
        self, expected_dir: Path, start_dir: Path | None = None
    ) -> None:
        """Assert find_manifest returns manifest in expected directory."""
        if start_dir is None:
            start_dir = self._child
        found = self._run_find_manifest(start_dir)
        self.assertIsNotNone(found, "find_manifest should find a manifest")
        expected = expected_dir / "envy.lua"
        self.assertEqual(
            expected, found, f"Expected manifest at {expected}, got {found}"
        )

    # Scenario 1: F F F -> grandparent (topmost non-root)
    def test_scenario_01_fff_uses_grandparent(self) -> None:
        self._write_manifest(self._child, "false")
        self._write_manifest(self._parent, "false")
        self._write_manifest(self._grandparent, "false")
        self._assert_manifest_at(self._grandparent)

    # Scenario 2: F F T -> grandparent (root found)
    def test_scenario_02_fft_uses_grandparent(self) -> None:
        self._write_manifest(self._child, "false")
        self._write_manifest(self._parent, "false")
        self._write_manifest(self._grandparent, "true")
        self._assert_manifest_at(self._grandparent)

    # Scenario 3: F F A -> grandparent (absent=root)
    def test_scenario_03_ffa_uses_grandparent(self) -> None:
        self._write_manifest(self._child, "false")
        self._write_manifest(self._parent, "false")
        self._write_manifest(self._grandparent, None)  # absent = root
        self._assert_manifest_at(self._grandparent)

    # Scenario 4: F T F -> parent (root found, stops)
    def test_scenario_04_ftf_uses_parent(self) -> None:
        self._write_manifest(self._child, "false")
        self._write_manifest(self._parent, "true")
        self._write_manifest(self._grandparent, "false")
        self._assert_manifest_at(self._parent)

    # Scenario 5: F T T -> parent (root found)
    def test_scenario_05_ftt_uses_parent(self) -> None:
        self._write_manifest(self._child, "false")
        self._write_manifest(self._parent, "true")
        self._write_manifest(self._grandparent, "true")
        self._assert_manifest_at(self._parent)

    # Scenario 6: F T A -> parent (root found)
    def test_scenario_06_fta_uses_parent(self) -> None:
        self._write_manifest(self._child, "false")
        self._write_manifest(self._parent, "true")
        self._write_manifest(self._grandparent, None)
        self._assert_manifest_at(self._parent)

    # Scenario 7: F A F -> parent (absent=root, stops)
    def test_scenario_07_faf_uses_parent(self) -> None:
        self._write_manifest(self._child, "false")
        self._write_manifest(self._parent, None)  # absent = root
        self._write_manifest(self._grandparent, "false")
        self._assert_manifest_at(self._parent)

    # Scenario 8: F A T -> parent (absent=root)
    def test_scenario_08_fat_uses_parent(self) -> None:
        self._write_manifest(self._child, "false")
        self._write_manifest(self._parent, None)  # absent = root
        self._write_manifest(self._grandparent, "true")
        self._assert_manifest_at(self._parent)

    # Scenario 9: F A A -> parent (absent=root)
    def test_scenario_09_faa_uses_parent(self) -> None:
        self._write_manifest(self._child, "false")
        self._write_manifest(self._parent, None)  # absent = root
        self._write_manifest(self._grandparent, None)
        self._assert_manifest_at(self._parent)

    # Scenario 10: T F F -> child (root found immediately)
    def test_scenario_10_tff_uses_child(self) -> None:
        self._write_manifest(self._child, "true")
        self._write_manifest(self._parent, "false")
        self._write_manifest(self._grandparent, "false")
        self._assert_manifest_at(self._child)

    # Scenario 11: T T T -> child (root found immediately)
    def test_scenario_11_ttt_uses_child(self) -> None:
        self._write_manifest(self._child, "true")
        self._write_manifest(self._parent, "true")
        self._write_manifest(self._grandparent, "true")
        self._assert_manifest_at(self._child)

    # Scenario 12: A F F -> child (absent=root, stops)
    def test_scenario_12_aff_uses_child(self) -> None:
        self._write_manifest(self._child, None)  # absent = root
        self._write_manifest(self._parent, "false")
        self._write_manifest(self._grandparent, "false")
        self._assert_manifest_at(self._child)

    # Scenario 13: F F - -> parent (no grandparent manifest)
    def test_scenario_13_ff_dash_uses_parent(self) -> None:
        self._write_manifest(self._child, "false")
        self._write_manifest(self._parent, "false")
        # No grandparent manifest
        self._assert_manifest_at(self._parent)

    # Scenario 14: F - F -> grandparent (parent has no manifest)
    def test_scenario_14_f_dash_f_uses_grandparent(self) -> None:
        self._write_manifest(self._child, "false")
        # No parent manifest
        self._write_manifest(self._grandparent, "false")
        self._assert_manifest_at(self._grandparent)

    # Scenario 15: F - - -> child (only manifest in tree)
    def test_scenario_15_f_dash_dash_uses_child(self) -> None:
        self._write_manifest(self._child, "false")
        # No parent or grandparent manifest
        self._assert_manifest_at(self._child)

    # --- where the header ends -------------------------------------------------------
    #
    # The walk reads the same header the binary does: comments and blank lines, stopping at
    # the first line of code, with no line cap. Disagreeing with manifest::discover() here
    # puts the launcher and the binary it execs in different projects.

    def test_root_false_below_the_first_code_line_is_not_a_directive(self) -> None:
        (self._child / "envy.lua").write_text(_ROOT_FALSE_BELOW_CODE)
        self._write_manifest(self._parent, None)
        self._assert_manifest_at(self._child)

    def test_root_false_under_a_long_preamble_is_still_a_directive(self) -> None:
        (self._child / "envy.lua").write_text(_ROOT_FALSE_AFTER_PREAMBLE)
        self._write_manifest(self._parent, None)
        self._assert_manifest_at(self._parent)

    def test_tab_indented_root_false_is_still_a_directive(self) -> None:
        (self._child / "envy.lua").write_text(_ROOT_FALSE_TAB_INDENTED)
        self._write_manifest(self._parent, None)
        self._assert_manifest_at(self._parent)

    def test_semicolon_led_line_ends_the_header(self) -> None:
        (self._child / "envy.lua").write_text(_ROOT_FALSE_BELOW_SEMICOLON)
        self._write_manifest(self._parent, None)
        self._assert_manifest_at(self._child)

    def test_repeated_root_directive_takes_the_last(self) -> None:
        (self._child / "envy.lua").write_text(_ROOT_REPEATED_LAST_WINS)
        self._write_manifest(self._parent, None)
        self._assert_manifest_at(self._parent)


@unittest.skipUnless(sys.platform == "win32", "Windows-only tests")
class TestBatchLauncherRootDiscovery(EnvyTestCase):
    """Test batch launcher's root-aware manifest discovery (Windows only)."""

    def setUp(self) -> None:
        self._temp_dir = self.make_temp_dir("_temp_dir")
        self._grandparent = self._temp_dir / "grandparent"
        self._parent = self._grandparent / "parent"
        self._child = self._parent / "child"
        self._child.mkdir(parents=True)

        # Create the test batch script
        self._script = self._temp_dir / "test_find_manifest.bat"
        self._script.write_text(self._get_batch_find_manifest_script())

    def tearDown(self) -> None:
        if hasattr(self, "_temp_dir") and self._temp_dir.exists():
            shutil.rmtree(self._temp_dir, ignore_errors=True)

    def _get_batch_find_manifest_script(self) -> str:
        """Return the shipped envy.bat, with the walk's start overridable and :found printing.

        Derived from src/resources/envy.bat for the same reason as the bash side. Spliced in
        place rather than sliced: the walk calls :read_root, which lives with the other
        subroutines at the end of the file, so a prefix cut would drop it. `exit /b 0` right
        after the print makes the rest of the real script unreachable, so nothing downloads.
        """
        src = (_LAUNCHER_DIR / "envy.bat").read_text(encoding="utf-8")
        src = _splice(
            src,
            'set "DIR=%~dp0"\nif "!DIR:~-1!"=="\\" set "DIR=!DIR:~0,-1!"',
            'if "%~1"=="" (\n'
            '    set "DIR=%~dp0"\n'
            '    if "!DIR:~-1!"=="\\" set "DIR=!DIR:~0,-1!"\n'
            ') else (\n'
            '    set "DIR=%~1"\n'
            ')',
        )
        # Anchored on the label at line start, not on `:found` anywhere: the `goto :found`
        # lines above it would otherwise take the splice and leave the label itself bare.
        return _splice(src, "\n:found\n", "\n:found\necho !MANIFEST!\nexit /b 0\n")

    def _write_manifest(self, directory: Path, root_value: str | None) -> None:
        """Write a manifest to the given directory."""
        manifest = directory / "envy.lua"
        manifest.write_text(_make_manifest(root_value))

    def _run_find_manifest(self, start_dir: Path) -> Path | None:
        """Run find_manifest from given directory, return discovered manifest path."""
        result = test_config.run(
            ["cmd", "/c", str(self._script), str(start_dir)],
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            return None
        return Path(result.stdout.strip())

    def _assert_manifest_at(
        self, expected_dir: Path, start_dir: Path | None = None
    ) -> None:
        """Assert find_manifest returns manifest in expected directory."""
        if start_dir is None:
            start_dir = self._child
        found = self._run_find_manifest(start_dir)
        self.assertIsNotNone(found, "find_manifest should find a manifest")
        expected = expected_dir / "envy.lua"
        self.assertEqual(
            expected, found, f"Expected manifest at {expected}, got {found}"
        )

    # All 15 scenarios - same as bash tests
    def test_scenario_01_fff_uses_grandparent(self) -> None:
        self._write_manifest(self._child, "false")
        self._write_manifest(self._parent, "false")
        self._write_manifest(self._grandparent, "false")
        self._assert_manifest_at(self._grandparent)

    def test_scenario_02_fft_uses_grandparent(self) -> None:
        self._write_manifest(self._child, "false")
        self._write_manifest(self._parent, "false")
        self._write_manifest(self._grandparent, "true")
        self._assert_manifest_at(self._grandparent)

    def test_scenario_03_ffa_uses_grandparent(self) -> None:
        self._write_manifest(self._child, "false")
        self._write_manifest(self._parent, "false")
        self._write_manifest(self._grandparent, None)
        self._assert_manifest_at(self._grandparent)

    def test_scenario_04_ftf_uses_parent(self) -> None:
        self._write_manifest(self._child, "false")
        self._write_manifest(self._parent, "true")
        self._write_manifest(self._grandparent, "false")
        self._assert_manifest_at(self._parent)

    def test_scenario_05_ftt_uses_parent(self) -> None:
        self._write_manifest(self._child, "false")
        self._write_manifest(self._parent, "true")
        self._write_manifest(self._grandparent, "true")
        self._assert_manifest_at(self._parent)

    def test_scenario_06_fta_uses_parent(self) -> None:
        self._write_manifest(self._child, "false")
        self._write_manifest(self._parent, "true")
        self._write_manifest(self._grandparent, None)
        self._assert_manifest_at(self._parent)

    def test_scenario_07_faf_uses_parent(self) -> None:
        self._write_manifest(self._child, "false")
        self._write_manifest(self._parent, None)
        self._write_manifest(self._grandparent, "false")
        self._assert_manifest_at(self._parent)

    def test_scenario_08_fat_uses_parent(self) -> None:
        self._write_manifest(self._child, "false")
        self._write_manifest(self._parent, None)
        self._write_manifest(self._grandparent, "true")
        self._assert_manifest_at(self._parent)

    def test_scenario_09_faa_uses_parent(self) -> None:
        self._write_manifest(self._child, "false")
        self._write_manifest(self._parent, None)
        self._write_manifest(self._grandparent, None)
        self._assert_manifest_at(self._parent)

    def test_scenario_10_tff_uses_child(self) -> None:
        self._write_manifest(self._child, "true")
        self._write_manifest(self._parent, "false")
        self._write_manifest(self._grandparent, "false")
        self._assert_manifest_at(self._child)

    def test_scenario_11_ttt_uses_child(self) -> None:
        self._write_manifest(self._child, "true")
        self._write_manifest(self._parent, "true")
        self._write_manifest(self._grandparent, "true")
        self._assert_manifest_at(self._child)

    def test_scenario_12_aff_uses_child(self) -> None:
        self._write_manifest(self._child, None)
        self._write_manifest(self._parent, "false")
        self._write_manifest(self._grandparent, "false")
        self._assert_manifest_at(self._child)

    def test_scenario_13_ff_dash_uses_parent(self) -> None:
        self._write_manifest(self._child, "false")
        self._write_manifest(self._parent, "false")
        self._assert_manifest_at(self._parent)

    def test_scenario_14_f_dash_f_uses_grandparent(self) -> None:
        self._write_manifest(self._child, "false")
        self._write_manifest(self._grandparent, "false")
        self._assert_manifest_at(self._grandparent)

    def test_scenario_15_f_dash_dash_uses_child(self) -> None:
        self._write_manifest(self._child, "false")
        self._assert_manifest_at(self._child)

    # --- where the header ends: same contract as the bash walk above ------------------

    def test_root_false_below_the_first_code_line_is_not_a_directive(self) -> None:
        (self._child / "envy.lua").write_text(_ROOT_FALSE_BELOW_CODE)
        self._write_manifest(self._parent, None)
        self._assert_manifest_at(self._child)

    def test_root_false_under_a_long_preamble_is_still_a_directive(self) -> None:
        (self._child / "envy.lua").write_text(_ROOT_FALSE_AFTER_PREAMBLE)
        self._write_manifest(self._parent, None)
        self._assert_manifest_at(self._parent)

    def test_tab_indented_root_false_is_still_a_directive(self) -> None:
        (self._child / "envy.lua").write_text(_ROOT_FALSE_TAB_INDENTED)
        self._write_manifest(self._parent, None)
        self._assert_manifest_at(self._parent)

    def test_semicolon_led_line_ends_the_header(self) -> None:
        (self._child / "envy.lua").write_text(_ROOT_FALSE_BELOW_SEMICOLON)
        self._write_manifest(self._parent, None)
        self._assert_manifest_at(self._child)

    def test_repeated_root_directive_takes_the_last(self) -> None:
        (self._child / "envy.lua").write_text(_ROOT_REPEATED_LAST_WINS)
        self._write_manifest(self._parent, None)
        self._assert_manifest_at(self._parent)


if __name__ == "__main__":
    unittest.main()
