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
    anchor = "ENVY_MANIFEST=$(find_manifest)"
    if anchor not in src:
        raise AssertionError(f"launcher no longer contains {anchor!r}; update this harness")
    # resolve_script_dir takes no arguments, so the start directory arrives in a global.
    return src.split(anchor)[0] + (
        'ENVY_START_DIR="$1"\nresolve_script_dir() { echo "$ENVY_START_DIR"; }\nfind_manifest\n'
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
        # write_text emits CRLF on Windows; see the note in TestBatchCacheRootParity.
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
            'set "ENVY_DIR=%~dp0"\nif "!ENVY_DIR:~-1!"=="\\" set "ENVY_DIR=!ENVY_DIR:~0,-1!"',
            'if "%~1"=="" (\n'
            '    set "ENVY_DIR=%~dp0"\n'
            '    if "!ENVY_DIR:~-1!"=="\\" set "ENVY_DIR=!ENVY_DIR:~0,-1!"\n'
            ') else (\n'
            '    set "ENVY_DIR=%~1"\n'
            ')',
        )
        # Anchored on the label at line start, not on `:found` anywhere: the `goto :found`
        # lines above it would otherwise take the splice and leave the label itself bare.
        return _splice(src, "\n:found\n", "\n:found\necho !ENVY_MANIFEST!\nexit /b 0\n")

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


# ---------------------------------------------------------------------------------------
# Cache-root parity
#
# The load-bearing test of the whole cache-root design. The launcher resolves the cache root
# before any envy binary exists, and the binary resolves it again -- and for a long time
# they disagreed: three expansion grammars across four readers, none of them the documented
# one. A launcher that resolves a different root bootstraps envy into one tree while the
# binary it execs installs packages into another, and the binary then prints the right
# answer *after* the wrong one was already used, which is what made the class of bug
# invisible.
#
# Both harnesses graft a print onto the real shipped launcher, exactly as the root-discovery
# tests above do, and cut it off before any network access.


def _get_bash_cache_root_script() -> str:
    """The shipped bash launcher, told where to start, cut short after CACHE is resolved.

    Truncated at the version-resolution chain: everything the cache tiers need is above it,
    and nothing below runs, so no download can happen. The cache block itself calls no
    function defined later in the file.
    """
    src = (_LAUNCHER_DIR / "envy").read_text(encoding="utf-8")
    # resolve_script_dir takes no arguments, so the start directory arrives in a global.
    src = _splice(
        src,
        "resolve_script_dir() {",
        'ENVY_START_DIR="${1:-}"\n'
        'resolve_script_dir() { echo "$ENVY_START_DIR"; return; }\n'
        "_unused_resolve_script_dir() {",
    )
    anchor = 'if [[ -z "$ENVY_VERSION" ]]; then'
    if anchor not in src:
        raise AssertionError(f"launcher no longer contains {anchor!r}; update this harness")
    # Both roots: ENVY_SHARED_CACHE is the second root this launcher computes, and a second
    # root resolved twice with nothing comparing the two answers is how this area went
    # wrong the first time. Empty line 2 means "no user-wide root", which is meaningful.
    return src.split(anchor)[0] + 'echo "$ENVY_CACHE"\necho "$ENVY_SHARED_CACHE"\n'


def _get_batch_cache_root_script() -> str:
    """The shipped envy.bat, told where to start, printing CACHE before it resolves a version.

    Spliced rather than sliced: the cache tiers `call :quoted_value` and `:require_absolute`,
    which live with the other subroutines at the end of the file.
    """
    src = (_LAUNCHER_DIR / "envy.bat").read_text(encoding="utf-8")
    src = _splice(
        src,
        'set "ENVY_DIR=%~dp0"\nif "!ENVY_DIR:~-1!"=="\\" set "ENVY_DIR=!ENVY_DIR:~0,-1!"',
        'if "%~1"=="" (\n'
        '    set "ENVY_DIR=%~dp0"\n'
        '    if "!ENVY_DIR:~-1!"=="\\" set "ENVY_DIR=!ENVY_DIR:~0,-1!"\n'
        ") else (\n"
        '    set "ENVY_DIR=%~1"\n'
        ")",
    )
    # `echo.` rather than `echo` for the second line: an empty ENVY_SHARED_CACHE would
    # otherwise make cmd print "ECHO is on." instead of a blank line.
    return _splice(
        src,
        '\nif "!ENVY_VERSION!"=="" (\n',
        '\necho !ENVY_CACHE!\necho.!ENVY_SHARED_CACHE!\nexit /b 0\n'
        'if "!ENVY_VERSION!"=="" (\n',
    )


