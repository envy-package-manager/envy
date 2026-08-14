"""Functional tests for cache locking, staging, and crash recovery.

Uses the functional tester's `cache-test` commands, which drive
cache::ensure_pkg / ensure_spec directly so two processes can be choreographed
around a single lock via file barriers. The commands print nothing; what
happened is read back from the trace stream (see CacheProc).

Single-process layout and path-construction behavior is covered by the unit
tests in src/cache_tests.cpp, not here.
"""

import shutil
import subprocess
import tempfile
import time
import uuid
from pathlib import Path
import unittest

from . import test_config
from .env import EnvyTestCase
from .trace_parser import TraceParser


class CacheProc:
    """A cache-test subprocess plus the trace file only it writes to.

    The cache commands emit no stdout; every observation below is derived from
    the same trace events production code emits. `cache_miss` is the signal
    that this process took the lock -- cache.cpp emits it exactly on the path
    that hands back a scoped_entry_lock.
    """

    def __init__(self, proc, trace_file):
        self._proc = proc
        self._trace_file = trace_file
        self._events = None

    def communicate(self):
        return self._proc.communicate()

    @property
    def returncode(self):
        return self._proc.returncode

    def events(self):
        """Parse this process's trace. Crashed processes lose queued events."""
        if self._events is None:
            self._events = (
                TraceParser(self._trace_file).parse()
                if self._trace_file.exists()
                else []
            )
        return self._events

    def _first(self, name):
        return next((e for e in self.events() if e.event == name), None)

    @property
    def locked(self):
        """True when this process staged the entry (i.e. took the lock)."""
        return self._first("cache_miss") is not None

    @property
    def fast_path(self):
        """True when the entry was already complete before any lock attempt."""
        hit = self._first("cache_hit")
        return bool(hit and hit.raw["fast_path"])

    @property
    def entry_path(self):
        finalized = self._first("cache_entry_finalized")
        if finalized:
            return Path(finalized.raw["entry_dir"])
        hit = self._first("cache_hit")
        return Path(hit.raw["pkg_path"]).parent if hit else None

    @property
    def pkg_path(self):
        hit = self._first("cache_hit")
        if hit:
            return Path(hit.raw["pkg_path"])
        entry = self.entry_path
        return entry / "pkg" if entry else None

    @property
    def lock_file(self):
        acquired = self._first("lock_acquired")
        return Path(acquired.raw["lock_path"]) if acquired else None


class CacheTestBase(EnvyTestCase):
    """Spawns cache-test subprocesses with per-test barrier and trace dirs."""

    def setUp(self):
        super().setUp()
        self.test_id = str(uuid.uuid4())
        self.barrier_dir = self.make_temp_dir("barrier_dir")
        # Each test gets its own trace directory, each process gets unique file
        self.trace_dir = self.make_temp_dir("trace_dir").resolve()

    def tearDown(self):
        shutil.rmtree(self.barrier_dir, ignore_errors=True)
        shutil.rmtree(self.trace_dir, ignore_errors=True)
        super().tearDown()

    def run_cache_cmd(
        self,
        *args,
        cache_root=None,
        barrier_signal=None,
        barrier_wait=None,
        barrier_signal_after=None,
        barrier_wait_after=None,
        crash_after_ms=None,
        fail_before_complete=False,
    ):
        """Spawn one cache-test process; returns a CacheProc."""
        trace_file = self.trace_dir / f"proc-{uuid.uuid4()}.jsonl"
        cmd = [
            str(self.envy),
            f"--cache-root={cache_root or self.cache_root}",
            f"--trace=file:{trace_file}",
            "cache-test",
            *args,
            f"--test-id={self.test_id}",
            f"--barrier-dir={self.barrier_dir}",
        ]
        for flag, value in (
            ("--barrier-signal", barrier_signal),
            ("--barrier-wait", barrier_wait),
            ("--barrier-signal-after", barrier_signal_after),
            ("--barrier-wait-after", barrier_wait_after),
            ("--crash-after", crash_after_ms),
        ):
            if value is not None:
                cmd.append(f"{flag}={value}")
        if fail_before_complete:
            cmd.append("--fail-before-complete")

        return CacheProc(
            test_config.popen(
                cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
            ),
            trace_file,
        )

    def all_trace_events(self):
        """Parse every per-process trace file written by this test.

        Only cleanly-exiting processes are parseable (crash-path processes lose
        queued events); callers targeting crashes should not use this.
        """
        events = []
        for trace_file in sorted(self.trace_dir.glob("proc-*.jsonl")):
            events.extend(TraceParser(trace_file).parse())
        return events

    def assert_lock_pairing(self):
        """Every lock_acquired has a matching lock_released with sane durations."""
        events = self.all_trace_events()
        acquired = [e for e in events if e.event == "lock_acquired"]
        released = [e for e in events if e.event == "lock_released"]
        self.assertEqual(
            sorted(e.raw["lock_path"] for e in acquired),
            sorted(e.raw["lock_path"] for e in released),
            "lock_acquired/lock_released must pair per lock_path",
        )
        for e in acquired:
            self.assertGreaterEqual(e.raw["wait_duration_ms"], 0)
        for e in released:
            self.assertGreaterEqual(e.raw["hold_duration_ms"], 0)

    def await_barrier(self, name, timeout=10.0):
        """Block until another process signals the named barrier."""
        marker = self.barrier_dir / name
        deadline = time.monotonic() + timeout
        while not marker.exists():
            self.assertLess(time.monotonic(), deadline, f"barrier '{name}' never fired")
            time.sleep(0.01)


