"""Centralized environment for functional tests: scratch cache, scratch dirs, runners.

Every suite here used to hand-roll the same three things -- a `tempfile.mkdtemp`
cache root, a `shutil.rmtree` tearDown to match, and a bespoke wrapper around
`envy install --manifest ... --cache-root ...`. That is this module, once.

Subclass `EnvyTestCase` and you get:

    class TestThing(EnvyTestCase):
        def test_it(self):
            manifest = self.write_manifest('PACKAGES = { "local.tool@v1" }')
            run = self.install(manifest)
            self.assertEqual(0, run.returncode, run.stderr)
            self.assertTrue(self.spec_complete("local.tool@v1"))

No tearDown: every directory this hands out is registered with `addCleanup`, which
unittest runs even when setUp itself raises -- the failure mode a paired
setUp/tearDown silently leaks through. Runs are always traced, so assertions read
the machinery events envy already emits instead of scraping stdout.

`test_config` stays the low-level module (binary discovery, subprocess policy,
manifest strings); this builds the environment on top of it.
"""

from __future__ import annotations

import shutil
import subprocess
import tempfile
import threading
import unittest
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from . import test_config
from .trace_parser import TraceEvent, TraceParser


# Generous: this only has to be longer than an orderly shutdown ever takes, since
# exceeding it is reported as a failure rather than waited out.
SERVER_SHUTDOWN_TIMEOUT_S = 10.0


class QuietHTTPHandler(SimpleHTTPRequestHandler):
    """SimpleHTTPRequestHandler without per-request logging."""

    def log_message(self, format, *args):  # noqa: A003
        return


class EnvyRun:
    """One `envy` invocation: the process result plus the trace it wrote."""

    def __init__(self, result: subprocess.CompletedProcess, trace_path: Path):
        self.result = result
        self.returncode = result.returncode
        self.stdout = result.stdout
        self.stderr = result.stderr
        self.trace_path = trace_path
        self._events: list[TraceEvent] | None = None

    def events(
        self, name: str | None = None, spec: str | None = None
    ) -> list[TraceEvent]:
        """Trace events, optionally narrowed to one event type and/or subject."""
        if self._events is None:
            self._events = (
                TraceParser(self.trace_path).parse() if self.trace_path.exists() else []
            )
        return [
            e
            for e in self._events
            if (name is None or e.event == name) and (spec is None or e.spec == spec)
        ]

    def outcomes(self) -> dict[str, str]:
        """identity -> pkg_outcome, the per-package verdict envy traces."""
        return {e.spec: e.raw["outcome"] for e in self.events("pkg_outcome")}


