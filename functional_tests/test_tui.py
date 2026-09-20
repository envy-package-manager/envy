"""Functional tests for TUI progress rendering.

Tests ANSI rendering in TTY mode, fallback mode for non-TTY environments,
and interactive mode terminal control.
"""

import os
import sys
import unittest
from pathlib import Path

from . import test_config
from .env import EnvyTestCase
from .test_config import make_manifest

# User-managed spec that runs shell commands during install
SPEC_BUILD_FUNCTION = """IDENTITY = "local.build_function@v1"

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      envy.run("echo 'Building with envy.run()'", { quiet = true })
      envy.run("echo 'Build finished successfully'", { quiet = true })
    end,
  },
}

"""

# Dependency spec for parallel execution test
SPEC_BUILD_DEPENDENCY = """IDENTITY = "local.build_dependency@v1"

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      envy.run("echo 'dependency: begin'", { quiet = true })
      envy.run("echo 'dependency: success'", { quiet = true })
    end,
  },
}

"""

# Fast-completing spec (instant install)
SPEC_FAST = """IDENTITY = "local.fast@v1"
USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options) return false end,
    INSTALL = function(pkg_dir, options)
      envy.run("echo fast-install-done", { quiet = true })
    end,
  },
}

"""

# Slow spec (sleeps to keep renderer active)
SPEC_SLOW = """IDENTITY = "local.slow@v1"
USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options) return false end,
    INSTALL = function(pkg_dir, options)
      envy.run("sleep 2")
    end,
  },
}

"""

# Minimal spec for ANSI/fallback mode tests
SPEC_SIMPLE = """IDENTITY = "local.simple@v1"
DEPENDENCIES = {}

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- No-op install
    end,
  },
}

"""


class TestTUIRendering(EnvyTestCase):
    """Tests for TUI progress rendering modes."""

    def setUp(self):
        super().setUp()
        self.test_dir = self.make_temp_dir("test_dir")

        self.specs_dir = self.test_dir / "specs"
        self.specs_dir.mkdir()

        # Write specs
        (self.specs_dir / "build_function.lua").write_text(SPEC_BUILD_FUNCTION)
        (self.specs_dir / "build_dependency.lua").write_text(SPEC_BUILD_DEPENDENCY)
        (self.specs_dir / "simple.lua").write_text(SPEC_SIMPLE)
        (self.specs_dir / "fast.lua").write_text(SPEC_FAST)
        (self.specs_dir / "slow.lua").write_text(SPEC_SLOW)

    @staticmethod
    def lua_path(path: Path) -> str:
        """Convert path to Lua-safe string."""
        return path.as_posix()

    def create_manifest(self, content: str) -> Path:
        """Create manifest file with given content."""
        manifest_path = self.test_dir / "envy.lua"
        manifest_path.write_text(make_manifest(content), encoding="utf-8")
        return manifest_path

    def run_sync(self, manifest: Path, env: dict | None = None):
        """Run 'envy install' and return result."""
        cmd = [
            str(self.envy),
            "--cache-root",
            str(self.cache_root),
            "install",
            "--manifest",
            str(manifest),
        ]

        run_env = os.environ.copy()
        if env:
            run_env.update(env)

        result = test_config.run(
            cmd,
            cwd=self.project_root,
            capture_output=True,
            env=run_env,
        )
        return result

    def test_parallel_specs_complete_successfully(self):
        """Multiple specs complete successfully in parallel.

        Note: ANSI rendering verification requires a real TTY and is tested manually.
        When stderr is captured (as in automated tests), TUI uses fallback mode.
        """
        manifest = self.create_manifest(
            f"""
PACKAGES = {{
    {{ spec = "local.build_function@v1", source = "{self.lua_path(self.specs_dir)}/build_function.lua" }},
    {{ spec = "local.build_dependency@v1", source = "{self.lua_path(self.specs_dir)}/build_dependency.lua" }},
}}
"""
        )

        result = self.run_sync(manifest, env={"TERM": "xterm-256color"})

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

    def test_fallback_mode_with_term_dumb(self):
        """No ANSI codes when TERM=dumb."""
        manifest = self.create_manifest(
            f"""
PACKAGES = {{
    {{ spec = "local.simple@v1", source = "{self.lua_path(self.specs_dir)}/simple.lua" }},
}}
"""
        )

        result = self.run_sync(manifest, env={"TERM": "dumb"})

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        # Check that no ANSI codes are present
        self.assertNotIn(
            "\x1b[", result.stderr, "Expected no ANSI codes when TERM=dumb"
        )

    def test_fallback_mode_with_piped_stderr(self):
        """No ANSI codes when stderr is piped (not a TTY)."""
        manifest = self.create_manifest(
            f"""
PACKAGES = {{
    {{ spec = "local.simple@v1", source = "{self.lua_path(self.specs_dir)}/simple.lua" }},
}}
"""
        )

        # capture_output=True makes stderr a pipe, not a TTY
        result = self.run_sync(manifest)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        # Check that no ANSI codes are present in piped output
        self.assertNotIn(
            "\x1b[", result.stderr, "Expected no ANSI codes when stderr is piped"
        )

    def test_completed_sections_not_repeated_in_dumb_mode(self):
        """Completed sections don't repeat in fallback output."""
        manifest = self.create_manifest(
            f"""
PACKAGES = {{
    {{ spec = "local.fast@v1", source = "{self.lua_path(self.specs_dir)}/fast.lua", setup = {{ "main" }} }},
    {{ spec = "local.slow@v1", source = "{self.lua_path(self.specs_dir)}/slow.lua", setup = {{ "main" }} }},
}}
"""
        )

        result = self.run_sync(
            manifest,
            env={
                "TERM": "dumb",
                "ENVY_TEST_FALLBACK_THROTTLE_MS": "200",
            },
        )
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        lines = result.stderr.splitlines()
        slow_lines = [l for l in lines if "local.slow@v1" in l]
        fast_done_lines = [
            l for l in lines if "local.fast@v1" in l and "done" in l.lower()
        ]

        # Slow package should have multiple progress updates (renderer is active)
        self.assertGreater(
            len(slow_lines), 1, f"Expected multiple slow lines, got: {slow_lines}"
        )
        # Fast package's "done" should appear at most once
        self.assertLessEqual(
            len(fast_done_lines),
            1,
            f"Expected at most 1 fast-done line, got: {fast_done_lines}",
        )