class TestCacheLockingAndConcurrency(CacheTestBase):
    """Lock acquisition and concurrency tests."""

    def test_ensure_asset_first_time(self):
        """Cold package acquires the lock and publishes pkg/ on completion."""
        proc = self.run_cache_cmd(
            "ensure-package", "gcc", "darwin", "arm64", "a1b2c3d4"
        )
        proc.communicate()

        self.assertEqual(proc.returncode, 0)
        self.assertTrue(proc.locked)

        entry = self.cache_root / "packages" / "gcc" / "darwin-arm64-blake3-a1b2c3d4"
        self.assertEqual(proc.entry_path, entry)
        self.assertTrue((entry / "envy-complete").exists())
        self.assertTrue((entry / "pkg").exists())
        self.assertFalse((entry / "work").exists())

    def test_ensure_asset_already_complete(self):
        """Request complete package, returns final path immediately without lock."""
        # Pre-populate cache
        entry = self.cache_root / "packages" / "gcc" / "darwin-arm64-blake3-complete1"
        entry.mkdir(parents=True)
        (entry / "envy-complete").touch()

        proc = self.run_cache_cmd(
            "ensure-package", "gcc", "darwin", "arm64", "complete1"
        )
        proc.communicate()

        self.assertFalse(proc.locked)
        self.assertTrue(proc.fast_path)
        self.assertEqual(proc.entry_path, entry)
        self.assertEqual(proc.pkg_path, entry / "pkg")

    def test_concurrent_ensure_same_asset(self):
        """Two processes request same package—one stages, other blocks then finds complete."""
        # Process A: signal immediately, then do work (no wait - completes freely)
        proc_a = self.run_cache_cmd(
            "ensure-package",
            "gcc",
            "darwin",
            "arm64",
            "concurrent1",
            barrier_signal="a_ready",
        )

        # Process B: wait for A to be ready, then attempt (will find A's completed entry)
        proc_b = self.run_cache_cmd(
            "ensure-package",
            "gcc",
            "darwin",
            "arm64",
            "concurrent1",
            barrier_wait="a_ready",
        )

        proc_a.communicate()
        proc_b.communicate()

        # Critical invariant: exactly one process stages, one gets cache hit
        self.assertEqual({proc_a.locked, proc_b.locked}, {True, False})
        # Fast path is timing-dependent (directory flush affects timing)
        # The process that locked should always have fast_path=false
        self.assertFalse(proc_a.fast_path if proc_a.locked else proc_b.fast_path)

        # Trace contract: exactly one racer misses (stages), the other hits,
        # and every lock acquisition pairs with a release.
        events = self.all_trace_events()
        misses = [e for e in events if e.event == "cache_miss"]
        hits = [e for e in events if e.event == "cache_hit"]
        self.assertEqual(len(misses), 1, "exactly one racer should stage")
        self.assertEqual(len(hits), 1, "exactly one racer should hit")
        finalized = [e for e in events if e.event == "cache_entry_finalized"]
        self.assertEqual(len(finalized), 1)
        self.assertEqual(finalized[0].raw["disposition"], "completed")
        self.assert_lock_pairing()

    def test_ensure_spec_vs_ensure_asset_different_locks(self):
        """Verify spec and package locks don't conflict."""
        # Start package lock in background
        proc_asset = self.run_cache_cmd(
            "ensure-package",
            "gcc",
            "darwin",
            "arm64",
            "locktest",
            barrier_signal="package_locked",
            barrier_wait="recipe_checked",
        )

        # Wait for package lock, then try spec lock
        proc_spec = self.run_cache_cmd(
            "ensure-spec",
            "envy.cmake@v1",
            barrier_wait="package_locked",
            barrier_signal="recipe_checked",
        )

        # Both should succeed without blocking each other
        proc_asset.communicate()
        proc_spec.communicate()
        self.assertEqual(proc_asset.returncode, 0)
        self.assertEqual(proc_spec.returncode, 0)


