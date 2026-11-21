"""A spec cache entry is keyed on its source, not on its identity alone.

Nothing revalidates a complete cache entry, so if identity were the whole key,
repointing a spec at a new URL, a new git ref, or a new archive would keep
serving yesterday's bytes forever -- the declaration would say one thing and the
cache would answer with another. Each test here redeclares one identity against a
second source and requires envy to fetch it rather than reuse the first entry.
"""

import hashlib
import os
import shutil
import subprocess
import sys
import tarfile
import time
import unittest
from pathlib import Path

from . import test_config
from .env import EnvyTestCase

SPEC = """IDENTITY = "{identity}"
DEPENDENCIES = {{}}
USER_MANAGED = true
local MARKER = "{marker}"
SETUP = {{
  main = {{ CHECK = "exit 0", INSTALL = "exit 0" }},
}}
"""


class TestRemoteSpecCacheKey(EnvyTestCase):
    """Two URLs, one identity."""

    IDENTITY = "test.keyed-remote@v1"

    def setUp(self):
        super().setUp()
        self.serve_dir = self.make_temp_dir("www")
        self.base_url = self.serve_directory(self.serve_dir)
        for name in ("a.lua", "b.lua"):
            (self.serve_dir / name).write_text(
                SPEC.format(identity=self.IDENTITY, marker=name), encoding="utf-8"
            )

    def manifest_for(self, filename: str):
        return self.write_manifest(
            f"""
PACKAGES = {{
    {{ spec = "{self.IDENTITY}", source = "{self.base_url}/{filename}",
       setup = {{ "main" }} }},
}}
"""
        )

    def test_changing_the_url_fetches_again(self):
        first = self.install(self.manifest_for("a.lua"))
        self.assertEqual(0, first.returncode, first.stderr)
        self.assertEqual(1, len(self.spec_entries(self.IDENTITY)))

        second = self.install(self.manifest_for("b.lua"))
        self.assertEqual(0, second.returncode, second.stderr)
        self.assertTrue(
            second.events("cache_miss", spec=self.IDENTITY),
            "a new URL must miss the cache, not reuse the old entry",
        )
        self.assertEqual(2, len(self.spec_entries(self.IDENTITY)))

    def test_returning_to_the_first_url_hits_its_own_entry(self):
        self.install(self.manifest_for("a.lua"))
        self.install(self.manifest_for("b.lua"))

        again = self.install(self.manifest_for("a.lua"))
        self.assertEqual(0, again.returncode, again.stderr)
        self.assertFalse(
            again.events("cache_miss", spec=self.IDENTITY),
            "the original URL still has its own complete entry",
        )
        self.assertEqual(2, len(self.spec_entries(self.IDENTITY)))


class TestBundleSourceCacheKey(EnvyTestCase):
    """Two archives, one bundle identity. (A bundle directory is read in place.)"""

    IDENTITY = "test.keyed-bundle@v1"
    MEMBER = "test.keyed-member@v1"

    def setUp(self):
        super().setUp()
        self.first = self.make_bundle("first")
        self.second = self.make_bundle("second")

    def make_bundle(self, label: str):
        root = self.make_temp_dir(label)
        (root / "specs").mkdir()
        (root / "envy-bundle.lua").write_text(
            f'BUNDLE = "{self.IDENTITY}"\n'
            f'SPECS = {{ ["{self.MEMBER}"] = "specs/m.lua" }}\n',
            encoding="utf-8",
        )
        (root / "specs" / "m.lua").write_text(
            SPEC.format(identity=self.MEMBER, marker=label), encoding="utf-8"
        )
        archive = self.make_temp_dir(f"{label}-archive") / "bundle.tar.gz"
        with tarfile.open(archive, "w:gz") as tf:
            for path in sorted(root.rglob("*")):
                tf.add(path, arcname=path.relative_to(root).as_posix(), recursive=False)
        return archive

    def manifest_for(self, bundle_dir):
        return self.write_manifest(
            f"""
BUNDLES = {{
    tc = {{ identity = "{self.IDENTITY}", source = "{self.lua_path(bundle_dir)}" }},
}}

PACKAGES = {{
    {{ spec = "{self.MEMBER}", bundle = "tc", setup = {{ "main" }} }},
}}
"""
        )

    def test_changing_the_bundle_archive_fetches_again(self):
        first = self.install(self.manifest_for(self.first))
        self.assertEqual(0, first.returncode, first.stderr)
        self.assertEqual(1, len(self.spec_entries(self.IDENTITY)))

        second = self.install(self.manifest_for(self.second))
        self.assertEqual(0, second.returncode, second.stderr)
        self.assertTrue(
            second.events("cache_miss", spec=self.IDENTITY),
            "a new bundle archive must miss the cache",
        )
        self.assertEqual(2, len(self.spec_entries(self.IDENTITY)))


