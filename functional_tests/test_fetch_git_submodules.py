"""Git fetches recurse into submodules on request.

`envy fetch --submodules` and `submodules = true | { selectors }` on every git entry
(FETCH, envy.fetch, spec and bundle sources) check out each submodule at the commit its
superproject records, recursively, in git's own layout: `.git/modules/<name>`, a
relative gitfile, `submodule.<name>.url` in the superproject's config.

Fixtures are built with plumbing (gitlinks via update-index), so making them needs no
`git submodule add` and no file-transport opt-in. libgit2's local transport cannot go
shallow, so the strategy tests serve the same repos through `git daemon`.
"""

from __future__ import annotations

import hashlib
import os
import re
import shutil
import socket
import subprocess
import time
import unittest
from pathlib import Path

from . import test_config
from .env import EnvyTestCase
from .test_config import make_manifest

_GIT = shutil.which("git")

TOOL_SPEC = """IDENTITY = "test.subtool@v1"
DEPENDENCIES = {}
USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options) return false end,
    INSTALL = function(pkg_dir, options) end,
  },
}
"""

BUNDLE_LUA = """BUNDLE = "test.subs@v1"
SPECS = { ["test.subtool@v1"] = "deps/lib/tool.lua" }
"""


def _git(*args, cwd: Path) -> str:
    """Run git with identity pinned, failing with git's own words."""
    run = subprocess.run(
        [
            _GIT,
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
    if run.returncode != 0:
        raise AssertionError(f"git {args} failed ({run.returncode}): {run.stderr.strip()}")
    return run.stdout.rstrip()  # a leading space is `git submodule status` data


class _Repos:
    """Sibling repos under one directory, so `../x.git` URLs resolve between them.

    super.git
      top.txt
      deps/lib     -> lib.git @ c1 (not lib's tip), relative url
        vendor/leaf  -> leaf.git, relative to lib's own url
      deps/other   -> other.git, absolute local url, submodule *name* "other-mod"
      deps/skipped -> skipped.git, update = none
    dsuper.git       deps/lib only: every url relative, so a server can resolve it
    evil.git         deps/other by absolute local path, for a served superproject
    """

    def __init__(self, root: Path):
        self.root = root
        self.leaf = self._repo("leaf", {"leaf.txt": "leaf\n"})
        self.leaf_sha = self.head(self.leaf)

        self.lib = self._repo("lib", {"lib.txt": "v1\n", "tool.lua": TOOL_SPEC})
        self._submodules(self.lib, [("vendor/leaf", "vendor/leaf", "../leaf.git",
                                     self.leaf_sha, None)])
        self.lib_c1 = self.head(self.lib)
        (self.lib / "lib.txt").write_text("v2\n", encoding="utf-8")
        _git("commit", "-am", "tip", cwd=self.lib)
        self.lib_tip = self.head(self.lib)

        self.other = self._repo("other", {"other.txt": "other\n"})
        self.skipped = self._repo("skipped", {"skipped.txt": "skipped\n"})

        lib_entry = ("deps/lib", "deps/lib", "../lib.git", self.lib_c1, None)
        self.super = self._repo("super", {"top.txt": "top\n",
                                          "envy-bundle.lua": BUNDLE_LUA})
        self._submodules(self.super, [
            lib_entry,
            ("other-mod", "deps/other", self.other.as_posix(), self.head(self.other),
             None),
            ("deps/skipped", "deps/skipped", "../skipped.git", self.head(self.skipped),
             "none"),
        ])
        self.super_sha = self.head(self.super)

        self.dsuper = self._repo("dsuper", {"top.txt": "top\n"})
        self._submodules(self.dsuper, [lib_entry])
        self.dsuper_sha = self.head(self.dsuper)

        self.evil = self._repo("evil", {"top.txt": "top\n"})
        self._submodules(self.evil, [("deps/other", "deps/other",
                                      self.other.as_posix(), self.head(self.other),
                                      None)])
        self.evil_sha = self.head(self.evil)

        # A gitlink with no .gitmodules at all: nothing says where to fetch it from.
        self.orphan = self._repo("orphan", {"top.txt": "top\n"})
        _git("update-index", "--add", "--cacheinfo", f"160000,{self.leaf_sha},deps/x",
             cwd=self.orphan)
        _git("commit", "-m", "orphan gitlink", cwd=self.orphan)
        self.orphan_sha = self.head(self.orphan)

    def _repo(self, name: str, files: dict[str, str]) -> Path:
        repo = self.root / f"{name}.git"
        repo.mkdir()
        _git("init", "-b", "main", cwd=repo)
        for rel, content in files.items():
            (repo / rel).write_text(content, encoding="utf-8")
        _git("add", "-A", cwd=repo)
        _git("commit", "-m", "initial", cwd=repo)
        return repo

    @staticmethod
    def head(repo: Path) -> str:
        return _git("rev-parse", "HEAD", cwd=repo)

    @staticmethod
    def _submodules(repo: Path, entries) -> None:
        """Record gitlinks plus their .gitmodules stanzas in one commit."""
        stanzas = []
        for name, path, url, sha, update in entries:
            _git("update-index", "--add", "--cacheinfo", f"160000,{sha},{path}", cwd=repo)
            stanza = f'[submodule "{name}"]\n\tpath = {path}\n\turl = {url}\n'
            if update:
                stanza += f"\tupdate = {update}\n"
            stanzas.append(stanza)
        (repo / ".gitmodules").write_text("".join(stanzas), encoding="utf-8")
        _git("add", ".gitmodules", cwd=repo)
        _git("commit", "-m", "submodules", cwd=repo)


def _tree(root: Path) -> dict[str, str | None]:
    """Relative path -> sha256 of its bytes (None for a directory), minus git metadata.

    CRLF folds to LF first: Git for Windows ships `core.autocrlf=true` system-wide, and
    whether libgit2 and the git CLI both honor it is not what these tests ask.
    """
    out: dict[str, str | None] = {}
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d != ".git"]
        rel_dir = Path(dirpath).relative_to(root)
        for d in dirnames:
            out[(rel_dir / d).as_posix()] = None
        for f in filenames:
            if f == ".git":  # a submodule's gitfile
                continue
            out[(rel_dir / f).as_posix()] = hashlib.sha256(
                (Path(dirpath) / f).read_bytes().replace(b"\r\n", b"\n")
            ).hexdigest()
    return out


class _SubmoduleCase(EnvyTestCase):
    # Per test, not per class: the parallel runner gives each test its own suite, so
    # a class fixture is rebuilt -- and torn down -- under its siblings' feet.
    def setUp(self):
        super().setUp()
        self.repos = _Repos(self.make_temp_dir("repos"))

    def fetch(self, source: str, ref: str, *extra, **kwargs):
        dest = self.work / "out"
        return dest, self.run_envy("fetch", source, dest, "--ref", ref, *extra, **kwargs)

    def git_fetch_events(self, run) -> dict[str, dict]:
        return {e.raw["path"]: e.raw for e in run.events("git_fetch")}


@unittest.skipIf(_GIT is None, "git binary not available")
class TestFetchGitSubmodulesLocal(_SubmoduleCase):
    envy_watchdog_timeout = 30

    def test_tree_matches_a_recursive_git_clone(self):
        dest, run = self.fetch(str(self.repos.super), self.repos.super_sha,
                               "--submodules")
        self.assertEqual(0, run.returncode, run.stderr)

        ref = self.work / "ref"
        _git("-c", "protocol.file.allow=always", "clone", "--recurse-submodules",
             self.repos.super, ref, cwd=self.work)
        self.assertEqual(_tree(ref), _tree(dest))

        # git's layout: gitdirs under .git/modules/<name>, keyed by name, not path.
        self.assertTrue((dest / ".git" / "modules" / "other-mod").is_dir())
        self.assertTrue((dest / "deps" / "lib" / ".git").is_file())
        self.assertTrue(
            (dest / ".git" / "modules" / "deps" / "lib" / "modules" / "vendor" / "leaf")
            .is_dir()
        )

        # Populated submodules sit at their recorded commit; update=none stays out.
        status = {
            line[1:].split()[1]: line[0]
            for line in _git("submodule", "status", "--recursive", cwd=dest).splitlines()
        }
        self.assertEqual(
            {"deps/lib": " ", "deps/lib/vendor/leaf": " ", "deps/other": " ",
             "deps/skipped": "-"},
            status,
        )

        events = self.git_fetch_events(run)
        self.assertEqual({"", "deps/lib", "deps/lib/vendor/leaf", "deps/other"},
                         set(events))
        self.assertEqual(self.repos.lib_c1, events["deps/lib"]["sha"])
        self.assertEqual({"local"}, {e["strategy"] for e in events.values()})

    def test_nested_gitdir_past_libgit2s_windows_cap(self):
        """libgit2 rejects a gitdir whose path, plus 92 for its longest file, passes
        MAX_PATH -- about 168 characters -- whatever core.longpaths says; envy patches
        that out. Aim the nested gitdir near 185, so every real file stays under 260 and
        only that cap is under test."""
        dest = self.work / "out"
        while len(str(dest / ".git/modules/deps/lib/modules/vendor/leaf")) < 185:
            dest = dest.parent / ("x" + dest.name)
        run = self.run_envy("fetch", self.repos.super, dest,
                            "--ref", self.repos.super_sha, "--submodules")
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertTrue((dest / "deps/lib/vendor/leaf/leaf.txt").is_file())

    def test_without_the_flag_no_submodule_is_fetched(self):
        dest, run = self.fetch(str(self.repos.super), self.repos.super_sha)
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertTrue((dest / "top.txt").is_file())
        self.assertFalse((dest / "deps" / "lib" / "lib.txt").exists())
        self.assertEqual({""}, set(self.git_fetch_events(run)))

    def test_exclude_prunes_a_subtree(self):
        dest, run = self.fetch(str(self.repos.super), self.repos.super_sha,
                               "--submodule-filter", "!deps/lib")
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertTrue((dest / "deps" / "other" / "other.txt").is_file())
        self.assertFalse((dest / "deps" / "lib" / "lib.txt").exists())
        self.assertEqual({"", "deps/other"}, set(self.git_fetch_events(run)))

    def test_include_selects_and_reaches_nested(self):
        dest, run = self.fetch(str(self.repos.super), self.repos.super_sha,
                               "--submodule-filter", "deps/lib")
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertTrue((dest / "deps" / "lib" / "vendor" / "leaf" / "leaf.txt").is_file())
        self.assertFalse((dest / "deps" / "other" / "other.txt").exists())

    def test_nested_selector_fetches_the_submodules_on_its_path(self):
        for selector in ("deps/lib/vendor/leaf", "**/leaf"):
            with self.subTest(selector=selector):
                dest, run = self.fetch(str(self.repos.super), self.repos.super_sha,
                                       "--submodule-filter", selector)
                self.assertEqual(0, run.returncode, run.stderr)
                self.assertTrue((dest / "deps/lib/vendor/leaf/leaf.txt").is_file())
                self.assertIn("deps/lib/vendor/leaf", self.git_fetch_events(run))

    def test_nested_selector_still_honors_an_excluded_ancestor(self):
        dest, run = self.fetch(str(self.repos.super), self.repos.super_sha,
                               "--submodule-filter", "**/leaf",
                               "--submodule-filter", "!deps/lib")
        self.assertNotEqual(0, run.returncode)  # nothing left to match **/leaf
        self.assertIn("**/leaf", run.stderr)
        self.assertNotIn("deps/lib", self.git_fetch_events(run))

    def test_gitlink_without_gitmodules_is_an_error(self):
        _, run = self.fetch(str(self.repos.orphan), self.repos.orphan_sha, "--submodules")
        self.assertNotEqual(0, run.returncode)
        self.assertIn("'deps/x' has no url", run.stderr)

    def test_include_matching_nothing_is_an_error(self):
        _, run = self.fetch(str(self.repos.super), self.repos.super_sha,
                            "--submodule-filter", "deps/nope")
        self.assertNotEqual(0, run.returncode)
        self.assertIn("deps/nope", run.stderr)

    def test_short_sha_is_rejected(self):
        _, run = self.fetch(str(self.repos.super), self.repos.super_sha[:12])
        self.assertNotEqual(0, run.returncode)
        self.assertIn("full commit sha", run.stderr)

    def test_branch_name_still_resolves(self):
        dest, run = self.fetch(str(self.repos.super), "main", "--submodules")
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertEqual("v1\n", (dest / "deps" / "lib" / "lib.txt").read_text())
        self.assertEqual(self.repos.super_sha, self.git_fetch_events(run)[""]["sha"])

    def test_bundle_source_with_submodules(self):
        """A bundle whose spec lives inside a submodule loads only with submodules."""

        def manifest(extra: str) -> Path:
            path = self.work / "envy.lua"
            path.write_text(
                make_manifest(
                    f"""
BUNDLES = {{
    tc = {{ identity = "test.subs@v1", source = "{self.repos.super.as_posix()}",
           ref = "{self.repos.super_sha}"{extra} }},
}}
PACKAGES = {{
    {{ spec = "test.subtool@v1", bundle = "tc", setup = {{ "main" }} }},
}}
"""
                ),
                encoding="utf-8",
            )
            return path

        without = self.install(manifest(""))
        self.assertNotEqual(0, without.returncode)

        run = self.install(manifest(", submodules = { '!deps/other' }"))
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertEqual({"", "deps/lib", "deps/lib/vendor/leaf"},
                         set(self.git_fetch_events(run)))

    def test_fetch_verb_with_submodules(self):
        spec = self.write_spec(
            "subfetch.lua",
            f"""IDENTITY = "local.subfetch@v1"
FETCH = {{
    source = "{self.repos.super.as_posix()}",
    ref = "{self.repos.super_sha}",
    submodules = {{ "!deps/other" }},
}}
function INSTALL(install_dir, stage_dir, fetch_dir, tmp_dir, options)
    local function exists(p)
        local f = io.open(stage_dir .. "/super.git/" .. p, "r")
        if f then f:close() end
        return f ~= nil
    end
    assert(exists("deps/lib/vendor/leaf/leaf.txt"), "nested submodule missing")
    assert(not exists("deps/other/other.txt"), "excluded submodule fetched")
end
""",
        )
        manifest = self.write_manifest(
            f'PACKAGES = {{ {{ spec = "local.subfetch@v1", source = "{spec.as_posix()}" }} }}'
        )
        run = self.install(manifest)
        self.assertEqual(0, run.returncode, run.stderr)

    def test_fetch_table_rejects_unknown_keys(self):
        spec = self.write_spec(
            "typo.lua",
            f"""IDENTITY = "local.typo@v1"
FETCH = {{
    source = "{self.repos.super.as_posix()}",
    ref = "{self.repos.super_sha}",
    submodule = true,
}}
function INSTALL(install_dir, stage_dir, fetch_dir, tmp_dir, options) end
""",
        )
        manifest = self.write_manifest(
            f'PACKAGES = {{ {{ spec = "local.typo@v1", source = "{spec.as_posix()}" }} }}'
        )
        run = self.install(manifest)
        self.assertNotEqual(0, run.returncode)
        self.assertIn("submodule", run.stderr)


@unittest.skipIf(_GIT is None, "git binary not available")
class TestFetchGitSubmodulesServed(_SubmoduleCase):
    """Strategies a smart server makes possible, which the local transport cannot."""

    envy_watchdog_timeout = 30

    def serve(self, reachable_wants: bool) -> None:
        """`git daemon` over the fixture; upload-pack inherits the want policy via env."""
        sock = socket.socket()
        sock.bind(("127.0.0.1", 0))
        self.port = sock.getsockname()[1]
        sock.close()
        self.daemon_log = self.work / "git-daemon.log"
        log = open(self.daemon_log, "w", encoding="utf-8")
        self.addCleanup(log.close)
        daemon = subprocess.Popen(
            [
                _GIT,
                "daemon",
                "--verbose",
                "--informative-errors",
                f"--port={self.port}",
                "--reuseaddr",
                "--export-all",
                f"--base-path={self.repos.root}",
                str(self.repos.root),
            ],
            stdout=subprocess.DEVNULL,
            stderr=log,
            env={
                **os.environ,
                "GIT_CONFIG_COUNT": "1",
                "GIT_CONFIG_KEY_0": "uploadpack.allowReachableSHA1InWant",
                "GIT_CONFIG_VALUE_0": "true" if reachable_wants else "false",
            },
        )

        def stop():
            daemon.terminate()
            daemon.wait(timeout=10)

        self.addCleanup(stop)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            with socket.socket() as s:
                s.settimeout(0.25)
                if s.connect_ex(("127.0.0.1", self.port)) == 0:
                    return
            time.sleep(0.05)
        self.fail("git daemon did not start")

    def url(self, name: str) -> str:
        return f"git://127.0.0.1:{self.port}/{name}.git"

    def fetch(self, source: str, ref: str, *extra, **kwargs):
        """A failed run carries the daemon's own account, so a server-side refusal is
        legible in CI rather than a bare connection reset."""
        dest, run = super().fetch(source, ref, *extra, **kwargs)
        if run.returncode != 0 and self.daemon_log.exists():
            daemon = self.daemon_log.read_text(encoding="utf-8", errors="replace")
            run.stderr += f"\n--- git daemon ---\n{daemon}"
        return dest, run

    def test_tip_shallow_and_full_fallback(self):
        """The superproject's tip goes shallow anywhere; lib's non-tip pin cannot."""
        self.serve(reachable_wants=False)
        dest, run = self.fetch(self.url("dsuper"), self.repos.dsuper_sha, "--submodules")
        self.assertEqual(0, run.returncode, run.stderr)
        events = self.git_fetch_events(run)
        self.assertEqual(("tip", 1), (events[""]["strategy"], events[""]["depth"]))
        self.assertEqual(("full", 0),
                         (events["deps/lib"]["strategy"], events["deps/lib"]["depth"]))
        self.assertEqual("v1\n", (dest / "deps" / "lib" / "lib.txt").read_text())
        self.assertEqual("leaf\n",
                         (dest / "deps/lib/vendor/leaf/leaf.txt").read_text())
        self.assertEqual(self.url("leaf"), events["deps/lib/vendor/leaf"]["url"])

    def test_unadvertised_commit_goes_shallow_when_allowed(self):
        self.serve(reachable_wants=True)
        dest, run = self.fetch(self.url("dsuper"), self.repos.dsuper_sha, "--submodules")
        self.assertEqual(0, run.returncode, run.stderr)
        lib = self.git_fetch_events(run)["deps/lib"]
        self.assertEqual(("oid", 1), (lib["strategy"], lib["depth"]))
        self.assertTrue((dest / ".git" / "modules" / "deps" / "lib" / "shallow").is_file())
        self.assertEqual("v1\n", (dest / "deps" / "lib" / "lib.txt").read_text())

    def test_row_lands_on_the_tree_count(self):
        """The row ends on N/N submodules. Off a TTY only the last frame is certain to
        print; the counts and child rows along the way are unit-tested (git_progress)."""
        self.serve(reachable_wants=True)
        env = test_config.get_test_env()
        env["TERM"] = "dumb"
        env["ENVY_TEST_FALLBACK_THROTTLE_MS"] = "0"
        _, run = self.fetch(self.url("dsuper"), self.repos.dsuper_sha, "--submodules",
                            env=env)
        self.assertEqual(0, run.returncode, run.stderr)
        counts = re.findall(r"(\d+)/(\d+) submodules", run.stderr)
        self.assertEqual(("2", "2"), counts[-1] if counts else None, run.stderr)

    def test_served_superproject_cannot_reach_local_paths(self):
        self.serve(reachable_wants=True)
        _, run = self.fetch(self.url("evil"), self.repos.evil_sha, "--submodules")
        self.assertNotEqual(0, run.returncode)
        self.assertIn("refusing local submodule url", run.stderr)
        self.assertNotIn("deps/other", self.git_fetch_events(run))


@unittest.skipUnless(os.environ.get("ENVY_TEST_ESP_IDF"), "set ENVY_TEST_ESP_IDF=1")
class TestFetchGitSubmodulesEspIdf(EnvyTestCase):
    """Issue #391's acceptance check; several GB of network, so opt-in only."""

    envy_watchdog_timeout = 3600
    V61 = "fff9895c82d744c7237be8847347bdd1b07c6643"

    def test_matches_shallow_recursive_clone(self):
        url = "https://github.com/espressif/esp-idf.git"
        dest = self.work / "envy"
        run = self.run_envy("fetch", url, dest, "--ref", self.V61, "--submodules")
        self.assertEqual(0, run.returncode, run.stderr)

        ref = self.work / "git"
        _git("clone", "--branch", "v6.1", "--depth", "1", "--recurse-submodules",
             "--shallow-submodules", url, ref, cwd=self.work)
        self.assertEqual(_tree(ref), _tree(dest))


if __name__ == "__main__":
    unittest.main()