class TestStagingAndCommit(CacheTestBase):
    """Staging directory and commit behavior tests."""

    def test_staging_auto_created(self):
        """Lock returned, pkg/ created for install, work/ cleaned on completion."""
        proc = self.run_cache_cmd(
            "ensure-package", "gcc", "darwin", "arm64", "staging1"
        )
        proc.communicate()

        self.assertTrue(proc.locked)

        entry = self.cache_root / "packages" / "gcc" / "darwin-arm64-blake3-staging1"
        self.assertTrue((entry / "envy-complete").exists())
        self.assertTrue((entry / "pkg").exists())
        self.assertFalse((entry / "work").exists())

    def test_mark_complete_commits_on_exit(self):
        """Call mark_complete(), verify pkg/ exists and marker written."""
        proc = self.run_cache_cmd("ensure-package", "gcc", "darwin", "arm64", "commit1")
        proc.communicate()

        self.assertTrue(proc.locked)

        entry = self.cache_root / "packages" / "gcc" / "darwin-arm64-blake3-commit1"
        self.assertTrue((entry / "envy-complete").exists())
        self.assertTrue((entry / "pkg").exists())
        self.assertFalse((entry / "work").exists())

    def test_no_mark_complete_abandons_staging(self):
        """Lock destructs without mark_complete(), staging abandoned."""
        proc = self.run_cache_cmd(
            "ensure-package",
            "gcc",
            "darwin",
            "arm64",
            "abandon1",
            fail_before_complete=True,
        )
        proc.communicate()

        entry = self.cache_root / "packages" / "gcc" / "darwin-arm64-blake3-abandon1"
        self.assertFalse((entry / "envy-complete").exists())
        # The entry is still finalized, just not as a completed one.
        finalized = [
            e for e in proc.events() if e.event == "cache_entry_finalized"
        ]
        self.assertEqual(len(finalized), 1)
        self.assertNotEqual(finalized[0].raw["disposition"], "completed")


class TestCrashRecovery(CacheTestBase):
    """Crash recovery and stale staging cleanup tests."""

    def test_stale_inprogress_cleaned(self):
        """Kill process mid-install, next ensure wipes stale pkg/ and reinstalls."""
        entry = self.cache_root / "packages" / "gcc" / "darwin-arm64-blake3-crash1"
        pkg_dir = entry / "pkg"

        # Process A crashes after acquiring lock
        proc_a = self.run_cache_cmd(
            "ensure-package",
            "gcc",
            "darwin",
            "arm64",
            "crash1",
            barrier_signal="locked",
            crash_after_ms=100,
        )

        # Wait for crash
        proc_a.communicate()
        self.assertNotEqual(proc_a.returncode, 0)

        # Verify stale pkg/ exists from crashed process
        self.assertTrue(pkg_dir.exists())

        # Plant a sentinel to prove process B wipes stale content
        sentinel = pkg_dir / "stale_sentinel.txt"
        sentinel.write_text("stale")

        # Process B cleans up and succeeds
        proc_b = self.run_cache_cmd(
            "ensure-package", "gcc", "darwin", "arm64", "crash1", barrier_wait="locked"
        )
        proc_b.communicate()

        self.assertTrue(proc_b.locked)
        # pkg/ exists (successful install) but stale sentinel is gone
        self.assertTrue(pkg_dir.exists())
        self.assertFalse(sentinel.exists())
        self.assertTrue((entry / "envy-complete").exists())

    def test_lock_released_on_crash(self):
        """Process crashes holding lock, OS releases lock, next process acquires."""
        # Process A crashes while holding lock
        proc_a = self.run_cache_cmd(
            "ensure-package",
            "gcc",
            "darwin",
            "arm64",
            "lockcrash",
            barrier_signal="a_locked",
            crash_after_ms=50,
        )

        # Process B waits for A to get lock, then tries after crash
        proc_b = self.run_cache_cmd(
            "ensure-package",
            "gcc",
            "darwin",
            "arm64",
            "lockcrash",
            barrier_wait="a_locked",
        )

        proc_a.communicate()
        self.assertNotEqual(proc_a.returncode, 0)

        proc_b.communicate()

        # B successfully acquired lock after A crashed
        self.assertTrue(proc_b.locked)
        self.assertEqual(proc_b.returncode, 0)


