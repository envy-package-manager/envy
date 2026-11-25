"""Cache entry locking across envy processes, driven through real `envy install` runs.

One cache-managed spec whose INSTALL logs whether it found pkg/ clean, then -- if the
test's hold marker exists -- announces itself and spins until a release marker appears.
It polls with envy.exists, which holds no handle a Windows delete could trip on. Every
race is a handshake on markers and the waiting row, never a delay; outcomes come from
each run's trace. Entry layout, lock naming, and clearing a stale pkg/ in one
process are unit-tested in src/cache_tests.cpp.
"""

import subprocess
import time
import unittest
from collections import Counter
from pathlib import Path

from . import test_config
from .env import EnvyTestCase
from .trace_parser import TraceParser

IDENTITY = "local.locked@v1"

# A spin rather than envy.run: a killed envy then leaves no child holding its pipes.
SPEC = """IDENTITY = "@IDENTITY@"
FETCH = function() end
INSTALL = function(install_dir)
  local log = io.open("@LOG@", "a")
  log:write(envy.exists(install_dir .. "/stale.txt") and "stale\\n" or "clean\\n")
  log:close()
  io.open(install_dir .. "/stale.txt", "w"):close()
  if envy.exists("@HOLD@") then
    io.open("@ENTERED@", "w"):close()
    while not envy.exists("@RELEASE@") do end
  end
  if envy.exists("@FAIL@") then error("install failed on purpose") end
end
"""


class Run:
    """One backgrounded `envy install`, its trace, and its stderr file."""

    def __init__(self, proc: subprocess.Popen, trace: Path, err: Path):
        self.proc, self.trace, self.err = proc, trace, err

    def events(self, name: str):
        return [e for e in TraceParser(self.trace).parse() if e.event == name]

    def mine(self, name: str):
        return [e for e in self.events(name) if e.spec == IDENTITY]