if __name__ == "__main__":
    unittest.main()


# What sudo does: prompt on the controlling terminal and read the answer back from it,
# never touching the stdin envy handed the child. Beside it, a package that keeps the
# renderer busy for the whole exchange. The prompt is spelled in two halves so it does
# not also match envy's echo of the command, which would end the wait a frame early.
SPEC_INTERACTIVE = """IDENTITY = "local.interactive@v1"
USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options) return false end,
    INSTALL = function(pkg_dir, options)
      envy.run('m=PASS; printf "%sWORD: " "$m" > /dev/tty; read -r pw < /dev/tty; echo "SAW:$pw"',
               { interactive = true })
    end,
  },
}

"""

SPEC_NOISY = """IDENTITY = "local.noisy@v1"
USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options) return false end,
    INSTALL = function(pkg_dir, options)
      envy.run("sleep 3")
    end,
  },
}

"""


@unittest.skipIf(sys.platform == "win32", "no pty on Windows")
class TestInteractiveHandoff(EnvyTestCase):
    """A child that wants the terminal gets all of it, for as long as it holds it.

    `sudo` asking for a password is the case, and it reads `/dev/tty` rather than stdin --
    so this needs a *controlling* terminal, which `pty.openpty` alone does not give.
    A neighbouring package's spinner repainting over the prompt, or its DEBUG lines
    landing in the middle of one, is at best ugly and at worst eats the keystrokes.
    """

    envy_watchdog_timeout = 90

    # Comfortably inside the wait, and twenty render frames at 33ms: a renderer that has
    # not stopped cannot stay quiet this long -- the spinner alone repaints every frame.
    QUIET_WINDOW_S = 0.66

    def setUp(self):
        super().setUp()
        self.test_dir = self.make_temp_dir("test_dir")
        self.specs_dir = self.test_dir / "specs"
        self.specs_dir.mkdir()
        (self.specs_dir / "interactive.lua").write_text(SPEC_INTERACTIVE)
        (self.specs_dir / "noisy.lua").write_text(SPEC_NOISY)

    def _spawn_on_controlling_tty(self, manifest):
        """envy on a pty that is its session's controlling terminal, as a shell gives it.

        `pty.fork` is what buys that: `openpty` plus an inherited fd leaves /dev/tty
        unopenable in the child, and the prompt this test waits for never arrives.
        """
        import pty

        argv = [
            str(self.envy),
            "--cache-root",
            str(self.cache_root),
            "--verbose",  # the log stream is the half of the pause that used to leak
            "install",
            "--manifest",
            str(manifest),
        ]
        pid, fd = pty.fork()
        if pid == 0:
            try:
                os.chdir(str(self.project_root))
                os.environ["TERM"] = "xterm-256color"
                os.execv(argv[0], argv)
            finally:
                os._exit(127)  # exec failed; never return into the test runner
        return pid, fd

    def test_the_live_region_stops_while_a_child_owns_the_terminal(self):
        manifest = self.test_dir / "envy.lua"
        entry = '  {{ spec = "{0}", source = "{1}", setup = {{ "main" }} }},\n'
        manifest.write_text(
            make_manifest(
                "PACKAGES = {\n"
                + entry.format(
                    "local.interactive@v1", (self.specs_dir / "interactive.lua").as_posix()
                )
                + entry.format("local.noisy@v1", (self.specs_dir / "noisy.lua").as_posix())
                + "}\n"
            ),
            encoding="utf-8",
        )

        import select
        import time

        pid, fd = self._spawn_on_controlling_tty(manifest)
        painted, seen = b"", b""
        try:
            deadline = time.monotonic() + 60.0
            while b"PASSWORD: " not in seen:
                remaining = deadline - time.monotonic()
                if remaining <= 0 or not select.select([fd], [], [], remaining)[0]:
                    self.fail(f"never saw the prompt; got: {seen!r}")
                seen += os.read(fd, 65536)

            # The child is blocked on the answer. Anything arriving now is envy's.
            deadline = time.monotonic() + self.QUIET_WINDOW_S
            while (remaining := deadline - time.monotonic()) > 0:
                if select.select([fd], [], [], remaining)[0]:
                    painted += os.read(fd, 65536)

            os.write(fd, b"hunter2\n")
            deadline = time.monotonic() + 60.0
            while b"SAW:hunter2" not in seen:
                remaining = deadline - time.monotonic()
                if remaining <= 0 or not select.select([fd], [], [], remaining)[0]:
                    self.fail(f"the child never read its answer; got: {seen!r}")
                seen += os.read(fd, 65536)

            while select.select([fd], [], [], 60.0)[0] and os.read(fd, 65536):
                pass
        finally:
            _, status = os.waitpid(pid, 0)
            os.close(fd)

        self.assertEqual(0, os.waitstatus_to_exitcode(status), seen)
        self.assertEqual(b"", painted, f"envy painted over a waiting child: {painted!r}")
