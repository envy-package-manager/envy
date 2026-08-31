"""Integration tests for envy shell hook behavior."""

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from . import test_config
from .env import EnvyTestCase

# --- where a manifest's header ends -------------------------------------------------------
#
# Every hook reads the header the same way parse_envy_meta does in src/manifest.cpp, and the
# same way src/resources/envy walks it: blank lines and comments, stopping at the first line
# of code, with no line cap. A hook that stops earlier or reads further puts a different
# project's bin directory on PATH than the envy it sits next to resolves. The four scripts
# used to cap at 20 lines and anchor `--` at column 0, so all three fixtures below were read
# wrong -- and in the same wrong way, which is why they are shared across the four classes.

_HEADER_PREAMBLE = "\n".join(
    f"-- preamble line {i}" if i % 5 else "" for i in range(1, 26)
)

# `root "false"` sitting under the first line of code is not a directive, so this manifest is
# its own project root and the walk must stop here rather than climb to the ancestor.
_ROOT_FALSE_BELOW_CODE = '-- @envy bin "tools"\nPACKAGES = {}\n-- @envy root "false"\n'

# Tab-indented and past the 20th line, so both directives still count and the walk continues
# upward. Tabs indent a header line exactly as spaces do.
_ROOT_FALSE_AFTER_PREAMBLE = (
    _HEADER_PREAMBLE + '\n\t-- @envy root "false"\n\t-- @envy bin "tools"\nPACKAGES = {}\n'
)

# The same preamble with `root` absent: this manifest is the root, and the `bin` beneath the
# preamble is the directory the hook has to put on PATH.
_BIN_AFTER_PREAMBLE = _HEADER_PREAMBLE + '\n\t-- @envy bin "tools"\nPACKAGES = {}\n'

# `bin` twice in one header. parse_envy_meta assigns per match, so the last wins, and the
# launchers' read loops overwrite the same way. Both directories exist in the fixture, so a
# hook that stops at the first hit puts a real directory on PATH -- just not the one the
# binary deploys into. Shared across the four classes because all four used to do exactly
# that.
_BIN_REPEATED_LAST_WINS = '-- @envy bin "stale"\n-- @envy bin "tools"\nPACKAGES = {}\n'


# A bin dir may sit at any depth under its manifest, so every hook joins '@envy bin' onto
# the manifest dir rather than assuming one component, and keeps walking past the
# intermediates to find that manifest in the first place.
_DEEP_BIN = "a/b/c/bin"


def _real(path) -> str:
    """A path in its physical spelling.

    The four hooks disagree about symlinks -- zsh resolves the bin dir with `:A`, bash and
    fish report the logical path -- so a raw string compare would pass or fail on macOS's
    /var -> /private/var alone rather than on the depth under test.
    """
    return str(Path(path).resolve())


def _path_entries(raw: str) -> list[str]:
    return [_real(e) for e in raw.strip().split(os.pathsep) if e]


def _write_deep_project(parent_dir: Path, name: str) -> tuple[Path, Path]:
    """A project whose bin dir sits four levels under its manifest."""
    project = parent_dir / name
    bin_dir = project / "a" / "b" / "c" / "bin"
    bin_dir.mkdir(parents=True, exist_ok=True)
    (project / "envy.lua").write_text(f'-- @envy bin "{_DEEP_BIN}"\nPACKAGES = {{}}\n')
    return project, bin_dir


def _write_project(parent_dir: Path, name: str, content: str) -> Path:
    """A project directory under `parent_dir` with a `tools/` bin dir and `content`."""
    project = parent_dir / name
    project.mkdir(parents=True, exist_ok=True)
    (project / "tools").mkdir(exist_ok=True)
    (project / "envy.lua").write_text(content)
    return project