class TestLockFileLifecycle(CacheTestBase):
    """Lock file creation and removal tests."""

    def test_lock_file_created_and_removed(self):
        """Lock acquired, verify lock file exists in locks/, then deleted on release."""
        # Process holds lock, signals AFTER acquiring lock
        proc = self.run_cache_cmd(
            "ensure-package",
            "gcc",
            "darwin",
            "arm64",
            "lockfile1",
            barrier_signal_after="locked",
            barrier_wait_after="check_done",
        )

        self.await_barrier("locked")

        lock_file = (
            self.cache_root
            / "locks"
            / "packages.gcc-darwin-arm64-blake3-lockfile1.lock"
        )
        self.assertTrue(lock_file.exists())

        # Let process finish
        (self.barrier_dir / "check_done").touch()
        proc.communicate()

        # Lock file should be removed
        self.assertFalse(lock_file.exists())

    def test_lock_file_naming_asset(self):
        """Verify package lock path matches packages.{identity}-{platform}-{arch}-blake3-{hash}.lock."""
        proc = self.run_cache_cmd("ensure-package", "gcc", "darwin", "arm64", "abc123")
        proc.communicate()

        self.assertEqual(
            proc.lock_file.name, "packages.gcc-darwin-arm64-blake3-abc123.lock"
        )

    def test_lock_file_naming_spec(self):
        """Spec locks carry the source key, so one identity can hold several."""
        proc = self.run_cache_cmd("ensure-spec", "envy.cmake@v1")
        proc.communicate()

        name = proc.lock_file.name
        self.assertTrue(name.startswith("spec.envy.cmake@v1.blake3-"), name)
        self.assertTrue(name.endswith(".lock"), name)

    def test_lock_file_naming_spec_differs_by_source(self):
        """Two sources for one identity take different locks, so neither blocks the other."""
        first = self.run_cache_cmd("ensure-spec", "envy.cmake@v1", "--source", "url-a")
        first.communicate()
        second = self.run_cache_cmd("ensure-spec", "envy.cmake@v1", "--source", "url-b")
        second.communicate()

        self.assertNotEqual(first.lock_file.name, second.lock_file.name)