# Each case is (name, manifest directives, markers to create under the state dir).
_PARITY_CASES: list[tuple[str, str, tuple[str, ...]]] = [
    ("no directives at all", "", ()),
    ("cache-local implies local", '-- @envy cache-local "out/.envy"\n', ()),
    ("cache-local with a deeper path", '-- @envy cache-local "a/b/c"\n', ()),
    ("cache-mode local, no cache-local", '-- @envy cache-mode "local"\n', ()),
    (
        "cache-mode shared overrides the implication",
        '-- @envy cache-local "out/.envy"\n-- @envy cache-mode "shared"\n',
        (),
    ),
    ("local marker on a shared-default project", "", (".envy-cache-local",)),
    (
        "shared marker on a local-default project",
        '-- @envy cache-local "out/.envy"\n',
        (".envy-cache-shared",),
    ),
    (
        "local marker agreeing with the default",
        '-- @envy cache-local "out/.envy"\n',
        (".envy-cache-local",),
    ),
    (
        "a relocated state dir",
        '-- @envy cache-local "out/.envy"\n-- @envy state-dir "out/state"\n',
        (),
    ),
    # The marker has to be found in the relocated directory, not beside the manifest.
    (
        "a marker in a relocated state dir",
        '-- @envy state-dir "st"\n',
        ("st/.envy-cache-local",),
    ),
    # A trailing comment on a directive line: envy.bat used to fold it into the value.
    (
        "a directive with a trailing comment",
        '-- @envy cache-local "out/.envy"   -- where the tree goes\n',
        (),
    ),
]


class _CacheRootParityMixin:
    """Asserts launcher-computed CACHE == `envy cache --root` for every tier.

    A mixin rather than a TestCase subclass: as a base it would be collected and run on its
    own, with no _launcher_root to call.
    """

    def setUp(self) -> None:
        self._temp_dir = self.make_temp_dir("_temp_dir").resolve()
        self._envy = test_config.get_envy_executable()

    def tearDown(self) -> None:
        if hasattr(self, "_temp_dir") and self._temp_dir.exists():
            shutil.rmtree(self._temp_dir, ignore_errors=True)

    def _make_project(self, name: str, directives: str, markers: tuple[str, ...]) -> Path:
        project = self._temp_dir / name
        project.mkdir(parents=True)
        (project / "envy.lua").write_text(
            '-- @envy bin "tools"\n' + directives + "PACKAGES = {}\n"
        )
        for m in markers:
            target = project / m
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(b"")
        return project

    def _sandbox_env(self, project: Path) -> dict:
        """Point every platform-default cache variable into the temp tree.

        Both readers must agree on the shared root too, and the real one belongs to the
        developer running the suite.
        """
        return test_config.sandbox_home_env(project / "home")

    def _binary_root(self, project: Path, env: dict) -> Path:
        result = test_config.run(
            [str(self._envy), "cache", "--root"],
            cwd=project,
            capture_output=True,
            text=True,
            env=env,
        )
        self.assertEqual(0, result.returncode, f"envy cache --root: {result.stderr}")
        return Path(result.stdout.strip())

    def _binary_user_wide_root(self, project: Path, env: dict) -> Path:
        result = test_config.run(
            [str(self._envy), "cache", "--user-wide-root"],
            cwd=project,
            capture_output=True,
            text=True,
            env=env,
        )
        self.assertEqual(
            0, result.returncode, f"envy cache --user-wide-root: {result.stderr}"
        )
        return Path(result.stdout.strip())

    def _check_case(self, name: str, directives: str, markers: tuple[str, ...]) -> None:
        slug = name.replace(" ", "-").replace(",", "")
        project = self._make_project(slug, directives, markers)
        env = self._sandbox_env(project)

        launcher, launcher_user_wide = self._launcher_roots(project, env)
        binary = self._binary_root(project, env)

        # Path objects, not strings: operator/ leaves 'C:\\p' / 'out/.envy' with a mixed
        # separator while envy.bat's %~fI normalizes, and that difference is not a bug.
        self.assertEqual(
            binary.resolve(),
            launcher.resolve(),
            f"{name}: launcher said {launcher}, binary said {binary}",
        )

        # The second root, which decides whether a local tree may borrow an envy binary
        # instead of downloading one. Nothing compared it until this line existed.
        binary_user_wide = self._binary_user_wide_root(project, env)
        self.assertEqual(
            binary_user_wide.resolve(),
            Path(launcher_user_wide).resolve(),
            f"{name}: launcher user-wide {launcher_user_wide!r}, "
            f"binary {binary_user_wide}",
        )

    def _launcher_root(self, project: Path, env: dict) -> Path:
        return self._launcher_roots(project, env)[0]

    def test_parity_across_every_tier(self) -> None:
        for name, directives, markers in _PARITY_CASES:
            with self.subTest(case=name):
                self._check_case(name, directives, markers)

    def test_parity_under_an_env_override(self) -> None:
        project = self._make_project("env-override", '-- @envy cache-local "out/.envy"\n', ())
        env = self._sandbox_env(project)
        override = self._temp_dir / "explicit-cache"
        env["ENVY_CACHE_ROOT"] = str(override)

        root, user_wide = self._launcher_roots(project, env)
        self.assertEqual(self._binary_root(project, env).resolve(), root.resolve())

        # An explicit root names exactly one tree, so the launcher must not carry a
        # user-wide root at all -- an empty value is what turns the binary borrow off.
        # (`envy cache --user-wide-root` still answers here; it reports where shell hooks
        # live, which an override does move. The launcher never consults that.)
        self.assertEqual("", str(user_wide))

    def test_no_user_wide_root_without_home(self) -> None:
        """A HOME-less box must still run a project whose cache is inside its own tree.

        `set -u` used to abort the bash launcher here before the project got anywhere,
        because the platform default was expanded unconditionally.
        """
        project = self._make_project("homeless", '-- @envy cache-local "out/.envy"\n', ())
        env = self._sandbox_env(project)
        for var in ("HOME", "USERPROFILE", "XDG_CACHE_HOME", "LOCALAPPDATA"):
            env.pop(var, None)

        root, user_wide = self._launcher_roots(project, env)
        self.assertEqual((project / "out" / ".envy").resolve(), root.resolve())
        self.assertEqual("", str(user_wide))

    def test_parity_when_a_git_boundary_ends_the_walk(self) -> None:
        """discover() stops at a .git directory; the launchers used to walk straight past it.

        A `root "false"` project inside a checkout then resolved against the *parent* tree's
        manifest in the launcher and its own in the binary -- two different caches for one
        invocation, and the tier matrix above cannot reach it.
        """
        outer = self._temp_dir / "outer"
        inner = outer / "inner"
        inner.mkdir(parents=True)
        (outer / "envy.lua").write_text('-- @envy bin "tools"\nPACKAGES = {}\n')
        (inner / ".git").mkdir()
        (inner / "envy.lua").write_text(
            '-- @envy bin "tools"\n'
            '-- @envy root "false"\n'
            '-- @envy cache-local "out/.envy"\n'
            "PACKAGES = {}\n"
        )

        env = self._sandbox_env(inner)
        self.assertEqual(
            self._binary_root(inner, env).resolve(),
            self._launcher_root(inner, env).resolve(),
        )


