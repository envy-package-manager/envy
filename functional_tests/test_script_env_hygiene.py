"""Every variable an envy-generated script sets must be ENVY_-prefixed.

These scripts hand their environment to a child: the launchers exec the envy binary, and a
product script execs its payload. The leak mechanism differs per shell --

  - envy.bat and product_script.bat run the child *inside* their `setlocal` scope, so every
    `set` is in the child's environment unconditionally.
  - The bash scripts' plain assignments are not exported, so they reach a child only when
    the caller had already exported that name -- and then the assignment silently replaces
    the caller's value for the child and everything under it.

-- but the rule is the same either way, and a name like SCRIPT_DIR, DIR, CACHE or VERSION
is one a caller plausibly owns. This is a static check on the shipped templates rather than
a behavioral one, because the batch halves only run on Windows and the bash halves only leak
when a specific name is already exported; neither shape is reliably reachable from a test.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

_RESOURCES = Path(__file__).resolve().parent.parent / "src" / "resources"

# Names envy reads from the caller's environment or that belong to the shell, rather than
# names envy owns. Setting these is the point, so prefixing them would break the script.
_NOT_OURS = {
    # envy's own documented interface, already prefixed.
    "ENVY_CACHE_ROOT", "ENVY_MIRROR", "ENVY_REEXEC", "ENVY_NO_REEXEC",
    # The caller's environment.
    "HOME", "USERPROFILE", "LOCALAPPDATA", "XDG_CACHE_HOME", "TMPDIR", "TEMP", "TMP",
    "PATH", "PATHEXT", "OS",
    # Shell builtins and specials.
    "IFS", "REPLY", "PS1", "PROMPT", "PROMPT_COMMAND", "precmd_functions", "path",
    "RANDOM", "ERRORLEVEL",
}

_BASH_ASSIGN = re.compile(r"^[ \t]*([A-Za-z_][A-Za-z0-9_]*)=", re.M)
_BAT_ASSIGN = re.compile(r'^[ \t]*set +"?([A-Za-z_][A-Za-z0-9_]*)=', re.M | re.I)
_BAT_SET_PA = re.compile(r"^[ \t]*set +/[ap] +([A-Za-z_][A-Za-z0-9_]*)", re.M | re.I)


def _offenders(names: set[str]) -> set[str]:
    return {n for n in names if n not in _NOT_OURS and not n.startswith("ENVY_")}


class TestGeneratedScriptEnvHygiene(unittest.TestCase):
    def _bash_globals(self, script: str) -> set[str]:
        """Assignments outside any function body.

        A `local` inside a function cannot escape it, so only top-level assignments are
        candidates for clobbering a caller's exported name.
        """
        depth, names = 0, set()
        for line in script.splitlines():
            if re.match(r"^[A-Za-z_][A-Za-z0-9_]*\(\) *\{", line):
                # A one-liner like `f() { ...; }` opens and closes on the same line. Counting
                # it as an opener left depth stuck above zero for the rest of the file, which
                # made this whole check silently examine nothing.
                if not line.rstrip().endswith("}"):
                    depth += 1
                continue
            if depth and line.startswith("}"):
                depth -= 1
                continue
            if depth:
                continue
            m = _BASH_ASSIGN.match(line)
            if m:
                names.add(m.group(1))
        return names

    def test_bash_launcher(self) -> None:
        script = (_RESOURCES / "envy").read_text(encoding="utf-8")
        self.assertEqual(set(), _offenders(self._bash_globals(script)))

    def test_posix_product_script(self) -> None:
        script = (_RESOURCES / "product_script").read_text(encoding="utf-8")
        self.assertEqual(set(), _offenders(self._bash_globals(script)))

    def test_batch_scripts(self) -> None:
        # Every `set` counts here: the child runs inside the setlocal scope, so function
        # scoping is not a thing that saves a name the way `local` does in bash.
        for name in ("envy.bat", "product_script.bat"):
            with self.subTest(script=name):
                script = (_RESOURCES / name).read_text(encoding="utf-8")
                found = set(_BAT_ASSIGN.findall(script)) | set(_BAT_SET_PA.findall(script))
                self.assertEqual(set(), _offenders(found))

    def test_the_check_can_fail(self) -> None:
        """A guard that cannot fail guards nothing."""
        self.assertEqual({"SCRIPT_DIR"}, _offenders({"SCRIPT_DIR", "ENVY_CACHE", "HOME"}))
        self.assertEqual({"CACHE"}, _offenders(self._bash_globals('CACHE=1\nENVY_X=2\n')))


if __name__ == "__main__":
    unittest.main()