class TestEdgeCases(CacheTestBase):
    """Edge cases and corner scenarios."""

    def test_recheck_after_lock_wait(self):
        """Process B waits on lock, A completes while waiting, B rechecks and finds complete."""
        # Process A: signal after acquiring lock, complete freely
        proc_a = self.run_cache_cmd(
            "ensure-package",
            "gcc",
            "darwin",
            "arm64",
            "recheck1",
            barrier_signal_after="a_ready",
        )

        # Process B: wait for A to acquire lock, then attempt (will find complete)
        proc_b = self.run_cache_cmd(
            "ensure-package",
            "gcc",
            "darwin",
            "arm64",
            "recheck1",
            barrier_wait="a_ready",
        )

        proc_a.communicate()
        proc_b.communicate()

        # Critical invariant: process A must stage, B must not
        self.assertTrue(proc_a.locked)
        self.assertFalse(proc_a.fast_path)
        # Fast path is timing-dependent - B might see complete before or after
        # waiting for the lock, but either way it does not stage.
        self.assertFalse(proc_b.locked)

    def test_asset_without_marker_requires_lock(self):
        """Existing package directory without marker still forces staging."""
        entry = self.cache_root / "packages" / "gcc" / "darwin-arm64-blake3-raw1"
        (entry / "pkg").mkdir(parents=True, exist_ok=True)

        proc = self.run_cache_cmd("ensure-package", "gcc", "darwin", "arm64", "raw1")
        proc.communicate()

        self.assertTrue(proc.locked)

    def test_empty_staging_committed(self):
        """Create staging but write nothing, call mark_complete()—verify commit happens."""
        proc = self.run_cache_cmd("ensure-package", "gcc", "darwin", "arm64", "empty1")
        proc.communicate()

        self.assertTrue(proc.locked)

        # Entry should be complete even though no files staged
        entry = self.cache_root / "packages" / "gcc" / "darwin-arm64-blake3-empty1"
        self.assertTrue((entry / "envy-complete").exists())

    def test_multiple_assets_same_identity_different_platforms(self):
        """Same identity but different platform/arch/hash → different cache entries."""
        proc_darwin = self.run_cache_cmd(
            "ensure-package", "gcc", "darwin", "arm64", "multi1"
        )
        proc_linux = self.run_cache_cmd(
            "ensure-package", "gcc", "linux", "x86_64", "multi1"
        )

        proc_darwin.communicate()
        proc_linux.communicate()

        self.assertNotEqual(proc_darwin.entry_path, proc_linux.entry_path)
        self.assertTrue((proc_darwin.entry_path / "envy-complete").exists())
        self.assertTrue((proc_linux.entry_path / "envy-complete").exists())

    def test_ensure_with_custom_cache_root(self):
        """Pass custom root to cache constructor, verify all paths relative to custom root."""
        custom_root = self.make_temp_dir("custom_root")
        proc = self.run_cache_cmd(
            "ensure-package",
            "gcc",
            "darwin",
            "arm64",
            "custom1",
            cache_root=custom_root,
        )
        proc.communicate()

        self.assertTrue(proc.entry_path.is_relative_to(custom_root))
class TestSubprocessConcurrency(CacheTestBase):
    """Integration tests with real subprocess spawning."""

    def test_subprocess_concurrent_ensure(self):
        """Spawn multiple envy subprocesses requesting same package—one stages, others wait."""
        procs = [
            self.run_cache_cmd("ensure-package", "gcc", "darwin", "arm64", "many1")
            for _ in range(5)
        ]
        for proc in procs:
            proc.communicate()

        # Critical invariant: exactly one process stages the package
        self.assertEqual(sum(1 for p in procs if p.locked), 1)
        # The other 4 processes should all get cache hits
        self.assertEqual(sum(1 for p in procs if not p.locked), 4)
        # Fast path count is timing-dependent (directory flush affects timing):
        # 0 (all hit after taking the lock) through 4 (all hit before).
        self.assertLessEqual(sum(1 for p in procs if p.fast_path), 4)

    def test_sigkill_recovery(self):
        """SIGKILL process mid-install, verify next process wipes stale pkg/ and succeeds."""
        entry = self.cache_root / "packages" / "gcc" / "darwin-arm64-blake3-sigkill1"
        pkg_dir = entry / "pkg"

        proc_a = self.run_cache_cmd(
            "ensure-package",
            "gcc",
            "darwin",
            "arm64",
            "sigkill1",
            crash_after_ms=50,
        )

        proc_a.communicate()
        self.assertNotEqual(proc_a.returncode, 0)

        # Verify stale pkg/ exists from killed process
        self.assertTrue(pkg_dir.exists())

        # Plant a sentinel to prove recovery wipes stale content
        sentinel = pkg_dir / "stale_sentinel.txt"
        sentinel.write_text("stale")

        # Recovery
        proc_b = self.run_cache_cmd(
            "ensure-package", "gcc", "darwin", "arm64", "sigkill1"
        )
        proc_b.communicate()

        self.assertTrue(proc_b.locked)
        # pkg/ exists (successful install) but stale sentinel is gone
        self.assertTrue(pkg_dir.exists())
        self.assertFalse(sentinel.exists())
        self.assertTrue((entry / "envy-complete").exists())


if __name__ == "__main__":
    unittest.main()