@unittest.skipIf(sys.platform == "win32", "Bash tests skipped on Windows")
class TestBashCacheRootParity(_CacheRootParityMixin, EnvyTestCase):
    def setUp(self) -> None:
        super().setUp()
        self._script = self._temp_dir / "cache_root.sh"
        self._script.write_text(_get_bash_cache_root_script())
        self._script.chmod(0o755)

    def _launcher_roots(self, project: Path, env: dict) -> tuple[Path, str]:
        result = test_config.run(
            [str(self._script), str(project)],
            capture_output=True,
            text=True,
            env=env,
        )
        self.assertEqual(0, result.returncode, f"launcher: {result.stderr}")
        # Two lines: the project's cache root, then the user-wide one (blank if none).
        lines = result.stdout.split("\n")
        return Path(lines[0].strip()), lines[1].strip() if len(lines) > 1 else ""


@unittest.skipUnless(sys.platform == "win32", "Batch tests require Windows")
class TestBatchCacheRootParity(_CacheRootParityMixin, EnvyTestCase):
    def setUp(self) -> None:
        super().setUp()
        # write_text, not write_bytes: on Windows it emits CRLF, which is what cmd.exe
        # needs to resolve `call :quoted_value` and what stamp_bootstrap() writes. An
        # LF-only envy.bat silently parses no @envy directives at all.
        self._script = self._temp_dir / "cache_root.bat"
        self._script.write_text(_get_batch_cache_root_script())

    def _launcher_roots(self, project: Path, env: dict) -> tuple[Path, str]:
        result = test_config.run(
            ["cmd", "/c", str(self._script), str(project)],
            capture_output=True,
            text=True,
            env=env,
        )
        self.assertEqual(0, result.returncode, f"launcher: {result.stderr}")
        # Two lines: the project's cache root, then the user-wide one (blank if none).
        lines = result.stdout.splitlines()
        return Path(lines[0].strip()), lines[1].strip() if len(lines) > 1 else ""