@unittest.skipIf(sys.platform == "win32", "bash hook tests require Unix")
class TestBashHook(EnvyTestCase):
    """Test bash shell hook behavior by sourcing and invoking _envy_hook."""

    def setUp(self) -> None:
        self._temp_dir = self.make_temp_dir("_temp_dir")
        self._cache_dir = self._temp_dir / "cache"
        self._envy = test_config.get_envy_production_executable()

        # Trigger self-deploy to get hook files
        project_dir = self._temp_dir / "setup-project"
        bin_dir = self._temp_dir / "setup-bin"
        env = test_config.get_test_env()
        env["ENVY_CACHE_ROOT"] = str(self._cache_dir)
        test_config.run(
            [str(self._envy), "init", str(project_dir), str(bin_dir)],
            capture_output=True,
            text=True,
            env=env,
            timeout=30,
        )

        self._hook_path = self._cache_dir / "shell" / "hook.bash"
        assert self._hook_path.exists(), f"Hook not found at {self._hook_path}"

    def tearDown(self) -> None:
        if hasattr(self, "_temp_dir") and self._temp_dir.exists():
            shutil.rmtree(self._temp_dir, ignore_errors=True)

    @staticmethod
    def _hook_test_env() -> dict[str, str]:
        """Clean environment for hook tests — strip ENVY_SHELL_* to avoid leaking host config."""
        env = os.environ.copy()
        for key in list(env):
            if key.startswith("ENVY_SHELL_"):
                del env[key]
        return env

    def _run_bash_hook_test(self, script: str) -> subprocess.CompletedProcess[str]:
        """Run a bash script that sources the hook and tests behavior."""
        return test_config.run(
            ["bash", "-e", "-c", script],
            capture_output=True,
            text=True,
            timeout=10,
            env=self._hook_test_env(),
        )

    def _make_envy_project(self, name: str, bin_val: str = "tools") -> Path:
        """Create a minimal envy project directory."""
        project = self._temp_dir / name
        project.mkdir(parents=True, exist_ok=True)
        (project / "tools").mkdir(exist_ok=True)
        manifest = project / "envy.lua"
        manifest.write_text(f'-- @envy bin "{bin_val}"\nPACKAGES = {{}}\n')
        return project

    def test_cd_into_project_adds_bin_to_path(self) -> None:
        project = self._make_envy_project("proj1")
        result = self._run_bash_hook_test(
            f'source "{self._hook_path}"\n'
            f'cd "{project}"\n'
            f'_ENVY_LAST_PWD=""\n'  # force re-check
            f"_envy_hook\n"
            f'echo "$PATH"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn(str(project / "tools"), result.stdout)

    def test_cd_into_subdirectory_keeps_path(self) -> None:
        project = self._make_envy_project("proj2")
        sub = project / "src"
        sub.mkdir()
        result = self._run_bash_hook_test(
            f'source "{self._hook_path}"\n'
            f'cd "{project}"\n'
            f'_ENVY_LAST_PWD=""\n'
            f"_envy_hook\n"
            f'cd "{sub}"\n'
            f'_ENVY_LAST_PWD=""\n'
            f"_envy_hook\n"
            f'echo "$PATH"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn(str(project / "tools"), result.stdout)

    def test_cd_out_removes_bin_from_path(self) -> None:
        project = self._make_envy_project("proj3")
        result = self._run_bash_hook_test(
            f'source "{self._hook_path}"\n'
            f'cd "{project}"\n'
            f'_ENVY_LAST_PWD=""\n'
            f"_envy_hook\n"
            f"cd /tmp\n"
            f'_ENVY_LAST_PWD=""\n'
            f"_envy_hook\n"
            f'echo "$PATH"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertNotIn(str(project / "tools"), result.stdout)

    def test_cd_between_projects_swaps_path(self) -> None:
        proj_a = self._make_envy_project("projA", "tools")
        proj_b = self._make_envy_project("projB", "tools")
        result = self._run_bash_hook_test(
            f'source "{self._hook_path}"\n'
            f'cd "{proj_a}"\n'
            f'_ENVY_LAST_PWD=""\n'
            f"_envy_hook\n"
            f'cd "{proj_b}"\n'
            f'_ENVY_LAST_PWD=""\n'
            f"_envy_hook\n"
            f'echo "$PATH"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn(str(proj_b / "tools"), result.stdout)
        self.assertNotIn(str(proj_a / "tools"), result.stdout)

    def test_envy_project_root_set(self) -> None:
        project = self._make_envy_project("proj4")
        result = self._run_bash_hook_test(
            f'source "{self._hook_path}"\n'
            f'cd "{project}"\n'
            f'_ENVY_LAST_PWD=""\n'
            f"_envy_hook\n"
            f'echo "$ENVY_PROJECT_ROOT"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(str(project), result.stdout.strip())

    def test_envy_project_root_unset_when_leaving(self) -> None:
        project = self._make_envy_project("proj5")
        result = self._run_bash_hook_test(
            f'source "{self._hook_path}"\n'
            f'cd "{project}"\n'
            f'_ENVY_LAST_PWD=""\n'
            f"_envy_hook\n"
            f"cd /tmp\n"
            f'_ENVY_LAST_PWD=""\n'
            f"_envy_hook\n"
            f'echo "root=${{ENVY_PROJECT_ROOT:-unset}}"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual("root=unset", result.stdout.strip())

    def test_missing_bin_directive_no_path_change(self) -> None:
        project = self._temp_dir / "no-bin"
        project.mkdir(parents=True)
        (project / "envy.lua").write_text("PACKAGES = {}\n")
        original_path = "/usr/bin:/bin"
        result = self._run_bash_hook_test(
            f'export PATH="{original_path}"\n'
            f'source "{self._hook_path}"\n'
            f'cd "{project}"\n'
            f'_ENVY_LAST_PWD=""\n'
            f"_envy_hook\n"
            f'echo "$PATH"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(original_path, result.stdout.strip())

    def test_disable_env_var(self) -> None:
        project = self._make_envy_project("proj-disable")
        result = self._run_bash_hook_test(
            f"export ENVY_SHELL_HOOK_DISABLE=1\n"
            f'export PATH="/usr/bin:/bin"\n'
            f'source "{self._hook_path}"\n'
            f'cd "{project}"\n'
            f'_ENVY_LAST_PWD=""\n'
            f"_envy_hook\n"
            f'echo "$PATH"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertNotIn(str(project / "tools"), result.stdout)

    def test_nested_root_false_finds_ancestor(self) -> None:
        parent = self._make_envy_project("parent-proj")
        child = parent / "sub"
        child.mkdir(parents=True, exist_ok=True)
        (child / "tools").mkdir(exist_ok=True)
        (child / "envy.lua").write_text(
            '-- @envy root "false"\n-- @envy bin "tools"\nPACKAGES = {}\n'
        )
        result = self._run_bash_hook_test(
            f'source "{self._hook_path}"\n'
            f'cd "{child}"\n'
            f'_ENVY_LAST_PWD=""\n'
            f"_envy_hook\n"
            f'echo "root=$ENVY_PROJECT_ROOT"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        # Should find the parent project (which is root=true by default)
        self.assertIn(str(parent), result.stdout)

    # --- where a manifest's header ends ---
    #
    # Compared exactly, not with assertIn: the child's path has the parent's as a prefix, so
    # a containment check passes for either answer.

    def test_root_false_below_the_first_code_line_is_not_a_directive(self) -> None:
        parent = self._make_envy_project("hdr-below-parent")
        child = _write_project(parent, "sub", _ROOT_FALSE_BELOW_CODE)
        result = self._run_bash_hook_test(
            f'source "{self._hook_path}"\n'
            f'cd "{child}"\n'
            f'_ENVY_LAST_PWD=""\n'
            f"_envy_hook\n"
            f'echo "$ENVY_PROJECT_ROOT"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(str(child), result.stdout.strip())

    def test_root_false_under_a_long_preamble_is_still_a_directive(self) -> None:
        parent = self._make_envy_project("hdr-preamble-parent")
        child = _write_project(parent, "sub", _ROOT_FALSE_AFTER_PREAMBLE)
        result = self._run_bash_hook_test(
            f'source "{self._hook_path}"\n'
            f'cd "{child}"\n'
            f'_ENVY_LAST_PWD=""\n'
            f"_envy_hook\n"
            f'echo "$ENVY_PROJECT_ROOT"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(str(parent), result.stdout.strip())

    def test_bin_under_a_long_preamble_reaches_path(self) -> None:
        project = _write_project(self._temp_dir, "hdr-bin-preamble", _BIN_AFTER_PREAMBLE)
        result = self._run_bash_hook_test(
            f'source "{self._hook_path}"\n'
            f'cd "{project}"\n'
            f'_ENVY_LAST_PWD=""\n'
            f"_envy_hook\n"
            f'echo "$PATH"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn(str(project / "tools"), result.stdout)

    def test_repeated_bin_directive_takes_the_last(self) -> None:
        project = _write_project(self._temp_dir, "hdr-bin-dup", _BIN_REPEATED_LAST_WINS)
        (project / "stale").mkdir(exist_ok=True)
        result = self._run_bash_hook_test(
            f'source "{self._hook_path}"\n'
            f'cd "{project}"\n'
            f'_ENVY_LAST_PWD=""\n'
            f"_envy_hook\n"
            f'echo "$PATH"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn(str(project / "tools"), result.stdout)
        self.assertNotIn(str(project / "stale"), result.stdout)

    def test_bin_dir_with_spaces(self) -> None:
        project = self._temp_dir / "space proj"
        project.mkdir(parents=True, exist_ok=True)
        (project / "my tools").mkdir(exist_ok=True)
        (project / "envy.lua").write_text('-- @envy bin "my tools"\nPACKAGES = {}\n')
        result = self._run_bash_hook_test(
            f'source "{self._hook_path}"\n'
            f'cd "{project}"\n'
            f'_ENVY_LAST_PWD=""\n'
            f"_envy_hook\n"
            f'echo "$PATH"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn(str(project / "my tools"), result.stdout)

    def test_deep_bin_dir_reaches_path(self) -> None:
        project, bin_dir = _write_deep_project(self._temp_dir, "bash-deep")
        result = self._run_bash_hook_test(
            f'source "{self._hook_path}"\n'
            f'cd "{project}"\n'
            f'_ENVY_LAST_PWD=""\n'
            f"_envy_hook\n"
            f'echo "$PATH"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(_real(bin_dir), _path_entries(result.stdout)[0])

    def test_deep_bin_dir_project_root_is_the_manifest_dir(self) -> None:
        project, _ = _write_deep_project(self._temp_dir, "bash-deep-root")
        result = self._run_bash_hook_test(
            f'source "{self._hook_path}"\n'
            f'cd "{project}"\n'
            f'_ENVY_LAST_PWD=""\n'
            f"_envy_hook\n"
            f'echo "$ENVY_PROJECT_ROOT"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(_real(project), _real(result.stdout.strip()))

    def test_an_intermediate_of_a_deep_bin_dir_is_not_a_project(self) -> None:
        """`a/b` holds no envy.lua, so the walk climbs past it to the real manifest."""
        project, bin_dir = _write_deep_project(self._temp_dir, "bash-deep-mid")
        result = self._run_bash_hook_test(
            f'source "{self._hook_path}"\n'
            f'cd "{project / "a" / "b"}"\n'
            f'_ENVY_LAST_PWD=""\n'
            f"_envy_hook\n"
            f'echo "$ENVY_PROJECT_ROOT"\n'
            f'echo "$PATH"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        root, path = result.stdout.strip().split("\n")
        self.assertEqual(_real(project), _real(root))
        self.assertEqual(_real(bin_dir), _path_entries(path)[0])

    def test_deep_bin_dir_leaves_path_on_the_way_out(self) -> None:
        project, bin_dir = _write_deep_project(self._temp_dir, "bash-deep-out")
        result = self._run_bash_hook_test(
            f'source "{self._hook_path}"\n'
            f'cd "{project}"\n'
            f'_ENVY_LAST_PWD=""\n'
            f"_envy_hook\n"
            f'cd "{self._temp_dir}"\n'
            f'_ENVY_LAST_PWD=""\n'
            f"_envy_hook\n"
            f'echo "$PATH"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertNotIn(_real(bin_dir), _path_entries(result.stdout))

    def test_existing_path_entries_preserved(self) -> None:
        project = self._make_envy_project("proj-path")
        result = self._run_bash_hook_test(
            f'export PATH="/usr/local/bin:/usr/bin:/bin"\n'
            f'source "{self._hook_path}"\n'
            f'cd "{project}"\n'
            f'_ENVY_LAST_PWD=""\n'
            f"_envy_hook\n"
            f'echo "$PATH"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn("/usr/local/bin", result.stdout)
        self.assertIn("/usr/bin", result.stdout)
        self.assertIn("/bin", result.stdout)
        self.assertIn(str(project / "tools"), result.stdout)

    def test_initial_activation_inside_project(self) -> None:
        """Sourcing hook while already in a project activates immediately."""
        project = self._make_envy_project("proj-init")
        result = self._run_bash_hook_test(
            f'cd "{project}"\nsource "{self._hook_path}"\necho "$PATH"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn(str(project / "tools"), result.stdout)

    # --- v2: enter/leave messages ---

    def test_entering_message_on_cd_into_project(self) -> None:
        project = self._make_envy_project("proj-msg-enter")
        result = self._run_bash_hook_test(
            f'source "{self._hook_path}"\n'
            f'cd "{project}"\n'
            f'_ENVY_LAST_PWD=""\n'
            f"_envy_hook\n"
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn("envy: entering proj-msg-enter", result.stderr)
        self.assertIn("tools added to PATH", result.stderr)

    def test_leaving_message_on_cd_out(self) -> None:
        project = self._make_envy_project("proj-msg-leave")
        result = self._run_bash_hook_test(
            f'source "{self._hook_path}"\n'
            f'cd "{project}"\n'
            f'_ENVY_LAST_PWD=""\n'
            f"_envy_hook\n"
            f"cd /tmp\n"
            f'_ENVY_LAST_PWD=""\n'
            f"_envy_hook\n"
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn("envy: leaving proj-msg-leave", result.stderr)
        self.assertIn("PATH restored", result.stderr)

    def test_switching_projects_prints_leave_and_enter(self) -> None:
        proj_a = self._make_envy_project("proj-sw-A")
        proj_b = self._make_envy_project("proj-sw-B")
        result = self._run_bash_hook_test(
            f'source "{self._hook_path}"\n'
            f'cd "{proj_a}"\n'
            f'_ENVY_LAST_PWD=""\n'
            f"_envy_hook\n"
            f'cd "{proj_b}"\n'
            f'_ENVY_LAST_PWD=""\n'
            f"_envy_hook\n"
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        leave_pos = result.stderr.index("leaving proj-sw-A")
        enter_pos = result.stderr.index("entering proj-sw-B")
        self.assertLess(leave_pos, enter_pos)

    def test_no_duplicate_message_within_project(self) -> None:
        project = self._make_envy_project("proj-nodup")
        sub = project / "src"
        sub.mkdir()
        result = self._run_bash_hook_test(
            f'source "{self._hook_path}"\n'
            f'cd "{project}"\n'
            f'_ENVY_LAST_PWD=""\n'
            f"_envy_hook\n"
            f'cd "{sub}"\n'
            f'_ENVY_LAST_PWD=""\n'
            f"_envy_hook\n"
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(1, result.stderr.count("entering proj-nodup"))

    def test_initial_activation_prints_message(self) -> None:
        project = self._make_envy_project("proj-init-msg")
        result = self._run_bash_hook_test(
            f'cd "{project}"\nsource "{self._hook_path}"\n'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn("envy: entering proj-init-msg", result.stderr)

    # --- v2: raccoon prompt ---

    def test_raccoon_in_prompt_when_in_project(self) -> None:
        project = self._make_envy_project("proj-raccoon")
        result = self._run_bash_hook_test(
            f'PS1="$ "\n'
            f'source "{self._hook_path}"\n'
            f'cd "{project}"\n'
            f'_ENVY_LAST_PWD=""\n'
            f"_envy_hook\n"
            f'echo "$PS1"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn("\U0001f99d", result.stdout)

    def test_raccoon_removed_on_leave(self) -> None:
        project = self._make_envy_project("proj-raccoon-rm")
        result = self._run_bash_hook_test(
            f'PS1="$ "\n'
            f'source "{self._hook_path}"\n'
            f'cd "{project}"\n'
            f'_ENVY_LAST_PWD=""\n'
            f"_envy_hook\n"
            f"cd /tmp\n"
            f'_ENVY_LAST_PWD=""\n'
            f"_envy_hook\n"
            f'echo "$PS1"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertNotIn("\U0001f99d", result.stdout)
        self.assertIn("$", result.stdout)

    def test_raccoon_disabled_via_env_var(self) -> None:
        project = self._make_envy_project("proj-raccoon-off")
        result = self._run_bash_hook_test(
            f"export ENVY_SHELL_NO_ICON=1\n"
            f'PS1="$ "\n'
            f'source "{self._hook_path}"\n'
            f'cd "{project}"\n'
            f'_ENVY_LAST_PWD=""\n'
            f"_envy_hook\n"
            f'echo "$PS1"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertNotIn("\U0001f99d", result.stdout)
        self.assertIn("$", result.stdout)

    def test_non_utf8_locale_no_raccoon_and_ascii_dash(self) -> None:
        project = self._make_envy_project("proj-c-locale")
        result = self._run_bash_hook_test(
            f"export LANG=C\n"
            f"unset LC_ALL LC_CTYPE\n"
            f'PS1="$ "\n'
            f'source "{self._hook_path}"\n'
            f'cd "{project}"\n'
            f'_ENVY_LAST_PWD=""\n'
            f"_envy_hook\n"
            f'echo "$PS1"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertNotIn("\U0001f99d", result.stdout)
        self.assertIn("--", result.stderr)
        self.assertNotIn("\u2014", result.stderr)

    # --- v5: display control env vars ---

    def test_messages_suppressed_via_env_var(self) -> None:
        """ENVY_SHELL_NO_ENTER_EXIT_ANNOUNCE=1 suppresses enter/leave text."""
        project = self._make_envy_project("proj-no-announce")
        result = self._run_bash_hook_test(
            f"export ENVY_SHELL_NO_ENTER_EXIT_ANNOUNCE=1\n"
            f'PS1="$ "\n'
            f'source "{self._hook_path}"\n'
            f'cd "{project}"\n'
            f'_ENVY_LAST_PWD=""\n'
            f"_envy_hook\n"
            f'echo "$PATH"\n'
            f'echo "$PS1"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        # PATH still modified
        self.assertIn(str(project / "tools"), result.stdout)
        # Raccoon still shown
        self.assertIn("\U0001f99d", result.stdout)
        # No entering/leaving messages
        self.assertNotIn("envy: entering", result.stderr)

    def test_messages_suppressed_but_icon_shown(self) -> None:
        """Messages suppressed, icon still shown — vars are independent."""
        project = self._make_envy_project("proj-ann-no-icon-yes")
        result = self._run_bash_hook_test(
            f"export ENVY_SHELL_NO_ENTER_EXIT_ANNOUNCE=1\n"
            f'PS1="$ "\n'
            f'source "{self._hook_path}"\n'
            f'cd "{project}"\n'
            f'_ENVY_LAST_PWD=""\n'
            f"_envy_hook\n"
            f'echo "$PS1"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertNotIn("envy: entering", result.stderr)
        self.assertIn("\U0001f99d", result.stdout)

    def test_icon_suppressed_but_messages_shown(self) -> None:
        """Icon suppressed, messages still shown — vars are independent."""
        project = self._make_envy_project("proj-icon-no-ann-yes")
        result = self._run_bash_hook_test(
            f"export ENVY_SHELL_NO_ICON=1\n"
            f'PS1="$ "\n'
            f'source "{self._hook_path}"\n'
            f'cd "{project}"\n'
            f'_ENVY_LAST_PWD=""\n'
            f"_envy_hook\n"
            f'echo "$PS1"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn("envy: entering", result.stderr)
        self.assertNotIn("\U0001f99d", result.stdout)


@unittest.skipUnless(shutil.which("zsh"), "zsh not installed")
@unittest.skipIf(sys.platform == "win32", "zsh hook tests require Unix")
class TestZshHook(EnvyTestCase):
    """Test zsh shell hook behavior."""

    def setUp(self) -> None:
        self._temp_dir = self.make_temp_dir("_temp_dir")
        self._cache_dir = self._temp_dir / "cache"
        self._envy = test_config.get_envy_production_executable()

        project_dir = self._temp_dir / "setup-project"
        bin_dir = self._temp_dir / "setup-bin"
        env = test_config.get_test_env()
        env["ENVY_CACHE_ROOT"] = str(self._cache_dir)
        test_config.run(
            [str(self._envy), "init", str(project_dir), str(bin_dir)],
            capture_output=True,
            text=True,
            env=env,
            timeout=30,
        )
        self._hook_path = self._cache_dir / "shell" / "hook.zsh"

    def tearDown(self) -> None:
        if hasattr(self, "_temp_dir") and self._temp_dir.exists():
            shutil.rmtree(self._temp_dir, ignore_errors=True)

    def _make_envy_project(self, name: str) -> Path:
        project = self._temp_dir / name
        project.mkdir(parents=True, exist_ok=True)
        (project / "tools").mkdir(exist_ok=True)
        (project / "envy.lua").write_text('-- @envy bin "tools"\nPACKAGES = {}\n')
        return project

    @staticmethod
    def _hook_test_env() -> dict[str, str]:
        """Clean environment for hook tests — strip ENVY_SHELL_* to avoid leaking host config."""
        env = os.environ.copy()
        for key in list(env):
            if key.startswith("ENVY_SHELL_"):
                del env[key]
        return env

    def _run_zsh_hook_test(self, script: str) -> subprocess.CompletedProcess[str]:
        """Run a zsh script with -f (skip RC files) to avoid CI hangs."""
        return test_config.run(
            ["zsh", "-f", "-c", script],
            capture_output=True,
            text=True,
            timeout=30,
            env=self._hook_test_env(),
        )

    def test_cd_into_project_adds_bin_to_path(self) -> None:
        project = self._make_envy_project("zsh-proj1")
        result = self._run_zsh_hook_test(
            f'source "{self._hook_path}"\ncd "{project}"\necho "$PATH"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn(str(project / "tools"), result.stdout)

    def test_cd_out_removes_path(self) -> None:
        project = self._make_envy_project("zsh-proj2")
        result = self._run_zsh_hook_test(
            f'source "{self._hook_path}"\ncd "{project}"\ncd /tmp\necho "$PATH"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertNotIn(str(project / "tools"), result.stdout)

    def test_cd_between_projects_swaps_path(self) -> None:
        proj_a = self._make_envy_project("zsh-projA")
        proj_b = self._make_envy_project("zsh-projB")
        result = self._run_zsh_hook_test(
            f'source "{self._hook_path}"\ncd "{proj_a}"\ncd "{proj_b}"\necho "$PATH"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn(str(proj_b / "tools"), result.stdout)
        self.assertNotIn(str(proj_a / "tools"), result.stdout)

    def test_envy_project_root_set(self) -> None:
        project = self._make_envy_project("zsh-proj-root")
        result = self._run_zsh_hook_test(
            f'source "{self._hook_path}"\ncd "{project}"\necho "$ENVY_PROJECT_ROOT"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(str(project), result.stdout.strip())

    def test_deep_bin_dir_reaches_path(self) -> None:
        project, bin_dir = _write_deep_project(self._temp_dir, "zsh-deep")
        result = self._run_zsh_hook_test(
            f'source "{self._hook_path}"\ncd "{project}"\necho "$PATH"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(_real(bin_dir), _path_entries(result.stdout)[0])

    def test_deep_bin_dir_project_root_is_the_manifest_dir(self) -> None:
        project, _ = _write_deep_project(self._temp_dir, "zsh-deep-root")
        result = self._run_zsh_hook_test(
            f'source "{self._hook_path}"\ncd "{project}"\necho "$ENVY_PROJECT_ROOT"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(_real(project), _real(result.stdout.strip()))

    def test_an_intermediate_of_a_deep_bin_dir_is_not_a_project(self) -> None:
        project, bin_dir = _write_deep_project(self._temp_dir, "zsh-deep-mid")
        result = self._run_zsh_hook_test(
            f'source "{self._hook_path}"\ncd "{project / "a" / "b"}"\n'
            f'echo "$ENVY_PROJECT_ROOT"\necho "$PATH"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        root, path = result.stdout.strip().split("\n")
        self.assertEqual(_real(project), _real(root))
        self.assertEqual(_real(bin_dir), _path_entries(path)[0])

    def test_deep_bin_dir_leaves_path_on_the_way_out(self) -> None:
        project, bin_dir = _write_deep_project(self._temp_dir, "zsh-deep-out")
        result = self._run_zsh_hook_test(
            f'source "{self._hook_path}"\ncd "{project}"\n'
            f'cd "{self._temp_dir}"\necho "$PATH"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertNotIn(_real(bin_dir), _path_entries(result.stdout))

    # --- where a manifest's header ends ---

    def test_root_false_below_the_first_code_line_is_not_a_directive(self) -> None:
        parent = self._make_envy_project("zsh-hdr-below")
        child = _write_project(parent, "sub", _ROOT_FALSE_BELOW_CODE)
        result = self._run_zsh_hook_test(
            f'source "{self._hook_path}"\ncd "{child}"\necho "$ENVY_PROJECT_ROOT"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(str(child), result.stdout.strip())

    def test_root_false_under_a_long_preamble_is_still_a_directive(self) -> None:
        parent = self._make_envy_project("zsh-hdr-preamble")
        child = _write_project(parent, "sub", _ROOT_FALSE_AFTER_PREAMBLE)
        result = self._run_zsh_hook_test(
            f'source "{self._hook_path}"\ncd "{child}"\necho "$ENVY_PROJECT_ROOT"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(str(parent), result.stdout.strip())

    def test_bin_under_a_long_preamble_reaches_path(self) -> None:
        project = _write_project(self._temp_dir, "zsh-hdr-bin", _BIN_AFTER_PREAMBLE)
        result = self._run_zsh_hook_test(
            f'source "{self._hook_path}"\ncd "{project}"\necho "$PATH"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn(str(project / "tools"), result.stdout)

    def test_repeated_bin_directive_takes_the_last(self) -> None:
        project = _write_project(self._temp_dir, "zsh-hdr-dup", _BIN_REPEATED_LAST_WINS)
        (project / "stale").mkdir(exist_ok=True)
        result = self._run_zsh_hook_test(
            f'source "{self._hook_path}"\ncd "{project}"\necho "$PATH"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn(str(project / "tools"), result.stdout)
        self.assertNotIn(str(project / "stale"), result.stdout)

    # --- v2: enter/leave messages ---

    def test_entering_message_on_cd_into_project(self) -> None:
        project = self._make_envy_project("zsh-msg-enter")
        result = self._run_zsh_hook_test(
            f'source "{self._hook_path}"\ncd "{project}"\n'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn("envy: entering zsh-msg-enter", result.stderr)
        self.assertIn("tools added to PATH", result.stderr)

    def test_leaving_message_on_cd_out(self) -> None:
        project = self._make_envy_project("zsh-msg-leave")
        result = self._run_zsh_hook_test(
            f'source "{self._hook_path}"\ncd "{project}"\ncd /tmp\n'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn("envy: leaving zsh-msg-leave", result.stderr)
        self.assertIn("PATH restored", result.stderr)

    def test_switching_projects_prints_leave_and_enter(self) -> None:
        proj_a = self._make_envy_project("zsh-sw-A")
        proj_b = self._make_envy_project("zsh-sw-B")
        result = self._run_zsh_hook_test(
            f'source "{self._hook_path}"\ncd "{proj_a}"\ncd "{proj_b}"\n'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        leave_pos = result.stderr.index("leaving zsh-sw-A")
        enter_pos = result.stderr.index("entering zsh-sw-B")
        self.assertLess(leave_pos, enter_pos)

    def test_no_duplicate_message_within_project(self) -> None:
        project = self._make_envy_project("zsh-nodup")
        sub = project / "src"
        sub.mkdir()
        result = self._run_zsh_hook_test(
            f'source "{self._hook_path}"\ncd "{project}"\ncd "{sub}"\n'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(1, result.stderr.count("entering zsh-nodup"))

    def test_initial_activation_prints_message(self) -> None:
        project = self._make_envy_project("zsh-init-msg")
        result = self._run_zsh_hook_test(
            f'cd "{project}"\nsource "{self._hook_path}"\n'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn("envy: entering zsh-init-msg", result.stderr)

    # --- v2: raccoon prompt ---

    def test_raccoon_in_prompt_when_in_project(self) -> None:
        project = self._make_envy_project("zsh-raccoon")
        result = self._run_zsh_hook_test(
            f'PROMPT="$ "\nsource "{self._hook_path}"\ncd "{project}"\necho "$PROMPT"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn("\U0001f99d", result.stdout)

    def test_raccoon_removed_on_leave(self) -> None:
        project = self._make_envy_project("zsh-raccoon-rm")
        result = self._run_zsh_hook_test(
            f'PROMPT="$ "\n'
            f'source "{self._hook_path}"\ncd "{project}"\ncd /tmp\n'
            f'echo "$PROMPT"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertNotIn("\U0001f99d", result.stdout)
        self.assertIn("$", result.stdout)

    def test_raccoon_disabled_via_env_var(self) -> None:
        project = self._make_envy_project("zsh-raccoon-off")
        result = self._run_zsh_hook_test(
            f"export ENVY_SHELL_NO_ICON=1\n"
            f'PROMPT="$ "\n'
            f'source "{self._hook_path}"\ncd "{project}"\n'
            f'echo "$PROMPT"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertNotIn("\U0001f99d", result.stdout)
        self.assertIn("$", result.stdout)

    def test_raccoon_prompt_when_switching_projects(self) -> None:
        proj_a = self._make_envy_project("zsh-raccoon-sw-A")
        proj_b = self._make_envy_project("zsh-raccoon-sw-B")
        result = self._run_zsh_hook_test(
            f'PROMPT="$ "\n'
            f'source "{self._hook_path}"\n'
            f'cd "{proj_a}"\ncd "{proj_b}"\n'
            f'echo "$PROMPT"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn("\U0001f99d", result.stdout)

    def test_envy_project_root_updated_on_switch(self) -> None:
        proj_a = self._make_envy_project("zsh-root-sw-A")
        proj_b = self._make_envy_project("zsh-root-sw-B")
        result = self._run_zsh_hook_test(
            f'source "{self._hook_path}"\n'
            f'cd "{proj_a}"\ncd "{proj_b}"\n'
            f'echo "$ENVY_PROJECT_ROOT"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(str(proj_b), result.stdout.strip())

    def test_raccoon_survives_theme_overwrite(self) -> None:
        """Raccoon is re-applied when a theme's precmd overwrites PROMPT."""
        project = self._make_envy_project("zsh-raccoon-theme")
        result = self._run_zsh_hook_test(
            # Register a fake theme precmd that rebuilds PROMPT (like OMZ themes)
            f'_fake_theme_precmd() {{ PROMPT="THEME> " }}\n'
            f"precmd_functions+=(_fake_theme_precmd)\n"
            # Source envy hook (appends _envy_precmd after fake theme)
            f'source "{self._hook_path}"\n'
            f'cd "{project}"\n'
            # Simulate a precmd cycle (non-interactive zsh doesn't auto-fire)
            f'for f in "${{precmd_functions[@]}}"; do "$f"; done\n'
            f'echo "$PROMPT"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn("\U0001f99d", result.stdout)
        self.assertIn("THEME>", result.stdout)

    # Stand-in for iTerm2/VS Code shell integration: re-prepend a prompt mark whenever PROMPT
    # differs from the copy it decorated last. Racing the hook's own precmd is the point.
    _REDECORATOR = (
        "_fake_mark_precmd() {\n"
        '  [[ "$_fake_mark_ps1" == "$PROMPT" ]] && return\n'
        '  PROMPT="%{MARK%}$PROMPT"\n'
        '  _fake_mark_ps1="$PROMPT"\n'
        "}\n"
        "precmd_functions+=(_fake_mark_precmd)\n"
    )

    # Re-expands per cycle on purpose: _envy_precmd reorders itself to the end of the array.
    _PRECMD_CYCLES = 'repeat 5 { for f in $precmd_functions; do "$f"; done }\n'

    def test_raccoon_not_multiplied_by_a_prompt_redecorator(self) -> None:
        """Exactly one raccoon, however often another precmd re-marks PROMPT."""
        project = self._make_envy_project("zsh-raccoon-redecorate")
        result = self._run_zsh_hook_test(
            self._REDECORATOR + f'PROMPT="$ "\n'
            f'source "{self._hook_path}"\n'
            f'cd "{project}"\n' + self._PRECMD_CYCLES + 'echo "$PROMPT"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(1, result.stdout.count("\U0001f99d"))

    def test_leaving_clears_a_stack_of_raccoons(self) -> None:
        """A PROMPT already carrying icons from an older hook leaves clean, not one short."""
        project = self._make_envy_project("zsh-raccoon-stacked")
        result = self._run_zsh_hook_test(
            f'PROMPT="%{{\U0001f99d%2G%}} %{{\U0001f99d%2G%}} $ "\n'
            f'source "{self._hook_path}"\n'
            f'cd "{project}"\ncd /tmp\n'
            f'echo "$PROMPT"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertNotIn("\U0001f99d", result.stdout)
        self.assertIn("$", result.stdout)

    def test_raccoon_removed_on_leave_when_a_mark_sits_ahead_of_it(self) -> None:
        """Leaving strips the icon even when another decorator's mark precedes it."""
        project = self._make_envy_project("zsh-raccoon-redecorate-rm")
        result = self._run_zsh_hook_test(
            self._REDECORATOR + f'PROMPT="$ "\n'
            f'source "{self._hook_path}"\n'
            f'cd "{project}"\n' + self._PRECMD_CYCLES + "cd /tmp\n"
            + self._PRECMD_CYCLES + 'echo "$PROMPT"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertNotIn("\U0001f99d", result.stdout)
        self.assertIn("$", result.stdout)

    # --- p10k integration ---

    def test_p10k_segment_registered(self) -> None:
        """When p10k is detected, envy auto-registers its segment."""
        project = self._make_envy_project("zsh-p10k-reg")
        result = self._run_zsh_hook_test(
            # Minimal p10k mock: define p10k function and elements array
            f"p10k() {{ : }}\n"
            f"typeset -ga POWERLEVEL9K_LEFT_PROMPT_ELEMENTS=(os_icon dir vcs)\n"
            f'source "{self._hook_path}"\n'
            f'cd "{project}"\n'
            f'echo "elements=${{POWERLEVEL9K_LEFT_PROMPT_ELEMENTS[*]}}"\n'
            f'echo "active=$_ENVY_PROMPT_ACTIVE"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        elements = result.stdout.split("elements=")[1].split("\n")[0]
        self.assertIn("envy", elements)
        self.assertTrue(elements.startswith("envy "), "envy should be first element")
        self.assertIn("active=1", result.stdout)

    def test_p10k_no_prompt_modification(self) -> None:
        """With p10k present, PROMPT is not modified by envy."""
        project = self._make_envy_project("zsh-p10k-noprompt")
        result = self._run_zsh_hook_test(
            f'PROMPT="$ "\n'
            f"p10k() {{ : }}\n"
            f"typeset -ga POWERLEVEL9K_LEFT_PROMPT_ELEMENTS=(dir)\n"
            f'source "{self._hook_path}"\n'
            f'cd "{project}"\n'
            f'echo "$PROMPT"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertNotIn("\U0001f99d", result.stdout)
        self.assertEqual("$", result.stdout.strip())

    def test_p10k_segment_function_renders(self) -> None:
        """prompt_envy() calls p10k segment with raccoon when active."""
        project = self._make_envy_project("zsh-p10k-render")
        result = self._run_zsh_hook_test(
            # p10k mock that echoes -t value
            f"p10k() {{\n"
            f'  if [ "$1" = "segment" ]; then\n'
            f"    shift\n"
            f"    while [ $# -gt 0 ]; do\n"
            f'      case "$1" in -t) echo "$2"; return ;; *) shift ;; esac\n'
            f"    done\n"
            f"  fi\n"
            f"}}\n"
            f"typeset -ga POWERLEVEL9K_LEFT_PROMPT_ELEMENTS=(dir)\n"
            f'source "{self._hook_path}"\n'
            f'cd "{project}"\n'
            f"prompt_envy"
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn("\U0001f99d", result.stdout)

    def test_p10k_segment_hidden_when_inactive(self) -> None:
        """prompt_envy() produces no output when not in a project."""
        result = self._run_zsh_hook_test(
            # Mock that only echoes for segment calls (reload is silent)
            f'p10k() {{ [ "$1" = "segment" ] && echo "SHOULD_NOT_APPEAR" }}\n'
            f"typeset -ga POWERLEVEL9K_LEFT_PROMPT_ELEMENTS=(dir)\n"
            f'source "{self._hook_path}"\n'
            f"prompt_envy\n"
            f'echo "done"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertNotIn("SHOULD_NOT_APPEAR", result.stdout)
        self.assertIn("done", result.stdout)

    def test_p10k_segment_not_registered_when_prompt_disabled(self) -> None:
        """ENVY_SHELL_NO_ICON=1 prevents p10k segment registration."""
        result = self._run_zsh_hook_test(
            f"export ENVY_SHELL_NO_ICON=1\n"
            f"p10k() {{ : }}\n"
            f"typeset -ga POWERLEVEL9K_LEFT_PROMPT_ELEMENTS=(os_icon dir vcs)\n"
            f'source "{self._hook_path}"\n'
            f'echo "elements=${{POWERLEVEL9K_LEFT_PROMPT_ELEMENTS[*]}}"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        elements = result.stdout.split("elements=")[1].split("\n")[0]
        self.assertNotIn("envy", elements)

    # --- v5: display control env vars ---

    def test_messages_suppressed_via_env_var(self) -> None:
        """ENVY_SHELL_NO_ENTER_EXIT_ANNOUNCE=1 suppresses enter/leave text."""
        project = self._make_envy_project("zsh-no-announce")
        result = self._run_zsh_hook_test(
            f"export ENVY_SHELL_NO_ENTER_EXIT_ANNOUNCE=1\n"
            f'PROMPT="$ "\n'
            f'source "{self._hook_path}"\ncd "{project}"\n'
            f'echo "$PATH"\necho "$PROMPT"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn(str(project / "tools"), result.stdout)
        self.assertIn("\U0001f99d", result.stdout)
        self.assertNotIn("envy: entering", result.stderr)

    def test_messages_suppressed_but_icon_shown(self) -> None:
        project = self._make_envy_project("zsh-ann-no-icon-yes")
        result = self._run_zsh_hook_test(
            f"export ENVY_SHELL_NO_ENTER_EXIT_ANNOUNCE=1\n"
            f'PROMPT="$ "\n'
            f'source "{self._hook_path}"\ncd "{project}"\n'
            f'echo "$PROMPT"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertNotIn("envy: entering", result.stderr)
        self.assertIn("\U0001f99d", result.stdout)

    def test_icon_suppressed_but_messages_shown(self) -> None:
        project = self._make_envy_project("zsh-icon-no-ann-yes")
        result = self._run_zsh_hook_test(
            f"export ENVY_SHELL_NO_ICON=1\n"
            f'PROMPT="$ "\n'
            f'source "{self._hook_path}"\ncd "{project}"\n'
            f'echo "$PROMPT"'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn("envy: entering", result.stderr)
        self.assertNotIn("\U0001f99d", result.stdout)


@unittest.skipUnless(shutil.which("fish"), "fish not installed")
@unittest.skipIf(sys.platform == "win32", "fish hook tests require Unix")
class TestFishHook(EnvyTestCase):
    """Test fish shell hook behavior."""

    def setUp(self) -> None:
        self._temp_dir = self.make_temp_dir("_temp_dir")
        self._cache_dir = self._temp_dir / "cache"
        self._envy = test_config.get_envy_production_executable()

        project_dir = self._temp_dir / "setup-project"
        bin_dir = self._temp_dir / "setup-bin"
        env = test_config.get_test_env()
        env["ENVY_CACHE_ROOT"] = str(self._cache_dir)
        test_config.run(
            [str(self._envy), "init", str(project_dir), str(bin_dir)],
            capture_output=True,
            text=True,
            env=env,
            timeout=30,
        )
        self._hook_path = self._cache_dir / "shell" / "hook.fish"

    def tearDown(self) -> None:
        if hasattr(self, "_temp_dir") and self._temp_dir.exists():
            shutil.rmtree(self._temp_dir, ignore_errors=True)

    def _make_envy_project(self, name: str) -> Path:
        project = self._temp_dir / name
        project.mkdir(parents=True, exist_ok=True)
        (project / "tools").mkdir(exist_ok=True)
        (project / "envy.lua").write_text('-- @envy bin "tools"\nPACKAGES = {}\n')
        return project

    def test_cd_into_project_adds_bin_to_path(self) -> None:
        project = self._make_envy_project("fish-proj1")
        result = self._run_fish_hook_test(
            f'source "{self._hook_path}"\ncd "{project}"\necho $PATH'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn(str(project / "tools"), result.stdout)

    def test_cd_out_removes_path(self) -> None:
        project = self._make_envy_project("fish-proj2")
        result = self._run_fish_hook_test(
            f'source "{self._hook_path}"\ncd "{project}"\ncd /tmp\necho $PATH'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertNotIn(str(project / "tools"), result.stdout)

    def test_cd_between_projects_swaps_path(self) -> None:
        proj_a = self._make_envy_project("fish-projA")
        proj_b = self._make_envy_project("fish-projB")
        result = self._run_fish_hook_test(
            f'source "{self._hook_path}"\ncd "{proj_a}"\ncd "{proj_b}"\necho $PATH'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn(str(proj_b / "tools"), result.stdout)
        self.assertNotIn(str(proj_a / "tools"), result.stdout)

    def test_envy_project_root_set(self) -> None:
        project = self._make_envy_project("fish-proj-root")
        result = self._run_fish_hook_test(
            f'source "{self._hook_path}"\ncd "{project}"\necho $ENVY_PROJECT_ROOT'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(str(project), result.stdout.strip())

    def test_deep_bin_dir_reaches_path(self) -> None:
        project, bin_dir = _write_deep_project(self._temp_dir, "fish-deep")
        result = self._run_fish_hook_test(
            f'source "{self._hook_path}"\ncd "{project}"\necho $PATH[1]'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(_real(bin_dir), _real(result.stdout.strip()))

    def test_deep_bin_dir_project_root_is_the_manifest_dir(self) -> None:
        project, _ = _write_deep_project(self._temp_dir, "fish-deep-root")
        result = self._run_fish_hook_test(
            f'source "{self._hook_path}"\ncd "{project}"\necho $ENVY_PROJECT_ROOT'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(_real(project), _real(result.stdout.strip()))

    def test_an_intermediate_of_a_deep_bin_dir_is_not_a_project(self) -> None:
        project, bin_dir = _write_deep_project(self._temp_dir, "fish-deep-mid")
        result = self._run_fish_hook_test(
            f'source "{self._hook_path}"\ncd "{project / "a" / "b"}"\n'
            f"echo $ENVY_PROJECT_ROOT\necho $PATH[1]"
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        root, first = result.stdout.strip().split("\n")
        self.assertEqual(_real(project), _real(root))
        self.assertEqual(_real(bin_dir), _real(first))

    def test_deep_bin_dir_leaves_path_on_the_way_out(self) -> None:
        project, bin_dir = _write_deep_project(self._temp_dir, "fish-deep-out")
        result = self._run_fish_hook_test(
            f'source "{self._hook_path}"\ncd "{project}"\n'
            f'cd "{self._temp_dir}"\necho (string join : $PATH)'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertNotIn(_real(bin_dir), _path_entries(result.stdout))

    # --- where a manifest's header ends ---

    def test_root_false_below_the_first_code_line_is_not_a_directive(self) -> None:
        parent = self._make_envy_project("fish-hdr-below")
        child = _write_project(parent, "sub", _ROOT_FALSE_BELOW_CODE)
        result = self._run_fish_hook_test(
            f'source "{self._hook_path}"\ncd "{child}"\necho $ENVY_PROJECT_ROOT'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(str(child), result.stdout.strip())

    def test_root_false_under_a_long_preamble_is_still_a_directive(self) -> None:
        parent = self._make_envy_project("fish-hdr-preamble")
        child = _write_project(parent, "sub", _ROOT_FALSE_AFTER_PREAMBLE)
        result = self._run_fish_hook_test(
            f'source "{self._hook_path}"\ncd "{child}"\necho $ENVY_PROJECT_ROOT'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(str(parent), result.stdout.strip())

    def test_bin_under_a_long_preamble_reaches_path(self) -> None:
        project = _write_project(self._temp_dir, "fish-hdr-bin", _BIN_AFTER_PREAMBLE)
        result = self._run_fish_hook_test(
            f'source "{self._hook_path}"\ncd "{project}"\necho $PATH'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn(str(project / "tools"), result.stdout)

    def test_repeated_bin_directive_takes_the_last(self) -> None:
        project = _write_project(self._temp_dir, "fish-hdr-dup", _BIN_REPEATED_LAST_WINS)
        (project / "stale").mkdir(exist_ok=True)
        result = self._run_fish_hook_test(
            f'source "{self._hook_path}"\ncd "{project}"\necho $PATH'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn(str(project / "tools"), result.stdout)
        self.assertNotIn(str(project / "stale"), result.stdout)

    @staticmethod
    def _hook_test_env() -> dict[str, str]:
        """Clean environment for hook tests — strip ENVY_SHELL_* to avoid leaking host config."""
        env = os.environ.copy()
        for key in list(env):
            if key.startswith("ENVY_SHELL_"):
                del env[key]
        return env

    def _run_fish_hook_test(self, script: str) -> subprocess.CompletedProcess[str]:
        return test_config.run(
            ["fish", "-c", script],
            capture_output=True,
            text=True,
            timeout=30,
            env=self._hook_test_env(),
        )

    # --- v2: enter/leave messages ---

    def test_entering_message_on_cd_into_project(self) -> None:
        project = self._make_envy_project("fish-msg-enter")
        result = self._run_fish_hook_test(
            f'source "{self._hook_path}"\ncd "{project}"\n'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn("envy: entering fish-msg-enter", result.stderr)
        self.assertIn("tools added to PATH", result.stderr)

    def test_leaving_message_on_cd_out(self) -> None:
        project = self._make_envy_project("fish-msg-leave")
        result = self._run_fish_hook_test(
            f'source "{self._hook_path}"\ncd "{project}"\ncd /tmp\n'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn("envy: leaving fish-msg-leave", result.stderr)
        self.assertIn("PATH restored", result.stderr)

    def test_switching_projects_prints_leave_and_enter(self) -> None:
        proj_a = self._make_envy_project("fish-sw-A")
        proj_b = self._make_envy_project("fish-sw-B")
        result = self._run_fish_hook_test(
            f'source "{self._hook_path}"\ncd "{proj_a}"\ncd "{proj_b}"\n'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        leave_pos = result.stderr.index("leaving fish-sw-A")
        enter_pos = result.stderr.index("entering fish-sw-B")
        self.assertLess(leave_pos, enter_pos)

    def test_no_duplicate_message_within_project(self) -> None:
        project = self._make_envy_project("fish-nodup")
        sub = project / "src"
        sub.mkdir()
        result = self._run_fish_hook_test(
            f'source "{self._hook_path}"\ncd "{project}"\ncd "{sub}"\n'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(1, result.stderr.count("entering fish-nodup"))

    def test_initial_activation_prints_message(self) -> None:
        project = self._make_envy_project("fish-init-msg")
        result = self._run_fish_hook_test(
            f'cd "{project}"\nsource "{self._hook_path}"\n'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn("envy: entering fish-init-msg", result.stderr)

    # --- v2: raccoon prompt ---

    def test_raccoon_in_prompt_when_in_project(self) -> None:
        project = self._make_envy_project("fish-raccoon")
        result = self._run_fish_hook_test(
            f'source "{self._hook_path}"\ncd "{project}"\nfish_prompt'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn("\U0001f99d", result.stdout)

    def test_raccoon_removed_on_leave(self) -> None:
        project = self._make_envy_project("fish-raccoon-rm")
        result = self._run_fish_hook_test(
            f'source "{self._hook_path}"\ncd "{project}"\ncd /tmp\n'
            f"echo $_ENVY_PROMPT_ACTIVE"
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertNotIn("1", result.stdout.strip())

    def test_raccoon_disabled_via_env_var(self) -> None:
        project = self._make_envy_project("fish-raccoon-off")
        result = self._run_fish_hook_test(
            f"set -gx ENVY_SHELL_NO_ICON 1\n"
            f'source "{self._hook_path}"\ncd "{project}"\nfish_prompt'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertNotIn("\U0001f99d", result.stdout)

    # --- v5: display control env vars ---

    def test_messages_suppressed_via_env_var(self) -> None:
        """ENVY_SHELL_NO_ENTER_EXIT_ANNOUNCE=1 suppresses enter/leave text."""
        project = self._make_envy_project("fish-no-announce")
        result = self._run_fish_hook_test(
            f"set -gx ENVY_SHELL_NO_ENTER_EXIT_ANNOUNCE 1\n"
            f'source "{self._hook_path}"\ncd "{project}"\n'
            f"echo $PATH\nfish_prompt"
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn(str(project / "tools"), result.stdout)
        self.assertIn("\U0001f99d", result.stdout)
        self.assertNotIn("envy: entering", result.stderr)

    def test_messages_suppressed_but_icon_shown(self) -> None:
        project = self._make_envy_project("fish-ann-no-icon-yes")
        result = self._run_fish_hook_test(
            f"set -gx ENVY_SHELL_NO_ENTER_EXIT_ANNOUNCE 1\n"
            f'source "{self._hook_path}"\ncd "{project}"\n'
            f"fish_prompt"
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertNotIn("envy: entering", result.stderr)
        self.assertIn("\U0001f99d", result.stdout)

    def test_icon_suppressed_but_messages_shown(self) -> None:
        project = self._make_envy_project("fish-icon-no-ann-yes")
        result = self._run_fish_hook_test(
            f"set -gx ENVY_SHELL_NO_ICON 1\n"
            f'source "{self._hook_path}"\ncd "{project}"\n'
            f"fish_prompt"
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn("envy: entering", result.stderr)
        self.assertNotIn("\U0001f99d", result.stdout)


@unittest.skipUnless(shutil.which("pwsh"), "pwsh not installed")
class TestPowerShellHook(EnvyTestCase):
    """Test PowerShell shell hook behavior."""

    @classmethod
    def setUpClass(cls) -> None:
        """Warm up pwsh to avoid cold-start timeouts on resource-constrained runners."""
        subprocess.run(
            ["pwsh", "-NoProfile", "-NonInteractive", "-Command", "$null"],
            capture_output=True,
            timeout=60,
        )

    def setUp(self) -> None:
        # .resolve() converts Windows 8.3 short names (RUNNER~1) to long names
        # (runneradmin) so Python paths match PowerShell's Resolve-Path output.
        self._temp_dir = self.make_temp_dir("_temp_dir").resolve()
        self._cache_dir = self._temp_dir / "cache"
        self._envy = test_config.get_envy_production_executable()

        project_dir = self._temp_dir / "setup-project"
        bin_dir = self._temp_dir / "setup-bin"
        env = test_config.get_test_env()
        env["ENVY_CACHE_ROOT"] = str(self._cache_dir)
        test_config.run(
            [str(self._envy), "init", str(project_dir), str(bin_dir)],
            capture_output=True,
            text=True,
            env=env,
            timeout=30,
        )
        self._hook_path = self._cache_dir / "shell" / "hook.ps1"
        assert self._hook_path.exists(), f"Hook not found at {self._hook_path}"

    def tearDown(self) -> None:
        if hasattr(self, "_temp_dir") and self._temp_dir.exists():
            shutil.rmtree(self._temp_dir, ignore_errors=True)

    @staticmethod
    def _hook_test_env() -> dict[str, str]:
        """Clean environment for hook tests — strip ENVY_SHELL_* to avoid leaking host config."""
        env = os.environ.copy()
        for key in list(env):
            if key.startswith("ENVY_SHELL_"):
                del env[key]
        return env

    def _run_pwsh_hook_test(self, script: str) -> subprocess.CompletedProcess[str]:
        """Run a PowerShell script that dot-sources the hook and tests behavior.

        Retries on a pwsh .NET-runtime startup crash (a nondeterministic infra
        flake, see test_config.is_pwsh_runtime_crash); genuine failures are
        returned as-is.
        """
        return test_config.run_pwsh(
            ["pwsh", "-NoProfile", "-NonInteractive", "-Command", script],
            capture_output=True,
            text=True,
            timeout=30,
            env=self._hook_test_env(),
        )

    def _make_envy_project(self, name: str, bin_val: str = "tools") -> Path:
        """Create a minimal envy project directory."""
        project = self._temp_dir / name
        project.mkdir(parents=True, exist_ok=True)
        (project / "tools").mkdir(exist_ok=True)
        (project / "envy.lua").write_text(
            f'-- @envy bin "{bin_val}"\nPACKAGES = {{}}\n'
        )
        return project

    def test_cd_into_project_adds_bin_to_path(self) -> None:
        project = self._make_envy_project("ps-proj1")
        result = self._run_pwsh_hook_test(
            f'. "{self._hook_path}"\n'
            f'Set-Location "{project}"\n'
            f"$global:_ENVY_LAST_PWD = $null\n"
            f"_envy_hook\n"
            f"Write-Output $env:PATH"
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn(str(project / "tools"), result.stdout)

    def test_cd_into_subdirectory_keeps_path(self) -> None:
        project = self._make_envy_project("ps-proj2")
        sub = project / "src"
        sub.mkdir()
        result = self._run_pwsh_hook_test(
            f'. "{self._hook_path}"\n'
            f'Set-Location "{project}"\n'
            f"$global:_ENVY_LAST_PWD = $null\n"
            f"_envy_hook\n"
            f'Set-Location "{sub}"\n'
            f"$global:_ENVY_LAST_PWD = $null\n"
            f"_envy_hook\n"
            f"Write-Output $env:PATH"
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn(str(project / "tools"), result.stdout)

    def test_cd_out_removes_bin_from_path(self) -> None:
        project = self._make_envy_project("ps-proj3")
        result = self._run_pwsh_hook_test(
            f'. "{self._hook_path}"\n'
            f'Set-Location "{project}"\n'
            f"$global:_ENVY_LAST_PWD = $null\n"
            f"_envy_hook\n"
            f'Set-Location "{self._temp_dir}"\n'
            f"$global:_ENVY_LAST_PWD = $null\n"
            f"_envy_hook\n"
            f"Write-Output $env:PATH"
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertNotIn(str(project / "tools"), result.stdout)

    def test_cd_between_projects_swaps_path(self) -> None:
        proj_a = self._make_envy_project("ps-projA")
        proj_b = self._make_envy_project("ps-projB")
        result = self._run_pwsh_hook_test(
            f'. "{self._hook_path}"\n'
            f'Set-Location "{proj_a}"\n'
            f"$global:_ENVY_LAST_PWD = $null\n"
            f"_envy_hook\n"
            f'Set-Location "{proj_b}"\n'
            f"$global:_ENVY_LAST_PWD = $null\n"
            f"_envy_hook\n"
            f"Write-Output $env:PATH"
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn(str(proj_b / "tools"), result.stdout)
        self.assertNotIn(str(proj_a / "tools"), result.stdout)

    def test_envy_project_root_set(self) -> None:
        project = self._make_envy_project("ps-proj4")
        result = self._run_pwsh_hook_test(
            f'. "{self._hook_path}"\n'
            f'Set-Location "{project}"\n'
            f"$global:_ENVY_LAST_PWD = $null\n"
            f"_envy_hook\n"
            f"Write-Output $env:ENVY_PROJECT_ROOT"
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(str(project), result.stdout.strip())

    def test_envy_project_root_unset_when_leaving(self) -> None:
        project = self._make_envy_project("ps-proj5")
        result = self._run_pwsh_hook_test(
            f'. "{self._hook_path}"\n'
            f'Set-Location "{project}"\n'
            f"$global:_ENVY_LAST_PWD = $null\n"
            f"_envy_hook\n"
            f'Set-Location "{self._temp_dir}"\n'
            f"$global:_ENVY_LAST_PWD = $null\n"
            f"_envy_hook\n"
            f'if ($env:ENVY_PROJECT_ROOT) {{ Write-Output "root=$env:ENVY_PROJECT_ROOT" }}'
            f' else {{ Write-Output "root=unset" }}'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual("root=unset", result.stdout.strip())

    def test_deep_bin_dir_reaches_path(self) -> None:
        project, bin_dir = _write_deep_project(self._temp_dir, "ps-deep")
        result = self._run_pwsh_hook_test(
            f'. "{self._hook_path}"\n'
            f'Set-Location "{project}"\n'
            f"$global:_ENVY_LAST_PWD = $null\n"
            f"_envy_hook\n"
            f"Write-Output ($env:PATH -split [System.IO.Path]::PathSeparator)[0]"
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(_real(bin_dir), _real(result.stdout.strip()))

    def test_deep_bin_dir_project_root_is_the_manifest_dir(self) -> None:
        project, _ = _write_deep_project(self._temp_dir, "ps-deep-root")
        result = self._run_pwsh_hook_test(
            f'. "{self._hook_path}"\n'
            f'Set-Location "{project}"\n'
            f"$global:_ENVY_LAST_PWD = $null\n"
            f"_envy_hook\n"
            f"Write-Output $env:ENVY_PROJECT_ROOT"
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(_real(project), _real(result.stdout.strip()))

    def test_an_intermediate_of_a_deep_bin_dir_is_not_a_project(self) -> None:
        """`a\\b` holds no envy.lua, so the walk climbs past it to the real manifest."""
        project, bin_dir = _write_deep_project(self._temp_dir, "ps-deep-mid")
        result = self._run_pwsh_hook_test(
            f'. "{self._hook_path}"\n'
            f'Set-Location "{project / "a" / "b"}"\n'
            f"$global:_ENVY_LAST_PWD = $null\n"
            f"_envy_hook\n"
            f"Write-Output $env:ENVY_PROJECT_ROOT\n"
            f"Write-Output ($env:PATH -split [System.IO.Path]::PathSeparator)[0]"
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        root, first = result.stdout.strip().splitlines()
        self.assertEqual(_real(project), _real(root))
        self.assertEqual(_real(bin_dir), _real(first))

    def test_deep_bin_dir_leaves_path_on_the_way_out(self) -> None:
        project, bin_dir = _write_deep_project(self._temp_dir, "ps-deep-out")
        result = self._run_pwsh_hook_test(
            f'. "{self._hook_path}"\n'
            f'Set-Location "{project}"\n'
            f"$global:_ENVY_LAST_PWD = $null\n"
            f"_envy_hook\n"
            f'Set-Location "{self._temp_dir}"\n'
            f"$global:_ENVY_LAST_PWD = $null\n"
            f"_envy_hook\n"
            f"Write-Output $env:PATH"
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertNotIn(_real(bin_dir), _path_entries(result.stdout))

    def test_missing_bin_directive_no_path_change(self) -> None:
        project = self._temp_dir / "ps-no-bin"
        project.mkdir(parents=True)
        (project / "envy.lua").write_text("PACKAGES = {}\n")
        sep = os.pathsep
        result = self._run_pwsh_hook_test(
            f'$env:PATH = "/usr/bin{sep}/bin"\n'
            f'. "{self._hook_path}"\n'
            f'Set-Location "{project}"\n'
            f"$global:_ENVY_LAST_PWD = $null\n"
            f"_envy_hook\n"
            f"Write-Output $env:PATH"
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(f"/usr/bin{sep}/bin", result.stdout.strip())

    def test_disable_env_var(self) -> None:
        project = self._make_envy_project("ps-proj-disable")
        sep = os.pathsep
        result = self._run_pwsh_hook_test(
            f'$env:ENVY_SHELL_HOOK_DISABLE = "1"\n'
            f'$env:PATH = "/usr/bin{sep}/bin"\n'
            f'. "{self._hook_path}"\n'
            f'Set-Location "{project}"\n'
            f"$global:_ENVY_LAST_PWD = $null\n"
            f"_envy_hook\n"
            f"Write-Output $env:PATH"
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertNotIn(str(project / "tools"), result.stdout)

    def test_nested_root_false_finds_ancestor(self) -> None:
        parent = self._make_envy_project("ps-parent-proj")
        child = parent / "sub"
        child.mkdir(parents=True, exist_ok=True)
        (child / "tools").mkdir(exist_ok=True)
        (child / "envy.lua").write_text(
            '-- @envy root "false"\n-- @envy bin "tools"\nPACKAGES = {}\n'
        )
        result = self._run_pwsh_hook_test(
            f'. "{self._hook_path}"\n'
            f'Set-Location "{child}"\n'
            f"$global:_ENVY_LAST_PWD = $null\n"
            f"_envy_hook\n"
            f"Write-Output $env:ENVY_PROJECT_ROOT"
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn(str(parent), result.stdout)

    # --- where a manifest's header ends ---
    #
    # Compared exactly, not with assertIn: the child's path has the parent's as a prefix, so
    # a containment check passes for either answer.

    def test_root_false_below_the_first_code_line_is_not_a_directive(self) -> None:
        parent = self._make_envy_project("ps-hdr-below")
        child = _write_project(parent, "sub", _ROOT_FALSE_BELOW_CODE)
        result = self._run_pwsh_hook_test(
            f'. "{self._hook_path}"\n'
            f'Set-Location "{child}"\n'
            f"$global:_ENVY_LAST_PWD = $null\n"
            f"_envy_hook\n"
            f"Write-Output $env:ENVY_PROJECT_ROOT"
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(str(child), result.stdout.strip())

    def test_root_false_under_a_long_preamble_is_still_a_directive(self) -> None:
        parent = self._make_envy_project("ps-hdr-preamble")
        child = _write_project(parent, "sub", _ROOT_FALSE_AFTER_PREAMBLE)
        result = self._run_pwsh_hook_test(
            f'. "{self._hook_path}"\n'
            f'Set-Location "{child}"\n'
            f"$global:_ENVY_LAST_PWD = $null\n"
            f"_envy_hook\n"
            f"Write-Output $env:ENVY_PROJECT_ROOT"
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(str(parent), result.stdout.strip())

    def test_bin_under_a_long_preamble_reaches_path(self) -> None:
        project = _write_project(self._temp_dir, "ps-hdr-bin", _BIN_AFTER_PREAMBLE)
        result = self._run_pwsh_hook_test(
            f'. "{self._hook_path}"\n'
            f'Set-Location "{project}"\n'
            f"$global:_ENVY_LAST_PWD = $null\n"
            f"_envy_hook\n"
            f"Write-Output $env:PATH"
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn(str(project / "tools"), result.stdout)

    def test_repeated_bin_directive_takes_the_last(self) -> None:
        project = _write_project(self._temp_dir, "ps-hdr-dup", _BIN_REPEATED_LAST_WINS)
        (project / "stale").mkdir(exist_ok=True)
        result = self._run_pwsh_hook_test(
            f'. "{self._hook_path}"\n'
            f'Set-Location "{project}"\n'
            f"$global:_ENVY_LAST_PWD = $null\n"
            f"_envy_hook\n"
            f"Write-Output $env:PATH"
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn(str(project / "tools"), result.stdout)
        self.assertNotIn(str(project / "stale"), result.stdout)

    def test_initial_activation_inside_project(self) -> None:
        """Dot-sourcing hook while already in a project activates immediately."""
        project = self._make_envy_project("ps-proj-init")
        result = self._run_pwsh_hook_test(
            f'Set-Location "{project}"\n. "{self._hook_path}"\nWrite-Output $env:PATH'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn(str(project / "tools"), result.stdout)

    # --- v2: enter/leave messages ---

    def test_entering_message_on_cd_into_project(self) -> None:
        project = self._make_envy_project("ps-msg-enter")
        result = self._run_pwsh_hook_test(
            f'. "{self._hook_path}"\n'
            f'Set-Location "{project}"\n'
            f"$global:_ENVY_LAST_PWD = $null\n"
            f"_envy_hook\n"
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        # PowerShell Write-Host goes to stdout in non-interactive mode
        combined = result.stdout + result.stderr
        self.assertIn("envy: entering ps-msg-enter", combined)
        self.assertIn("tools added to PATH", combined)

    def test_leaving_message_on_cd_out(self) -> None:
        project = self._make_envy_project("ps-msg-leave")
        result = self._run_pwsh_hook_test(
            f'. "{self._hook_path}"\n'
            f'Set-Location "{project}"\n'
            f"$global:_ENVY_LAST_PWD = $null\n"
            f"_envy_hook\n"
            f'Set-Location "{self._temp_dir}"\n'
            f"$global:_ENVY_LAST_PWD = $null\n"
            f"_envy_hook\n"
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        combined = result.stdout + result.stderr
        self.assertIn("envy: leaving ps-msg-leave", combined)
        self.assertIn("PATH restored", combined)

    def test_switching_projects_prints_leave_and_enter(self) -> None:
        proj_a = self._make_envy_project("ps-sw-A")
        proj_b = self._make_envy_project("ps-sw-B")
        result = self._run_pwsh_hook_test(
            f'. "{self._hook_path}"\n'
            f'Set-Location "{proj_a}"\n'
            f"$global:_ENVY_LAST_PWD = $null\n"
            f"_envy_hook\n"
            f'Set-Location "{proj_b}"\n'
            f"$global:_ENVY_LAST_PWD = $null\n"
            f"_envy_hook\n"
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        combined = result.stdout + result.stderr
        leave_pos = combined.index("leaving ps-sw-A")
        enter_pos = combined.index("entering ps-sw-B")
        self.assertLess(leave_pos, enter_pos)

    def test_no_duplicate_message_within_project(self) -> None:
        project = self._make_envy_project("ps-nodup")
        sub = project / "src"
        sub.mkdir()
        result = self._run_pwsh_hook_test(
            f'. "{self._hook_path}"\n'
            f'Set-Location "{project}"\n'
            f"$global:_ENVY_LAST_PWD = $null\n"
            f"_envy_hook\n"
            f'Set-Location "{sub}"\n'
            f"$global:_ENVY_LAST_PWD = $null\n"
            f"_envy_hook\n"
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        combined = result.stdout + result.stderr
        self.assertEqual(1, combined.count("entering ps-nodup"))

    def test_initial_activation_prints_message(self) -> None:
        project = self._make_envy_project("ps-init-msg")
        result = self._run_pwsh_hook_test(
            f'Set-Location "{project}"\n. "{self._hook_path}"\n'
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        combined = result.stdout + result.stderr
        self.assertIn("envy: entering ps-init-msg", combined)

    # --- v2: raccoon prompt ---

    def test_raccoon_in_prompt_when_in_project(self) -> None:
        project = self._make_envy_project("ps-raccoon")
        result = self._run_pwsh_hook_test(
            f'. "{self._hook_path}"\n'
            f'Set-Location "{project}"\n'
            f"$global:_ENVY_LAST_PWD = $null\n"
            f"_envy_hook\n"
            f"Write-Output $global:_ENVY_PROMPT_ACTIVE"
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual("True", result.stdout.strip())

    def test_raccoon_removed_on_leave(self) -> None:
        project = self._make_envy_project("ps-raccoon-rm")
        result = self._run_pwsh_hook_test(
            f'. "{self._hook_path}"\n'
            f'Set-Location "{project}"\n'
            f"$global:_ENVY_LAST_PWD = $null\n"
            f"_envy_hook\n"
            f'Set-Location "{self._temp_dir}"\n'
            f"$global:_ENVY_LAST_PWD = $null\n"
            f"_envy_hook\n"
            f"Write-Output $global:_ENVY_PROMPT_ACTIVE"
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual("False", result.stdout.strip())

    def test_raccoon_disabled_via_env_var(self) -> None:
        project = self._make_envy_project("ps-raccoon-off")
        result = self._run_pwsh_hook_test(
            f'$env:ENVY_SHELL_NO_ICON = "1"\n'
            f'. "{self._hook_path}"\n'
            f'Set-Location "{project}"\n'
            f"$global:_ENVY_LAST_PWD = $null\n"
            f"_envy_hook\n"
            f"prompt"
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        combined = result.stdout + result.stderr
        self.assertNotIn("\U0001f99d", combined)

    # --- v5: display control env vars ---

    def test_messages_suppressed_via_env_var(self) -> None:
        """ENVY_SHELL_NO_ENTER_EXIT_ANNOUNCE=1 suppresses enter/leave text."""
        project = self._make_envy_project("ps-no-announce")
        result = self._run_pwsh_hook_test(
            f'$env:ENVY_SHELL_NO_ENTER_EXIT_ANNOUNCE = "1"\n'
            f'. "{self._hook_path}"\n'
            f'Set-Location "{project}"\n'
            f"$global:_ENVY_LAST_PWD = $null\n"
            f"_envy_hook\n"
            f"Write-Output $env:PATH\n"
            f"Write-Output $global:_ENVY_PROMPT_ACTIVE"
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn(str(project / "tools"), result.stdout)
        self.assertIn("True", result.stdout)
        combined = result.stdout + result.stderr
        self.assertNotIn("envy: entering", combined)

    def test_messages_suppressed_but_icon_shown(self) -> None:
        project = self._make_envy_project("ps-ann-no-icon-yes")
        result = self._run_pwsh_hook_test(
            f'$env:ENVY_SHELL_NO_ENTER_EXIT_ANNOUNCE = "1"\n'
            f'. "{self._hook_path}"\n'
            f'Set-Location "{project}"\n'
            f"$global:_ENVY_LAST_PWD = $null\n"
            f"_envy_hook\n"
            f"Write-Output $global:_ENVY_PROMPT_ACTIVE"
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        combined = result.stdout + result.stderr
        self.assertNotIn("envy: entering", combined)
        self.assertIn("True", result.stdout)

    def test_icon_suppressed_but_messages_shown(self) -> None:
        project = self._make_envy_project("ps-icon-no-ann-yes")
        result = self._run_pwsh_hook_test(
            f'$env:ENVY_SHELL_NO_ICON = "1"\n'
            f'. "{self._hook_path}"\n'
            f'Set-Location "{project}"\n'
            f"$global:_ENVY_LAST_PWD = $null\n"
            f"_envy_hook\n"
            f"prompt"
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        combined = result.stdout + result.stderr
        self.assertIn("envy: entering", combined)
        self.assertNotIn("\U0001f99d", combined)

    # --- v6: one-time UTF-8 capability hint ---

    def test_utf8_hint_shown_once_when_icon_wanted_but_console_not_utf8(self) -> None:
        """Non-UTF-8 console + icon wanted: nudge exactly once per session, not on every entry."""
        project = self._make_envy_project("ps-utf8-hint")
        result = self._run_pwsh_hook_test(
            f"$env:LANG = ''; $env:LC_ALL = ''; $env:LC_CTYPE = ''\n"
            f"[Console]::OutputEncoding = [System.Text.Encoding]::ASCII\n"
            f'. "{self._hook_path}"\n'
            f'Set-Location "{project}"\n'
            f"$global:_ENVY_LAST_PWD = $null\n"
            f"_envy_hook\n"
            f'Set-Location "{self._temp_dir}"\n'
            f"$global:_ENVY_LAST_PWD = $null\n"
            f"_envy_hook\n"
            f'Set-Location "{project}"\n'
            f"$global:_ENVY_LAST_PWD = $null\n"
            f"_envy_hook\n"
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        combined = result.stdout + result.stderr
        # Entered twice (with a leave between), but the nudge fires only once.
        self.assertEqual(1, combined.count("raccoon icon hidden"))
        self.assertIn("UTF8Encoding", combined)

    def test_utf8_hint_suppressed_when_icon_disabled(self) -> None:
        """ENVY_SHELL_NO_ICON=1 means the icon is unwanted — never nudge about it."""
        project = self._make_envy_project("ps-utf8-hint-off")
        result = self._run_pwsh_hook_test(
            f'$env:ENVY_SHELL_NO_ICON = "1"\n'
            f"$env:LANG = ''; $env:LC_ALL = ''; $env:LC_CTYPE = ''\n"
            f"[Console]::OutputEncoding = [System.Text.Encoding]::ASCII\n"
            f'. "{self._hook_path}"\n'
            f'Set-Location "{project}"\n'
            f"$global:_ENVY_LAST_PWD = $null\n"
            f"_envy_hook\n"
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        combined = result.stdout + result.stderr
        self.assertNotIn("raccoon icon hidden", combined)


if __name__ == "__main__":
    unittest.main()
