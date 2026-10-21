"""Cache entry locks under cross-thread and cross-process contention.

The test plays a second envy by taking the same fcntl locks; lock paths come from a
discovery run's trace, and marker files sequence the two sides.
"""

import hashlib
import io
import os
import subprocess
import sys
import tarfile
import threading
import time
import unittest
from pathlib import Path

from . import test_config
from .env import EnvyTestCase
from .trace_parser import TraceParser

if sys.platform != "win32":
    import fcntl

USER_MANAGED = (  # no payload, so a spec lock is its only lock; the pair never runs
    "USER_MANAGED = true\n"
    "SETUP = { main = { CHECK = function() return true end, "
    "INSTALL = function() end } }\n"
)


def lock_exclusive(path: Path) -> int:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(path, os.O_CREAT | os.O_RDWR, 0o666)
    fcntl.lockf(fd, fcntl.LOCK_EX)
    return fd


@unittest.skipIf(sys.platform == "win32", "fcntl locks are POSIX")
class TestCacheLockContention(EnvyTestCase):
    envy_watchdog_timeout = 60

    def setUp(self):
        super().setUp()
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode="w:gz") as tar:
            data = b"payload\n"
            info = tarfile.TarInfo(name="root/file.txt")
            info.size = len(data)
            tar.addfile(info, io.BytesIO(data))
        self.archive = self.work / "payload.tar.gz"
        self.archive.write_bytes(buf.getvalue())
        self.archive_sha = hashlib.sha256(buf.getvalue()).hexdigest()
        self.go = self.work / "go"

    def spec(self, name: str, build: str = "", deps: str = "") -> Path:
        """A cache-managed local.<name>@v1 whose BUILD runs the Lua in `build`."""
        return self.write_spec(
            f"{name}.lua",
            f'IDENTITY = "local.{name}@v1"\n'
            f"{deps}\n"
            f'FETCH = {{ source = "{self.lua_path(self.archive)}", '
            f'sha256 = "{self.archive_sha}" }}\n'
            "STAGE = { strip = 1 }\n"
            f"BUILD = function() {build} end\n",
        )

    def after_go(self, seconds: float = 0) -> str:
        """BUILD Lua that waits for the test's go marker, then sleeps."""
        go = self.lua_path(self.go)
        return f'envy.run([[while [ ! -e "{go}" ]; do sleep 0.02; done; sleep {seconds}]])'

    def entry(self, name: str, extra: str = "") -> str:
        path = self.lua_path(self.work / f"{name}.lua")
        return f'{{ spec = "local.{name}@v1", source = "{path}"{extra} }}'

    def manifest_of(self, *entries: str) -> Path:
        return self.write_manifest("PACKAGES = {\n" + ",\n".join(entries) + "\n}\n")

    def lock_names(self, manifest: Path) -> dict[str, str]:
        """identity -> lock file name, from an uncontended cold run."""
        cache = self.make_temp_dir("discovery-cache")
        trace = self.work / "discovery.jsonl"
        self.go.touch()
        result = test_config.run(
            [str(self.envy), "--cache-root", str(cache), f"--trace=file:{trace}"]
            + ["install", "--manifest", str(manifest)],
            capture_output=True,
            text=True,
        )
        self.go.unlink()
        self.assertEqual(result.returncode, 0, result.stderr)
        names = {}
        for e in TraceParser(trace).parse():
            if e.event == "lock_acquired":
                names.setdefault(e.spec, Path(e.raw["lock_path"]).name)
        return names

    def start(self, manifest: Path, trace: Path):
        return test_config.popen(
            [str(self.envy), "--cache-root", str(self.cache_root)]
            + [f"--trace=file:{trace}", "install", "--manifest", str(manifest)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

    @staticmethod
    def first_seq(trace: Path, event: str, spec: str):
        try:  # envy may still be writing it
            events = TraceParser(trace).parse()
        except Exception:
            return None
        return next((e.seq for e in events if e.event == event and e.spec == spec), None)

    def await_seq(self, trace: Path, event: str, spec: str, timeout: float = 10):
        deadline = time.monotonic() + timeout
        while (seq := self.first_seq(trace, event, spec)) is None:
            if time.monotonic() > deadline:
                return None
            time.sleep(0.02)
        return seq

    def test_cross_process_lock_cycle_is_not_a_deadlock(self):
        """envy holds b and asks for a while we hold a and wait on b: the kernel calls that
        a deadlock (one owner per process), though our b waiter never needs a."""
        self.spec("cyc_b", self.after_go(2))
        self.spec("cyc_c", self.after_go())
        cyc_c = self.entry("cyc_c", ', needed_by = "check"')  # cyc_a locks only after go
        self.spec("cyc_a", deps=f"DEPENDENCIES = {{ {cyc_c} }}")
        manifest = self.manifest_of(self.entry("cyc_a"), self.entry("cyc_b"))
        names = self.lock_names(manifest)
        locks = self.cache_root / "locks"

        trace = self.work / "trace.jsonl"
        proc = self.start(manifest, trace)
        self.assertIsNotNone(self.await_seq(trace, "lock_acquired", "local.cyc_b@v1"))

        fd_a = lock_exclusive(locks / names["local.cyc_a@v1"])
        got_b = threading.Event()

        def wait_on_b():  # parks behind envy's cyc_b, then lets go at once
            os.close(lock_exclusive(locks / names["local.cyc_b@v1"]))
            got_b.set()

        threading.Thread(target=wait_on_b, daemon=True).start()
        time.sleep(0.3)  # let the waiter park before envy's request closes the cycle
        self.go.touch()
        self.assertTrue(got_b.wait(30), "envy never released cyc_b")
        os.close(fd_a)

        _, err = proc.communicate(timeout=30)
        self.assertEqual(proc.returncode, 0, err)
        self.assertGreater(  # cyc_a waited out the cycle rather than skipping it
            self.first_seq(trace, "lock_acquired", "local.cyc_a@v1"),
            self.first_seq(trace, "lock_released", "local.cyc_b@v1"),
        )

    def test_waiting_on_one_entry_does_not_stall_other_locks(self):
        """While two option variants wait on a spec entry we hold, another spec's lock is
        still granted."""

        def sha_of(name: str) -> str:  # a sha256 routes a spec through the spec cache
            body = f'IDENTITY = "local.{name}@v1"\n' + USER_MANAGED
            path = self.write_spec(f"{name}.lua", body)
            return f', sha256 = "{hashlib.sha256(path.read_bytes()).hexdigest()}"'

        shared, late = sha_of("shared"), sha_of("late")
        self.write_spec(  # reaches for late's spec once the variants are parked on theirs
            "root.lua",
            'IDENTITY = "local.root@v1"\n'
            + USER_MANAGED
            + "local t0 = os.clock()\n"
            "while os.clock() - t0 < 0.5 do end\n"
            f"DEPENDENCIES = {{ {self.entry('late', late)} }}\n",
        )
        manifest = self.manifest_of(
            self.entry("shared", shared + ', options = { v = "a" }'),
            self.entry("shared", shared + ', options = { v = "b" }'),
            self.entry("root"),
        )
        names = self.lock_names(manifest)

        fd = lock_exclusive(self.cache_root / "locks" / names["local.shared@v1"])
        trace = self.work / "trace.jsonl"
        proc = self.start(manifest, trace)
        late_seq = self.await_seq(trace, "lock_acquired", "local.late@v1")
        os.close(fd)

        _, err = proc.communicate(timeout=30)
        self.assertIsNotNone(late_seq, "late's lock queued behind the held entry")
        self.assertEqual(proc.returncode, 0, err)
        shared_seq = self.first_seq(trace, "lock_acquired", "local.shared@v1")
        self.assertGreater(shared_seq, late_seq)  # late was granted while we held shared

    def test_failed_package_releases_its_lock_at_once(self):
        """A build failing between check and install releases its entry lock then, not when
        the run ends."""
        self.spec("fails", 'error("boom")')
        self.spec("slow", 'envy.run("sleep 2")')
        run = self.install(self.manifest_of(self.entry("fails"), self.entry("slow")))
        self.assertNotEqual(run.returncode, 0)

        def seq_of(event: str, spec: str):
            hits = (e.seq for e in run.events() if e.event == event and e.spec == spec)
            return next(hits, None)

        released = seq_of("lock_released", "local.fails@v1")
        self.assertIsNotNone(released, "the failed package never released its lock")
        self.assertLess(released, seq_of("pkg_outcome", "local.slow@v1"))


if __name__ == "__main__":
    unittest.main()