class TestCheckoutIndependentKeys(EnvyTestCase):
    """Two checkouts of one project share every spec entry: where it sits is incidental."""

    def make_project(self) -> Path:
        root = self.make_temp_dir("checkout-a")
        (root / "specs").mkdir()
        (root / "vendor").mkdir()
        member = root / "bundle-src"
        (member / "specs").mkdir(parents=True)
        (member / "envy-bundle.lua").write_text(
            'BUNDLE = "test.arch-bundle@r1"\n'
            'SPECS = { ["test.arch-member@r1"] = "specs/m.lua" }\n',
            encoding="utf-8",
        )
        (member / "specs" / "m.lua").write_text(
            SPEC.format(identity="test.arch-member@r1", marker="m"), encoding="utf-8"
        )
        with tarfile.open(root / "vendor" / "bundle.tar.gz", "w:gz") as tf:
            for path in sorted(member.rglob("*")):
                tf.add(path, arcname=path.relative_to(member).as_posix(), recursive=False)
        shutil.rmtree(member)

        pinned = root / "specs" / "pinned.lua"
        pinned.write_text(SPEC.format(identity="test.pinned@r1", marker="p"), encoding="utf-8")
        digest = hashlib.sha256(pinned.read_bytes()).hexdigest()
        (root / "specs" / "parent.lua").write_text(
            'IDENTITY = "local.parent@r1"\nUSER_MANAGED = true\n'
            'SETUP = { main = { CHECK = "exit 0", INSTALL = "exit 0" } }\n'
            'DEPENDENCIES = { { spec = "test.fetched@r1", source = { fetch = function(tmp_dir)\n'
            '  local f = io.open(tmp_dir .. "/spec.lua", "w")\n'
            f"  f:write([[{SPEC.format(identity='test.fetched@r1', marker='f')}]])\n"
            '  f:close()\n  envy.commit_fetch({ "spec.lua" })\nend } } }\n',
            encoding="utf-8",
        )
        self.write_manifest(
            'BUNDLES = { b = { identity = "test.arch-bundle@r1", '
            'source = "vendor/bundle.tar.gz" } }\n'
            "PACKAGES = {\n"
            '  { spec = "test.arch-member@r1", bundle = "b", setup = { "main" } },\n'
            f'  {{ spec = "test.pinned@r1", source = "specs/pinned.lua", sha256 = "{digest}",'
            ' setup = { "main" } },\n'
            '  { spec = "local.parent@r1", source = "specs/parent.lua", setup = { "main" } },\n'
            "}\n",
            directory=root,
        )
        return root

    def test_a_second_checkout_hits_the_first_ones_entries(self):
        a = self.make_project()
        b = self.make_temp_dir("checkout-b") / "project"
        shutil.copytree(a, b)

        for checkout in (a, b):
            run = self.install(checkout / "envy.lua")
            self.assertEqual(0, run.returncode, run.stderr)

        for identity in ("test.arch-bundle@r1", "test.pinned@r1", "test.fetched@r1"):
            with self.subTest(identity):
                self.assertEqual(1, len(self.spec_entries(identity)))
                self.assertFalse(run.events("cache_miss", spec=identity), "b refetched")