class TestCacheLocking(EnvyTestCase):
    envy_watchdog_timeout = 30

    def setUp(self):
        super().setUp()
        self.log = self.work / "install.log"
        self.hold = self.work / "hold"
        self.release = self.work / "release"
        self.fail = self.work / "fail"
        self.entered = self.work / "entered"
        spec = SPEC
        for token, value in (
            ("@IDENTITY@", IDENTITY),
            ("@LOG@", self.lua_path(self.log)),
            ("@HOLD@", self.lua_path(self.hold)),
            ("@RELEASE@", self.lua_path(self.release)),
            ("@FAIL@", self.lua_path(self.fail)),
            ("@ENTERED@", self.lua_path(self.entered)),
        ):
            spec = spec.replace(token, value)
        self.write_spec("locked.lua", spec)
        self.manifest = test_config.write_spec_manifest(
            self.work, [(IDENTITY, "locked.lua")]
        )
        self._runs = 0

    def start(self) -> Run:
        """Launch an install; its stderr goes to a file so it can be read mid-run.

        TERM=dumb with no throttle makes the fallback renderer print the waiting row the
        moment the run blocks on another envy's lock.
        """
        self._runs += 1
        trace = self.work / f"run-{self._runs}.jsonl"
        err_path = self.work / f"run-{self._runs}.err"
        env = test_config.get_test_env()
        env.update(TERM="dumb", ENVY_TEST_FALLBACK_THROTTLE_MS="0")
        with open(err_path, "w", encoding="utf-8") as err:
            proc = test_config.popen(
                [str(self.envy), "--cache-root", str(self.cache_root)]
                + [f"--trace=file:{trace}", "install", "--manifest", str(self.manifest)],
                stdout=subprocess.DEVNULL,
                stderr=err,
                env=env,
            )
        self.addCleanup(lambda: proc.poll() is None and proc.kill())
        return Run(proc, trace, err_path)

    def finish(self, run: Run) -> int:
        return run.proc.wait(timeout=60)

    def await_condition(self, predicate, run: Run, what: str, timeout: float = 30.0):
        deadline = time.monotonic() + timeout
        while not predicate():
            self.assertIsNone(run.proc.poll(), f"{what}: run exited first")
            self.assertLess(time.monotonic(), deadline, f"{what}: timed out")
            time.sleep(0.02)

    def hold_in_install(self) -> Run:
        """A run parked inside INSTALL, holding the entry lock, until release appears."""
        self.hold.touch()
        holder = self.start()
        self.await_condition(self.entered.exists, holder, "holder never entered INSTALL")
        return holder

    def start_blocked_waiter(self) -> Run:
        waiter = self.start()
        row = f"waiting for another envy to release {IDENTITY}"
        self.await_condition(
            lambda: row in waiter.err.read_text(encoding="utf-8", errors="replace"),
            waiter,
            "waiter drew no waiting row",
        )
        return waiter

    def installs(self) -> list[str]:
        """What each INSTALL that ran found in pkg/, in order."""
        return self.log.read_text(encoding="utf-8").split()

    def test_concurrent_installs_stage_the_entry_once(self):
        """Five racing runs: one stages, four hit, and every lock taken is released."""
        runs = [self.start() for _ in range(5)]
        for run in runs:
            self.assertEqual(0, self.finish(run), run.err.read_text(encoding="utf-8"))

        self.assertEqual(["clean"], self.installs())
        self.assertEqual(1, sum(len(r.mine("cache_miss")) for r in runs))
        self.assertEqual(4, sum(len(r.mine("cache_hit")) for r in runs))
        finalized = [e for r in runs for e in r.mine("cache_entry_finalized")]
        self.assertEqual(["completed"], [e.raw["disposition"] for e in finalized])

        def paths(name):
            return Counter(e.raw["lock_path"] for r in runs for e in r.events(name))

        self.assertEqual(paths("lock_acquired"), paths("lock_released"))

    def test_a_waiter_rechecks_and_takes_the_finished_entry(self):
        """Blocked on the holder's lock, the waiter finds the entry complete once it gets
        the lock, and installs nothing."""
        holder = self.hold_in_install()
        waiter = self.start_blocked_waiter()
        self.release.touch()

        self.assertEqual(0, self.finish(holder), holder.err.read_text(encoding="utf-8"))
        self.assertEqual(0, self.finish(waiter), waiter.err.read_text(encoding="utf-8"))
        self.assertEqual(["clean"], self.installs())
        self.assertEqual([], waiter.mine("cache_miss"))
        self.assertEqual([False], [e.raw["fast_path"] for e in waiter.mine("cache_hit")])

    def test_a_killed_holder_frees_the_lock_and_its_partial_install_is_redone(self):
        """The OS releases a killed run's lock; the waiter then stages the entry over
        what the dead run left in pkg/, starting clean."""
        holder = self.hold_in_install()
        waiter = self.start_blocked_waiter()
        holder.proc.kill()
        self.assertNotEqual(0, self.finish(holder))
        self.release.touch()

        self.assertEqual(0, self.finish(waiter), waiter.err.read_text(encoding="utf-8"))
        self.assertEqual(["clean", "clean"], self.installs())
        self.assertEqual(1, len(waiter.mine("cache_miss")))
        finalized = waiter.mine("cache_entry_finalized")
        self.assertEqual(["completed"], [e.raw["disposition"] for e in finalized])

    def test_a_failed_install_leaves_no_entry_and_the_next_run_starts_clean(self):
        self.fail.touch()
        failed = self.start()
        self.assertNotEqual(0, self.finish(failed))
        finalized = failed.mine("cache_entry_finalized")
        self.assertEqual(1, len(finalized))
        self.assertNotEqual("completed", finalized[0].raw["disposition"])

        self.fail.unlink()
        retry = self.start()
        self.assertEqual(0, self.finish(retry), retry.err.read_text(encoding="utf-8"))
        self.assertEqual(["clean", "clean"], self.installs())


if __name__ == "__main__":
    unittest.main()