class EnvyTestCase(unittest.TestCase):
    """Base for tests that drive `envy` against a scratch cache.

    Set `use_production_binary = True` on a subclass that must exercise the shipped
    artifact (bootstrap, re-exec, mirroring); everything else wants the default
    functional tester, which carries the sanitizers and the test-only subcommands.
    """

    use_production_binary = False

    def setUp(self):
        super().setUp()
        self.envy = (
            test_config.get_envy_production_executable()
            if self.use_production_binary
            else test_config.get_envy_executable()
        )
        self.project_root = Path(__file__).resolve().parent.parent
        self.cache_root = self.make_temp_dir("cache")
        self.work = self.make_temp_dir("work")
        self._run_count = 0

    # -- scratch space ------------------------------------------------------

    def make_temp_dir(self, label: str = "dir") -> Path:
        """A temp directory removed when the test ends, however it ends."""
        path = Path(tempfile.mkdtemp(prefix=f"envy-test-{label}-"))
        self.addCleanup(shutil.rmtree, path, ignore_errors=True)
        return path

    @staticmethod
    def lua_path(path: Path) -> str:
        """A path as a Lua string literal: forward slashes on every platform."""
        return Path(path).as_posix()

    # -- authoring ----------------------------------------------------------

    def write_manifest(
        self, packages_content: str, deploy: bool = False, directory: Path | None = None
    ) -> Path:
        manifest = (directory or self.work) / "envy.lua"
        manifest.write_text(
            test_config.make_manifest(packages_content, deploy=deploy), encoding="utf-8"
        )
        return manifest

    def write_spec(self, name: str, content: str, directory: Path | None = None) -> Path:
        """Write a spec file into the scratch tree; returns its path."""
        path = (directory or self.work) / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
        return path

    # -- running ------------------------------------------------------------

    def run_envy(self, *args, **kwargs) -> EnvyRun:
        """Run `envy <args>` against the scratch cache, tracing to its own file.

        Keyword arguments reach subprocess.run, so a test that needs a different
        cwd, an env, or a timeout passes it straight through.
        """
        self._run_count += 1
        trace = self.work / f"trace-{self._run_count}.jsonl"
        kwargs.setdefault("cwd", self.project_root)
        kwargs.setdefault("capture_output", True)
        kwargs.setdefault("text", True)
        result = test_config.run(
            [
                str(self.envy),
                "--cache-root",
                str(self.cache_root),
                f"--trace=file:{trace}",
                *(str(a) for a in args),
            ],
            **kwargs,
        )
        return EnvyRun(result, trace)

    def install(self, manifest: Path, *extra, **kwargs) -> EnvyRun:
        return self.run_envy("install", "--manifest", manifest, *extra, **kwargs)

    def sync(self, manifest: Path, *extra, **kwargs) -> EnvyRun:
        return self.run_envy("sync", "--manifest", manifest, *extra, **kwargs)

    def serve_directory(self, directory: Path) -> str:
        """Serve `directory` over HTTP for the test's lifetime; returns the base URL.

        The port comes from the OS and contents are read per request, so a test can
        change what a fixed URL returns between runs.
        """
        server = ThreadingHTTPServer(
            ("127.0.0.1", 0), partial(QuietHTTPHandler, directory=str(directory))
        )
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()

        def stop():
            # One cleanup rather than three, so the order is stated instead of
            # implied by LIFO: stop the accept loop, drop the listening socket,
            # then wait for the thread -- with a bound. A wedged server thread has
            # to fail this one test, never hang the suite; it is a daemon, so an
            # unjoined one dies with the process.
            server.shutdown()
            server.server_close()
            thread.join(timeout=SERVER_SHUTDOWN_TIMEOUT_S)
            if thread.is_alive():
                raise AssertionError(
                    f"stub HTTP server for {directory} did not stop within "
                    f"{SERVER_SHUTDOWN_TIMEOUT_S}s"
                )

        self.addCleanup(stop)
        return f"http://127.0.0.1:{server.server_address[1]}"

    # -- git fixtures -------------------------------------------------------

    def git(self, *args, cwd: Path):
        """Run git with identity and branch name pinned, so no user config leaks in."""
        result = test_config.run(
            [
                "git",
                "-c",
                "user.name=envy tests",
                "-c",
                "user.email=tests@envy.invalid",
                "-c",
                "commit.gpgsign=false",
                *(str(a) for a in args),
            ],
            cwd=str(cwd),
            capture_output=True,
            text=True,
        )
        self.assertEqual(0, result.returncode, f"git {args}: {result.stderr}")
        return result

    def make_git_repo(self, files: dict[str, str], label: str = "repo") -> Path:
        """A local repo with one commit on `main`; returns its path.

        The directory name ends in `.git` on purpose: uri_classify routes any
        `.git`-suffixed source to the git scheme, which is what makes a filesystem
        path usable as a git spec source without a server.
        """
        repo = self.make_temp_dir(label) / f"{label}.git"
        repo.mkdir()
        self.git("init", "-b", "main", cwd=repo)
        self.commit_files(repo, files, "initial")
        return repo

    def commit_files(self, repo: Path, files: dict[str, str], message: str) -> None:
        for name, content in files.items():
            path = repo / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8")
        self.git("add", "-A", cwd=repo)
        self.git("commit", "-m", message, cwd=repo)

    # -- cache introspection ------------------------------------------------
    #
    # Both trees are keyed: a spec entry is specs/<identity>/<source-key>/ and a
    # package entry is packages/<identity>/<platform>-<arch>-blake3-<hash>/. Tests
    # assert on what is in the tree, not on a key they would have to recompute.

    @staticmethod
    def _entries(parent: Path) -> list[Path]:
        return sorted(p for p in parent.iterdir() if p.is_dir()) if parent.is_dir() else []

    def spec_entries(self, identity: str) -> list[Path]:
        """Every cache entry for a spec or bundle identity, one per source."""
        return self._entries(self.cache_root / "specs" / identity)

    def pkg_entries(self, identity: str) -> list[Path]:
        """Every cache entry for a package identity, one per platform/option hash."""
        return self._entries(self.cache_root / "packages" / identity)

    def spec_complete(self, identity: str) -> bool:
        """True when some entry for `identity` was finalized (`envy-complete`)."""
        return any((e / "envy-complete").exists() for e in self.spec_entries(identity))

    def pkg_complete(self, identity: str) -> bool:
        return any((e / "envy-complete").exists() for e in self.pkg_entries(identity))