class TestLocalArchiveKeyLocation(EnvyTestCase):
    """A local archive bundle inside its declaring project keys relative to that project,
    however the path is spelled, so checkouts share it; one outside keys on the canonical
    absolute path it resolves to, so one spelling reaching two archives is two entries."""

    IDENTITY = "test.loc-bundle@r1"
    MEMBER = "test.loc-member@r1"

    def pack(self, dest: Path, marker: str = "m", member: str = MEMBER) -> Path:
        src = self.make_temp_dir("bundle-src")
        (src / "specs").mkdir()
        (src / "envy-bundle.lua").write_text(
            f'BUNDLE = "{self.IDENTITY}"\nSPECS = {{ ["{member}"] = "specs/m.lua" }}\n',
            encoding="utf-8",
        )
        (src / "specs" / "m.lua").write_text(
            SPEC.format(identity=member, marker=marker), encoding="utf-8"
        )
        dest.parent.mkdir(parents=True, exist_ok=True)
        with tarfile.open(dest, "w:gz") as tf:
            for path in sorted(src.rglob("*")):
                tf.add(path, arcname=path.relative_to(src).as_posix(), recursive=False)
        return dest

    def body(self, source: str) -> str:
        """`source` is a Lua expression: a literal, or a call such as envy.abspath."""
        return (
            f'BUNDLES = {{ b = {{ identity = "{self.IDENTITY}", source = {source} }} }}\n'
            f'PACKAGES = {{ {{ spec = "{self.MEMBER}", bundle = "b", setup = {{ "main" }} }} }}\n'
        )

    def project(self, root: Path, source: str) -> Path:
        root.mkdir(parents=True, exist_ok=True)
        return self.write_manifest(self.body(source), directory=root)

    def superproject(self, root: Path, source: str) -> tuple[Path, Path]:
        """A superproject whose subproject `sub/` declares the bundle; both manifests."""
        (root / "sub").mkdir(parents=True, exist_ok=True)
        sub = root / "sub" / "envy.lua"
        sub.write_text(
            '-- @envy bin "sub-bin"\n-- @envy root "false"\n' + self.body(source),
            encoding="utf-8",
        )
        top = self.write_manifest(
            'local sub = envy.subproject("sub")\nPACKAGES = sub.PACKAGES\n', directory=root
        )
        return top, sub

    def spec_project(self, root: Path, source: str) -> Path:
        """A project whose spec `specs/consumer.lua` declares the bundle as a dependency."""
        (root / "specs").mkdir(parents=True, exist_ok=True)
        (root / "specs" / "consumer.lua").write_text(
            'IDENTITY = "local.consumer@r1"\nUSER_MANAGED = true\n'
            'SETUP = { main = { CHECK = "exit 0", INSTALL = "exit 0" } }\n'
            f'DEPENDENCIES = {{ {{ bundle = "{self.IDENTITY}", source = {source} }} }}\n',
            encoding="utf-8",
        )
        return self.write_manifest(
            'PACKAGES = { { spec = "local.consumer@r1", source = "specs/consumer.lua",'
            ' setup = { "main" } } }\n',
            directory=root,
        )

    def install_ok(self, manifest: Path):
        run = self.install(manifest)
        self.assertEqual(0, run.returncode, run.stderr)
        return run

    def assert_entries(self, n: int, *manifests: Path):
        for manifest in manifests:
            self.install_ok(manifest)
        self.assertEqual(n, len(self.spec_entries(self.IDENTITY)), self.spec_entries(self.IDENTITY))

    # -- inside the declaring project ---------------------------------------------------

    def test_inside_relative_is_shared_across_checkouts(self):
        a, b = self.make_temp_dir("a") / "proj", self.make_temp_dir("b") / "proj"
        for root in (a, b):
            self.pack(root / "vendor" / "b.tar.gz")
        self.assert_entries(
            1, self.project(a, '"vendor/b.tar.gz"'), self.project(b, '"vendor/b.tar.gz"')
        )

    def test_inside_absolute_is_shared_across_checkouts(self):
        """`envy.abspath` spells a different absolute path in every checkout."""
        a, b = self.make_temp_dir("a") / "proj", self.make_temp_dir("b") / "x" / "y" / "proj"
        for root in (a, b):
            self.pack(root / "vendor" / "b.tar.gz")
        source = 'envy.abspath("vendor/b.tar.gz")'
        self.assert_entries(1, self.project(a, source), self.project(b, source))

    def test_inside_absolute_at_the_project_root_is_shared_across_checkouts(self):
        a, b = self.make_temp_dir("a") / "proj", self.make_temp_dir("b") / "proj"
        for root in (a, b):
            self.pack(root / "b.tar.gz")
        source = 'envy.abspath("b.tar.gz")'
        self.assert_entries(1, self.project(a, source), self.project(b, source))

    def test_inside_absolute_literal_is_shared_across_checkouts(self):
        """A literal absolute path, written into each checkout's manifest, keys the same."""
        a, b = self.make_temp_dir("a") / "proj", self.make_temp_dir("b") / "proj"
        manifests = []
        for root in (a, b):
            archive = self.pack(root / "vendor" / "b.tar.gz")
            manifests.append(self.project(root, f'"{self.lua_path(archive)}"'))
        self.assert_entries(1, *manifests)

    def test_inside_relative_and_absolute_spellings_share_an_entry(self):
        root = self.make_temp_dir("a") / "proj"
        self.pack(root / "vendor" / "b.tar.gz")
        relative = self.project(root, '"vendor/b.tar.gz"')
        self.install_ok(relative)
        self.assert_entries(1, self.project(root, 'envy.abspath("vendor/b.tar.gz")'))

    def test_inside_a_subproject_keys_the_same_nested_and_standalone(self):
        """The subproject's own root anchors it, not whichever project is on top."""
        a, b = self.make_temp_dir("a") / "tree", self.make_temp_dir("b") / "tree"
        source = 'envy.abspath("vendor/b.tar.gz")'
        for root in (a, b):
            self.pack(root / "sub" / "vendor" / "b.tar.gz")
        top_a, _ = self.superproject(a, source)
        _, sub_b = self.superproject(b, source)
        self.assert_entries(1, top_a, sub_b)

    @unittest.skipIf(sys.platform == "win32", "symlinks need privileges on Windows")
    def test_inside_through_a_symlinked_checkout_is_inside(self):
        real = self.make_temp_dir("real") / "proj"
        self.pack(real / "vendor" / "b.tar.gz")
        manifest = self.project(real, 'envy.abspath("vendor/b.tar.gz")')
        link = self.make_temp_dir("link") / "proj"
        link.symlink_to(real, target_is_directory=True)
        self.assert_entries(1, manifest, link / "envy.lua")

    def test_inside_from_a_spec_keys_against_the_project_root(self):
        """`../vendor` leaves the spec's directory, not the project: checkouts share it."""
        a, b = self.make_temp_dir("a") / "proj", self.make_temp_dir("b") / "proj"
        manifests = []
        for root in (a, b):
            self.pack(root / "vendor" / "b.tar.gz")
            manifests.append(self.spec_project(root, '"../vendor/b.tar.gz"'))
        self.assert_entries(1, *manifests)

    def test_inside_absolute_from_a_spec_is_shared_across_checkouts(self):
        a, b = self.make_temp_dir("a") / "proj", self.make_temp_dir("b") / "proj"
        source = 'envy.abspath("../vendor/b.tar.gz")'
        manifests = []
        for root in (a, b):
            self.pack(root / "vendor" / "b.tar.gz")
            manifests.append(self.spec_project(root, source))
        self.assert_entries(1, *manifests)

    def test_inside_spec_and_manifest_spellings_share_an_entry(self):
        root = self.make_temp_dir("a") / "proj"
        self.pack(root / "vendor" / "b.tar.gz")
        self.install_ok(self.project(root, '"vendor/b.tar.gz"'))
        self.assert_entries(1, self.spec_project(root, '"../vendor/b.tar.gz"'))

    # -- outside the declaring project --------------------------------------------------

    def test_outside_absolute_from_a_spec_is_shared_by_checkouts_at_any_depth(self):
        archive = self.pack(self.make_temp_dir("shared") / "b.tar.gz")
        source = f'"{self.lua_path(archive)}"'
        self.assert_entries(
            1,
            self.spec_project(self.make_temp_dir("a") / "proj", source),
            self.spec_project(self.make_temp_dir("b") / "x" / "y" / "proj", source),
        )

    def test_outside_relative_from_a_spec_to_one_archive_is_shared(self):
        parent = self.make_temp_dir("parent")
        self.pack(parent / "shared" / "b.tar.gz")
        source = '"../../shared/b.tar.gz"'
        self.assert_entries(
            1, self.spec_project(parent / "a", source), self.spec_project(parent / "b", source)
        )

    def test_outside_absolute_is_shared_by_checkouts_at_any_depth(self):
        """Kept whole: relative to each checkout it would read differently per depth."""
        archive = self.pack(self.make_temp_dir("shared") / "b.tar.gz")
        source = f'"{self.lua_path(archive)}"'
        self.assert_entries(
            1,
            self.project(self.make_temp_dir("a") / "proj", source),
            self.project(self.make_temp_dir("b") / "x" / "y" / "proj", source),
        )

    def test_outside_absolute_beside_the_project_is_not_relativized(self):
        """`proj-other` shares a prefix with `proj`, not a subtree: each is its own path."""
        manifests = []
        for label in ("a", "b"):
            parent = self.make_temp_dir(label)
            self.pack(parent / "proj-other" / "b.tar.gz")
            manifests.append(self.project(parent / "proj", 'envy.abspath("../proj-other/b.tar.gz")'))
        self.assert_entries(2, *manifests)

    def test_outside_relative_to_one_archive_is_shared(self):
        """Sibling checkouts reaching one shared archive by the same relative path share it."""
        parent = self.make_temp_dir("parent")
        self.pack(parent / "shared" / "b.tar.gz")
        source = '"../shared/b.tar.gz"'
        self.assert_entries(
            1, self.project(parent / "a", source), self.project(parent / "b", source)
        )

    def test_outside_relative_from_a_subproject_keys_whole(self):
        """`../vendor` leaves the subproject's root, so each tree's archive is its own."""
        a, b = self.make_temp_dir("a") / "tree", self.make_temp_dir("b") / "tree"
        tops = []
        for root in (a, b):
            self.pack(root / "vendor" / "b.tar.gz", marker=root.parent.name)
            tops.append(self.superproject(root, '"../vendor/b.tar.gz"')[0])
        self.assert_entries(2, *tops)

    def test_outside_relative_reaching_two_archives_is_two_entries(self):
        """`../../shared` from the manifest and from `specs/` names two archives."""
        root = self.make_temp_dir("a") / "x" / "proj"
        source = '"../../shared/b.tar.gz"'
        self.pack(root.parent.parent / "shared" / "b.tar.gz", marker="manifest")
        self.pack(root.parent / "shared" / "b.tar.gz", marker="spec")
        self.install_ok(self.project(root, source))
        self.assert_entries(2, self.spec_project(root, source))

    # -- symlinked archives: the key names the link, as the fetch opens it --------------

    @unittest.skipIf(sys.platform == "win32", "symlinks need privileges on Windows")
    def test_a_symlink_alias_behaves_as_on_a_clean_cache(self):
        """`b.gz` -> `b.tar.gz` extracts as a bare stream, so the tar spelling's entry
        must not answer for it."""
        shared = self.make_temp_dir("shared")
        self.pack(shared / "b.tar.gz")
        (shared / "b.gz").symlink_to("b.tar.gz")
        root = self.make_temp_dir("a") / "proj"
        alias = f'"{self.lua_path(shared / "b.gz")}"'
        cold = self.install(self.project(root, alias))
        self.install_ok(self.project(root, f'"{self.lua_path(shared / "b.tar.gz")}"'))
        warm = self.install(self.project(root, alias))
        self.assertNotEqual(0, cold.returncode)
        self.assertEqual(cold.returncode, warm.returncode, warm.stderr)

    @unittest.skipIf(sys.platform == "win32", "symlinks need privileges on Windows")
    def test_a_symlink_whose_target_is_gone_still_hits(self):
        shared = self.make_temp_dir("shared")
        target = self.pack(shared / "real.tar.gz")
        (shared / "b.tar.gz").symlink_to("real.tar.gz")
        manifest = self.project(
            self.make_temp_dir("a") / "proj", f'"{self.lua_path(shared / "b.tar.gz")}"'
        )
        self.install_ok(manifest)
        target.unlink()
        self.assert_entries(1, manifest)

    @unittest.skipIf(sys.platform == "win32", "symlinks need privileges on Windows")
    def test_a_symlink_retargeted_mid_run_leaves_its_old_target_alone(self):
        """The link is repointed after envy keys it but before it fetches; the bytes it
        then reads must not land under the old target's key."""
        shared = self.make_temp_dir("shared")
        old = self.pack(shared / "a.tar.gz")
        self.pack(shared / "b.tar.gz", member="test.loc-other@r1")  # no MEMBER
        link = shared / "current.tar.gz"
        link.symlink_to("a.tar.gz")
        root = self.make_temp_dir("a") / "proj"
        barriers = self.make_temp_dir("barriers")

        # Hold the old target's entry so the run, if it keys on that, waits for it.
        held = test_config.popen(
            [str(self.envy), "--cache-root", str(self.cache_root), "cache-test",
             "ensure-spec", self.IDENTITY,
             "--source", "local\n" + Path(os.path.realpath(old)).as_posix(),
             f"--barrier-dir={barriers}", "--barrier-signal-after=held",
             "--barrier-wait-after=release", "--fail-before-complete"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        self.addCleanup(lambda: held.poll() is None and held.kill())
        self.wait_for(lambda: (barriers / "held").exists())

        trace = self.work / "retarget.jsonl"
        run = test_config.popen(
            [str(self.envy), "--cache-root", str(self.cache_root), f"--trace=file:{trace}",
             "install", "--manifest", str(self.project(root, f'"{self.lua_path(link)}"'))],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        self.addCleanup(lambda: run.poll() is None and run.kill())
        started = f'"spec":"{self.IDENTITY}"'  # its fetch phase began: the key is fixed
        self.wait_for(
            lambda: run.poll() is not None
            or (trace.exists() and any(
                "phase_start" in line and started in line
                for line in trace.read_text(encoding="utf-8").splitlines()))
        )
        link.unlink()
        link.symlink_to("b.tar.gz")
        (barriers / "release").touch()
        held.communicate(timeout=30)
        run.communicate(timeout=30)

        self.install_ok(self.project(root, f'"{self.lua_path(old)}"'))

    def wait_for(self, done, timeout: float = 30.0):
        deadline = time.monotonic() + timeout
        while not done():
            self.assertLess(time.monotonic(), deadline, "timed out")
            time.sleep(0.02)

    def test_outside_moved_is_a_new_entry(self):
        """Same bytes at a new path outside the project: the source changed, so it misses."""
        first = self.pack(self.make_temp_dir("one") / "b.tar.gz")
        second = self.make_temp_dir("two") / "b.tar.gz"
        shutil.copyfile(first, second)
        root = self.make_temp_dir("a") / "proj"
        self.install_ok(self.project(root, f'"{self.lua_path(first)}"'))
        self.assert_entries(2, self.project(root, f'"{self.lua_path(second)}"'))


@unittest.skipUnless(shutil.which("git"), "git is not on PATH")
class TestGitSpecCacheKey(EnvyTestCase):
    """Two refs of one repo, one identity.

    A filesystem path ending in `.git` is a git source (uri_classify routes any
    `.git` suffix to the git scheme), so this needs no server. The ref is part of
    the cache key: pinning a new ref must fetch it, not hand back the old checkout.
    """

    IDENTITY = "test.keyed-git@v1"

    def setUp(self):
        super().setUp()
        self.repo = self.make_git_repo(
            {"spec.lua": SPEC.format(identity=self.IDENTITY, marker="v1")}, "gitspec"
        )
        self.git("tag", "v1", cwd=self.repo)
        self.commit_files(
            self.repo,
            {"spec.lua": SPEC.format(identity=self.IDENTITY, marker="v2")},
            "second",
        )
        self.git("tag", "v2", cwd=self.repo)

    def manifest_for(self, ref: str):
        return self.write_manifest(
            f"""
PACKAGES = {{
    {{ spec = "{self.IDENTITY}", source = "{self.lua_path(self.repo)}",
       ref = "{ref}", setup = {{ "main" }} }},
}}
"""
        )

    def test_changing_the_ref_fetches_again(self):
        first = self.install(self.manifest_for("v1"))
        self.assertEqual(0, first.returncode, first.stderr)
        self.assertEqual(1, len(self.spec_entries(self.IDENTITY)))

        second = self.install(self.manifest_for("v2"))
        self.assertEqual(0, second.returncode, second.stderr)
        self.assertTrue(
            second.events("cache_miss", spec=self.IDENTITY),
            "a new ref must miss the cache, not reuse the old checkout",
        )
        self.assertEqual(2, len(self.spec_entries(self.IDENTITY)))

    def test_same_ref_hits_the_cache(self):
        self.install(self.manifest_for("v1"))

        again = self.install(self.manifest_for("v1"))
        self.assertEqual(0, again.returncode, again.stderr)
        self.assertFalse(again.events("cache_miss", spec=self.IDENTITY))
        self.assertEqual(1, len(self.spec_entries(self.IDENTITY)))


if __name__ == "__main__":
    unittest.main()
