"""The version stamped into this binary: well-formed, matching its tag, and deployed.

v0.2.1 and v0.2.2 shipped Linux artifacts reporting 0.0.0. CI's tag -> -DENVY_VERSION
step was a bashism, and container jobs default to `sh`, where `[[` is an unknown command
rather than a syntax error -- as an `if` condition it just tests false, so the flag was
dropped in silence. `deploy` stamps whatever the running binary reports, and Linux CI
regenerated the same wrong answer on every push, so nothing downstream ever objected.

The tag test is the one that would have caught it, and it deliberately does not read
-DENVY_VERSION: the expectation comes from the tag, so a build that dropped the flag has
nothing to agree with itself about.
"""

import os
import re
import shutil
import subprocess
import unittest
from pathlib import Path

from . import test_config
from .env import EnvyTestCase


_REPO_ROOT = Path(__file__).resolve().parents[1]
# The sentinel reexec.cpp and both launchers treat as "dev build, let it through".
_DEV_VERSION = "0.0.0"
_SEMVER_RE = re.compile(r"^\d+\.\d+\.\d+$")
_FALLBACK_RE = re.compile(r"""ENVY_FALLBACK_VERSION=["']?([0-9][^"'\s]*)""")


def _exact_tag() -> str | None:
    """The `v*` tag HEAD sits exactly on; None if untagged, shallow, or git absent."""
    git = shutil.which("git")
    if git is None:
        return None
    try:
        result = subprocess.run(
            [git, "-C", str(_REPO_ROOT), "describe", "--exact-match", "--tags", "HEAD"],
            capture_output=True,
            timeout=30,
            **test_config.SUBPROCESS_TEXT_MODE,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    tag = result.stdout.strip()
    return tag if result.returncode == 0 and tag.startswith("v") else None


class TestVersionStamp(EnvyTestCase):
    """What `envy version` reports, and whether anyone else agrees with it."""

    def setUp(self) -> None:
        self._version = test_config.get_envy_version()

    def test_reported_version_is_well_formed(self) -> None:
        """A stamp that skipped the build system leaks a placeholder or a stub."""
        self.assertRegex(self._version, _SEMVER_RE)
        self.assertNotIn("@@", self._version)

    def test_tagged_checkout_reports_its_tag(self) -> None:
        """On a release tag the binary must say so -- this is the release gate.

        Off-tag there is nothing to check. On a tag, an unstamped build is a dev build
        everywhere but CI, the one place a tag build becomes an artifact.
        """
        tag = _exact_tag()
        if tag is None:
            self.skipTest("HEAD is not on a v* tag; nothing to hold the build to")
        if self._version == _DEV_VERSION and not os.environ.get("CI"):
            self.skipTest(f"local dev build at {tag}; CI is what must be stamped")
        self.assertEqual(
            tag[1:],
            self._version,
            f"built at {tag}, reports {self._version}: -DENVY_VERSION never reached"
            " cmake",
        )


class TestLauncherStamp(EnvyTestCase):
    """The launchers `deploy` writes must carry the version that wrote them.

    Not a tautology in practice: the launchers reach ENVY_VERSION_STR by a different
    substitution path than `envy version` does, and theirs is what a project commits.
    """

    def setUp(self) -> None:
        self._temp_dir = self.make_temp_dir("_temp_dir")
        self._project_dir = self._temp_dir / "project"
        self._bin_dir = self._project_dir / "bin"
        self._envy = test_config.get_envy_production_executable()
        self._version = test_config.get_envy_version()

    def test_init_stamps_the_running_version(self) -> None:
        env = test_config.get_test_env()
        env["ENVY_CACHE_ROOT"] = str(self._temp_dir / "cache")
        result = test_config.run(
            [str(self._envy), "init", str(self._project_dir), str(self._bin_dir)],
            capture_output=True,
            env=env,
            timeout=30,
            **test_config.SUBPROCESS_TEXT_MODE,
        )
        self.assertEqual(0, result.returncode, result.stderr)

        launchers = (self._bin_dir / "envy", self._bin_dir / "envy.bat")
        stamped = [p for p in launchers if p.exists()]
        self.assertTrue(stamped, f"init wrote no launcher into {self._bin_dir}")
        for launcher in stamped:
            match = _FALLBACK_RE.search(launcher.read_text(encoding="utf-8"))
            self.assertIsNotNone(match, f"no ENVY_FALLBACK_VERSION in {launcher}")
            self.assertEqual(self._version, match.group(1), f"stale stamp: {launcher}")


if __name__ == "__main__":
    unittest.main()
