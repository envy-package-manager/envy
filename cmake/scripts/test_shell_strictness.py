#!/usr/bin/env python3
"""Every shell script and every workflow `run:` step is strict bash, and says so.

v0.2.1 and v0.2.2 shipped Linux artifacts stamped 0.0.0 because a container job's `run:`
block ran under `sh`, where `[[` is an unknown command rather than a syntax error -- as an
`if` condition it just tests false, so `-DENVY_VERSION` was dropped in silence and the step
still went green. The same undeclared-shell default hid the valgrind wrapper for a release.

Neither the shipped launchers nor the workflows have a runtime that would catch this, so
the rule is enforced here: bash via `/usr/bin/env`, `set -Eeuo pipefail`, no implicit shell.
"""

import re
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORKFLOWS = ROOT / ".github" / "workflows"

SHEBANG = "#!/usr/bin/env bash"
STRICT = "set -Eeuo pipefail"
WF_SHELL = "/usr/bin/env bash -Eeuo pipefail {0}"

# Sourced into the caller's interactive shell, never executed. `set -e` there kills the
# user's terminal on any failing command and `-u` breaks on names they own, not envy's.
SOURCED = {"src/resources/shell_hook.bash"}

_BASH_SHEBANG_RE = re.compile(rb"^#!.*\bbash\b")


def _tracked_files() -> list[Path]:
    """Tracked files only: a scratch script in someone's tree is not envy's to police."""
    out = subprocess.run(
        ["git", "-C", str(ROOT), "ls-files", "-z"],
        capture_output=True,
        text=True,
        timeout=30,
    )
    if out.returncode != 0:
        return []
    return [ROOT / rel for rel in out.stdout.split("\0") if rel]


def bash_scripts() -> list[Path]:
    """Files that are bash: named like one, or carrying a bash shebang."""
    found = []
    for p in _tracked_files():
        if p.relative_to(ROOT).as_posix() in SOURCED or not p.is_file():
            continue
        with p.open("rb") as fh:
            first = fh.readline()
        if p.suffix in (".sh", ".bash") or _BASH_SHEBANG_RE.match(first):
            found.append(p)
    return sorted(found)


class ShellScriptStrictness(unittest.TestCase):
    """The scripts envy ships and the ones it builds with."""

    def setUp(self) -> None:
        self.scripts = bash_scripts()
        # A silent empty walk would pass every assertion below without checking anything.
        self.assertGreaterEqual(len(self.scripts), 4, "found almost no bash scripts")

    def test_shebang_goes_through_env(self) -> None:
        """`#!/bin/bash` is not portable; macOS puts 3.2 there and Homebrew's 5 elsewhere."""
        for p in self.scripts:
            with self.subTest(script=p.relative_to(ROOT).as_posix()):
                self.assertEqual(SHEBANG, p.read_text().splitlines()[0])

    def test_sets_strict_mode_up_front(self) -> None:
        """Before anything can fail unnoticed -- so within the first few lines."""
        for p in self.scripts:
            with self.subTest(script=p.relative_to(ROOT).as_posix()):
                head = p.read_text().splitlines()[:6]
                self.assertIn(STRICT, head, f"no `{STRICT}` in the opening lines")


def workflow_run_steps() -> list[tuple[str, str, str | None]]:
    """(workflow, job, effective shell) per `run:` step, from a fixed-indent scan.

    The runner's own defaults are what this guards against, so it cannot ask the runner.
    Two-space indent, jobs at 2 and step keys at 8, which is what every file here uses.
    """
    steps: list[tuple[str, str, str | None]] = []
    for wf in sorted(WORKFLOWS.glob("*.yml")):
        job = None
        job_shell: str | None = None
        step_shell: str | None = None
        pending_run = False
        in_defaults = False
        for line in wf.read_text().splitlines():
            if re.match(r"^  [A-Za-z0-9_-]+:\s*$", line):  # a job name
                if pending_run:
                    steps.append((wf.name, job, step_shell or job_shell))
                job, job_shell, step_shell, pending_run, in_defaults = (
                    line.strip().rstrip(":"), None, None, False, False)
            elif re.match(r"^    defaults:\s*$", line):
                in_defaults = True
            elif in_defaults and (m := re.match(r"^        shell:\s*(.+?)\s*$", line)):
                job_shell, in_defaults = m.group(1), False
            elif re.match(r"^      - ", line):  # a new step ends the previous one
                if pending_run:
                    steps.append((wf.name, job, step_shell or job_shell))
                step_shell, pending_run, in_defaults = None, False, False
                if re.match(r"^      - run:\s*", line):
                    pending_run = True
            elif m := re.match(r"^        shell:\s*(.+?)\s*$", line):
                step_shell = m.group(1)
            elif re.match(r"^        run:\s*", line):
                pending_run = True
        if pending_run:
            steps.append((wf.name, job, step_shell or job_shell))
    return steps


class WorkflowShellStrictness(unittest.TestCase):
    """No `run:` step may inherit the runner's choice of shell."""

    def setUp(self) -> None:
        self.steps = workflow_run_steps()
        # The scan is indentation-based: if it silently matched nothing it would pass.
        self.assertGreaterEqual(len(self.steps), 15, "scan found too few run: steps")
        self.assertGreaterEqual(
            len({w for w, _, _ in self.steps}), 4, "scan missed whole workflow files"
        )

    def test_every_run_step_declares_a_shell(self) -> None:
        """An undeclared shell is `sh` in a container and `bash -e` (no pipefail) outside."""
        for wf, job, shell in self.steps:
            with self.subTest(step=f"{wf}::{job}"):
                self.assertIsNotNone(shell, "no step-level or job-level shell")

    def test_bash_steps_are_the_strict_invocation(self) -> None:
        """Anything weaker discards a failed pipeline stage or an unbound name."""
        for wf, job, shell in self.steps:
            if shell and "bash" in shell:
                with self.subTest(step=f"{wf}::{job}"):
                    self.assertEqual(WF_SHELL, shell)


if __name__ == "__main__":
    sys.exit(0 if unittest.main(exit=False).result.wasSuccessful() else 1)
