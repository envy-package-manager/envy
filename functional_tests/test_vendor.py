"""Functional tests for vendoring: cached payloads copied into the project tree.

Assertions read the trace rather than stdout -- `vendor_result` carries the action and
the reason envy chose, so a test can tell "nothing was there" from "somebody edited it"
without matching prose.
"""

from __future__ import annotations

import os
import shutil
import sys
import time
import unittest
from pathlib import Path

from . import test_config
from .env import EnvyTestCase


POSIX_ONLY = unittest.skipIf(sys.platform == "win32", "POSIX file modes")

# A cache-managed package whose INSTALL copies a payload tree the test built. FETCH is
# required of every cache-managed spec, so it takes the cheapest local file there is.
SPEC = """IDENTITY = "{identity}"
FETCH = {{ source = "file://{seed}" }}
{vendor_list}
INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  envy.copy("{payload}", install_dir)
  {install_extra}
end
"""


def rmtree_retry(path: Path, attempts: int = 10) -> None:
    """shutil.rmtree, retried. An antivirus or indexer on Windows can hold a handle for
    a moment after the process that wrote the file has exited."""
    for attempt in range(attempts):
        try:
            shutil.rmtree(path)
            return
        except OSError:
            if attempt == attempts - 1:
                raise
            time.sleep(0.1)


class VendorTestCase(EnvyTestCase):
    def setUp(self):
        super().setUp()
        self.project = self.make_temp_dir("project")
        self.payload = self.make_temp_dir("payload")
        self.seed = self.project / "seed.txt"
        self.seed.write_text("seed\n", encoding="utf-8")

        self.write_payload("include/lib.h", "#pragma once\n")
        self.write_payload("include/internal/secret.h", "// internal\n")
        self.write_payload("src/lib.c", "int lib(void){return 0;}\n")
        self.write_payload("LICENSE", "MIT\n")

    # -- authoring ----------------------------------------------------------

    def write_payload(self, rel: str, text: str) -> Path:
        path = self.payload / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return path

    def spec(self, identity: str, vendor_list: str = "", install_extra: str = "") -> Path:
        name = identity.split(".", 1)[1].split("@", 1)[0]
        return self.write_spec(
            f"{name}.lua",
            SPEC.format(
                identity=identity,
                seed=self.lua_path(self.seed),
                payload=self.lua_path(self.payload),
                vendor_list=vendor_list,
                install_extra=install_extra,
            ),
            directory=self.project,
        )

    def manifest(self, entries: str, vendor_root: str | None = "vendor") -> Path:
        root = f'VENDOR_ROOT = "{vendor_root}"\n' if vendor_root is not None else ""
        path = self.project / "envy.lua"
        path.write_text(
            test_config.make_manifest(root + "PACKAGES = {\n" + entries + "\n}\n"),
            encoding="utf-8",
        )
        return path

    def entry(self, identity: str, spec: Path, vendor: str | None = "true", **kw) -> str:
        fields = [f'spec = "{identity}"', f'source = "{self.lua_path(spec)}"']
        if vendor is not None:
            fields.append(f"vendor = {vendor}")
        for key, value in kw.items():
            fields.append(f"{key} = {value}")
        return "  { " + ", ".join(fields) + " },"

    # -- inspecting ---------------------------------------------------------

    def results(self, run) -> dict[str, dict]:
        """identity -> the vendor_result event it produced."""
        return {e.spec: e.raw for e in run.events("vendor_result")}

    def assertVendored(
        self, run, identity: str, action: str, reason: str, dry_run: bool = False
    ):
        self.assertEqual(0, run.returncode, run.stderr)
        results = self.results(run)
        self.assertIn(identity, results, f"no vendor_result for {identity}")
        self.assertEqual(action, results[identity]["action"], results[identity])
        self.assertEqual(reason, results[identity]["reason"], results[identity])
        # Defaulted, so every install/sync assertion in this file also pins that a
        # package outside `envy vendor --dry-run` is never merely contemplated.
        self.assertEqual(dry_run, results[identity]["dry_run"], results[identity])

    def shown(self, dest: Path) -> str:
        """How envy names a destination out loud: project-relative, forward slashes."""
        return dest.relative_to(self.project).as_posix()

    def tree_of(self, root: Path) -> dict[str, str]:
        """Relative path -> contents, for every file under `root`."""
        return {
            p.relative_to(root).as_posix(): p.read_text(encoding="utf-8")
            for p in root.rglob("*")
            if p.is_file()
        }


class TestVendorPlacement(VendorTestCase):
    def test_vendor_true_derives_a_name_under_the_vendor_root(self):
        manifest = self.manifest(
            self.entry("local.nanocobs@r3", self.spec("local.nanocobs@r3"))
        )
        run = self.install(manifest)
        self.assertVendored(run, "local.nanocobs@r3", "copied", "absent")

        dest = self.project / "vendor" / "nanocobs"
        self.assertEqual(self.tree_of(self.payload), self.tree_of(dest))

    def test_two_option_variants_of_one_spec_both_vendor(self):
        spec = self.spec("local.nanocobs@r3")
        manifest = self.manifest(
            self.entry("local.nanocobs@r3", spec, options='{ build = "debug" }')
            + "\n"
            + self.entry("local.nanocobs@r3", spec, options='{ build = "release" }')
        )
        run = self.install(manifest)
        self.assertEqual(0, run.returncode, run.stderr)

        dests = sorted(p.name for p in (self.project / "vendor").iterdir())
        self.assertEqual(2, len(dests), dests)
        for name in dests:
            self.assertTrue(
                name.startswith("local.nanocobs@r3-"), f"not disambiguated: {name}"
            )
            self.assertEqual(
                self.tree_of(self.payload), self.tree_of(self.project / "vendor" / name)
            )

    def test_explicit_override_lands_exactly_where_it_says(self):
        manifest = self.manifest(
            self.entry(
                "local.nanocobs@r3", self.spec("local.nanocobs@r3"), vendor='"deps/cobs"'
            ),
            vendor_root=None,  # an override needs no VENDOR_ROOT at all
        )
        run = self.install(manifest)
        self.assertVendored(run, "local.nanocobs@r3", "copied", "absent")
        self.assertEqual(
            self.tree_of(self.payload), self.tree_of(self.project / "deps" / "cobs")
        )

    def test_absent_vendor_key_copies_nothing(self):
        manifest = self.manifest(
            self.entry("local.nanocobs@r3", self.spec("local.nanocobs@r3"), vendor=None)
        )
        run = self.install(manifest)
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertEqual({}, self.results(run))
        self.assertFalse((self.project / "vendor").exists())

    def test_vendor_false_copies_nothing(self):
        manifest = self.manifest(
            self.entry("local.nanocobs@r3", self.spec("local.nanocobs@r3"), vendor="false")
        )
        run = self.install(manifest)
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertEqual({}, self.results(run))
        self.assertFalse((self.project / "vendor").exists())

    def test_sync_vendors_the_same_way_install_does(self):
        manifest = self.manifest(
            self.entry("local.nanocobs@r3", self.spec("local.nanocobs@r3"))
        )
        run = self.sync(manifest)
        self.assertVendored(run, "local.nanocobs@r3", "copied", "absent")
        self.assertEqual(
            self.tree_of(self.payload), self.tree_of(self.project / "vendor" / "nanocobs")
        )


class TestVendorCollisions(VendorTestCase):
    def assertRefused(self, manifest: Path, *needles: str):
        run = self.install(manifest)
        self.assertNotEqual(0, run.returncode, run.stdout)
        for needle in needles:
            self.assertIn(needle, run.stderr)
        return run

    def test_two_overrides_naming_one_directory_are_refused_before_any_copy(self):
        manifest = self.manifest(
            self.entry("local.one@r1", self.spec("local.one@r1"), vendor='"deps/shared"')
            + "\n"
            + self.entry("local.two@r1", self.spec("local.two@r1"), vendor='"deps/shared"')
        )
        self.assertRefused(manifest, "local.one@r1", "local.two@r1", "collision")
        # The whole point of resolving up front: nothing was written on the way out.
        self.assertFalse((self.project / "deps").exists())

    def test_nested_destinations_are_refused(self):
        manifest = self.manifest(
            self.entry("local.one@r1", self.spec("local.one@r1"), vendor='"deps"')
            + "\n"
            + self.entry("local.two@r1", self.spec("local.two@r1"), vendor='"deps/inner"')
        )
        self.assertRefused(manifest, "nested", "local.one@r1", "local.two@r1")
        self.assertFalse((self.project / "deps").exists())

    @POSIX_ONLY
    def test_a_symlinked_destination_does_not_erase_what_it_points_at(self):
        """The wipe deletes the listing the drift check produced, and that listing was
        taken through the link -- so it names the target's contents, not the link."""
        target = self.project / "elsewhere"
        target.mkdir()
        (target / "precious.txt").write_text("not the package's\n", encoding="utf-8")
        (self.project / "link").symlink_to(target)

        manifest = self.manifest(
            self.entry(
                "local.nanocobs@r3", self.spec("local.nanocobs@r3"), vendor='"link"'
            )
        )
        # The link resolves to a directory holding something else, so the first run is
        # already a mismatch: it wipes and recopies.
        run = self.install(manifest)
        self.assertEqual(0, run.returncode, run.stderr)

        # remove_all unlinks the link and leaves the target; the copy then lands in a
        # real directory of its own.
        self.assertTrue(
            (target / "precious.txt").exists(),
            "vendoring through a symlinked destination erased the target's contents",
        )
        self.assertFalse((self.project / "link").is_symlink())
        self.assertEqual(
            self.tree_of(self.payload), self.tree_of(self.project / "link")
        )

    @POSIX_ONLY
    def test_destination_behind_a_symlink_out_of_the_project_is_refused(self):
        """A symlinked path component would put the wipe-and-recopy outside the project.

        vendor_resolve only sees the path as written, so this is caught on the way in to
        the copy, before remove_all follows the link.
        """
        outside = self.make_temp_dir("outside")
        (outside / "keepme.txt").write_text("not envy's to delete\n", encoding="utf-8")
        (self.project / "escape").symlink_to(outside)

        manifest = self.manifest(
            self.entry(
                "local.nanocobs@r3", self.spec("local.nanocobs@r3"), vendor='"escape/x"'
            ),
            vendor_root=None,
        )
        run = self.install(manifest)

        self.assertNotEqual(0, run.returncode, run.stdout)
        self.assertIn("outside the project", run.stderr)
        self.assertTrue((outside / "keepme.txt").exists())

    def test_derived_name_needs_a_vendor_root(self):
        manifest = self.manifest(
            self.entry("local.nanocobs@r3", self.spec("local.nanocobs@r3")),
            vendor_root=None,
        )
        self.assertRefused(manifest, "local.nanocobs@r3", "VENDOR_ROOT")

    def test_vendor_root_may_not_escape_the_project(self):
        manifest = self.manifest(
            self.entry("local.nanocobs@r3", self.spec("local.nanocobs@r3")),
            vendor_root="../outside",
        )
        self.assertRefused(manifest, "VENDOR_ROOT")

    def test_collision_is_checked_across_the_whole_manifest_not_just_the_target(self):
        # `envy install one` must still refuse a manifest whose two entries collide,
        # or the failure would surface only for whoever happened to build both.
        manifest = self.manifest(
            self.entry("local.one@r1", self.spec("local.one@r1"), vendor='"deps/shared"')
            + "\n"
            + self.entry("local.two@r1", self.spec("local.two@r1"), vendor='"deps/shared"')
        )
        run = self.run_envy("install", "--manifest", manifest, "local.one@r1")
        self.assertNotEqual(0, run.returncode, run.stdout)
        self.assertIn("collision", run.stderr)


class TestVendorSelectors(VendorTestCase):
    def test_spec_vendor_list_selects_a_subset(self):
        manifest = self.manifest(
            self.entry(
                "local.nanocobs@r3",
                self.spec(
                    "local.nanocobs@r3",
                    'VENDOR = { "include/**", "LICENSE", "!include/internal/**" }\n',
                ),
            )
        )
        run = self.install(manifest)
        self.assertVendored(run, "local.nanocobs@r3", "copied", "absent")

        dest = self.project / "vendor" / "nanocobs"
        self.assertEqual(
            {"include/lib.h": "#pragma once\n", "LICENSE": "MIT\n"}, self.tree_of(dest)
        )
        self.assertFalse((dest / "include" / "internal").exists())
        self.assertFalse((dest / "src").exists())

    def test_malformed_vendor_pattern_fails_before_any_fetch(self):
        manifest = self.manifest(
            self.entry(
                "local.nanocobs@r3",
                self.spec("local.nanocobs@r3", 'VENDOR = { "[unterminated" }\n'),
            )
        )
        run = self.install(manifest)
        self.assertNotEqual(0, run.returncode, run.stdout)
        self.assertIn("local.nanocobs@r3", run.stderr)
        # Rejected at spec_fetch, so nothing was downloaded on its behalf.
        self.assertEqual([], run.events("download_start"))


class TestVendorRefresh(VendorTestCase):
    def install_once(self, vendor_list: str = "") -> Path:
        self.manifest_path = self.manifest(
            self.entry(
                "local.nanocobs@r3", self.spec("local.nanocobs@r3", vendor_list)
            )
        )
        run = self.install(self.manifest_path)
        self.assertVendored(run, "local.nanocobs@r3", "copied", "absent")
        return self.project / "vendor" / "nanocobs"

    def test_second_run_is_a_no_op_with_a_partial_vendor_list(self):
        """A VENDOR list that names files, not directories, must still settle.

        The destination gains the directories holding those files, so the source-side
        digest only matches it if the selection covers them too. Without that, the
        package reports drift and is recopied on every single run.
        """
        dest = self.install_once('VENDOR = { "**/*.h", "LICENSE" }\n')
        self.assertEqual(
            {"include/lib.h": "#pragma once\n",
             "include/internal/secret.h": "// internal\n",
             "LICENSE": "MIT\n"},
            self.tree_of(dest),
        )

        run = self.install(self.manifest_path)
        self.assertVendored(run, "local.nanocobs@r3", "up_to_date", "current")

    def test_second_run_is_a_no_op_with_a_single_named_file(self):
        dest = self.install_once('VENDOR = { "src/lib.c" }\n')
        self.assertEqual({"src/lib.c": "int lib(void){return 0;}\n"}, self.tree_of(dest))

        run = self.install(self.manifest_path)
        self.assertVendored(run, "local.nanocobs@r3", "up_to_date", "current")

    def test_second_run_is_a_no_op(self):
        dest = self.install_once()
        before = {p: p.stat().st_mtime_ns for p in sorted(dest.rglob("*")) if p.is_file()}

        run = self.install(self.manifest_path)
        self.assertVendored(run, "local.nanocobs@r3", "up_to_date", "current")
        self.assertEqual(0, run.events("vendor_result")[0].raw["files"])
        after = {p: p.stat().st_mtime_ns for p in sorted(dest.rglob("*")) if p.is_file()}
        self.assertEqual(before, after, "an up-to-date vendor copy was rewritten")

    def test_edited_file_is_restored(self):
        dest = self.install_once()
        (dest / "src" / "lib.c").write_text("tampered\n", encoding="utf-8")

        run = self.install(self.manifest_path)
        self.assertVendored(run, "local.nanocobs@r3", "redeployed", "mismatch")
        self.assertEqual(self.tree_of(self.payload), self.tree_of(dest))

    def test_added_stray_file_is_removed(self):
        dest = self.install_once()
        (dest / "stray.txt").write_text("not mine\n", encoding="utf-8")

        run = self.install(self.manifest_path)
        self.assertVendored(run, "local.nanocobs@r3", "redeployed", "mismatch")
        self.assertFalse((dest / "stray.txt").exists())

    def test_deleted_file_is_restored(self):
        dest = self.install_once()
        (dest / "LICENSE").unlink()

        run = self.install(self.manifest_path)
        self.assertVendored(run, "local.nanocobs@r3", "redeployed", "mismatch")
        self.assertEqual(self.tree_of(self.payload), self.tree_of(dest))

    def test_deleted_destination_is_recreated(self):
        dest = self.install_once()
        rmtree_retry(dest)

        run = self.install(self.manifest_path)
        self.assertVendored(run, "local.nanocobs@r3", "copied", "absent")
        self.assertEqual(self.tree_of(self.payload), self.tree_of(dest))

    def test_changed_payload_redeploys(self):
        dest = self.install_once()
        self.write_payload("src/lib.c", "int lib(void){return 1;}\n")
        self.write_payload("NEWFILE", "added upstream\n")

        # A new revision is a new cache entry, so the vendor copy is intact but no longer
        # matches what the package now installs.
        manifest = self.manifest(
            self.entry("local.nanocobs@r4", self.spec("local.nanocobs@r4"),
                       vendor='"vendor/nanocobs"')
        )
        run = self.install(manifest)
        self.assertVendored(run, "local.nanocobs@r4", "redeployed", "mismatch")
        self.assertEqual(self.tree_of(self.payload), self.tree_of(dest))

    @POSIX_ONLY
    def test_an_executable_payload_file_stays_executable(self):
        """A clone carries the mode; a copy_file fallback has to be told to. Either way
        the digest covers the exec bit, so getting it wrong also never settles."""
        tool = self.write_payload("bin/tool", "#!/bin/sh\necho hi\n")
        tool.chmod(0o755)
        manifest = self.manifest(
            self.entry("local.nanocobs@r3", self.spec("local.nanocobs@r3"))
        )
        self.assertVendored(
            self.install(manifest), "local.nanocobs@r3", "copied", "absent"
        )

        dest = self.project / "vendor" / "nanocobs"
        self.assertTrue(os.access(dest / "bin" / "tool", os.X_OK))
        self.assertVendored(
            self.install(manifest), "local.nanocobs@r3", "up_to_date", "current"
        )

    def test_a_payload_of_many_files_copies_exactly_and_settles(self):
        """The copy hands each worker a slice of 32 files, so a payload of four never
        leaves the calling thread and proves nothing about the pool."""
        for i in range(200):
            self.write_payload(f"gen/{i % 7}/file{i}.txt", f"payload {i}\n")
        manifest = self.manifest(
            self.entry("local.nanocobs@r3", self.spec("local.nanocobs@r3"))
        )

        run = self.install(manifest)
        self.assertVendored(run, "local.nanocobs@r3", "copied", "absent")
        dest = self.project / "vendor" / "nanocobs"
        self.assertEqual(self.tree_of(self.payload), self.tree_of(dest))
        self.assertVendored(
            self.install(manifest), "local.nanocobs@r3", "up_to_date", "current"
        )

    @POSIX_ONLY
    def test_exec_bit_drift_is_detected(self):
        dest = self.install_once()
        target = dest / "src" / "lib.c"
        target.chmod(target.stat().st_mode | 0o111)

        run = self.install(self.manifest_path)
        self.assertVendored(run, "local.nanocobs@r3", "redeployed", "mismatch")
        self.assertFalse(os.access(target, os.X_OK))


class TestVendorRedeployMatrix(VendorTestCase):
    """The rest of the redeploy decision, case by case.

    The phase compares three things: whether the destination exists, its whole-tree
    digest against the project stamp, and that stamp against the cache's pristine
    digest. Every branch and every way to reach it belongs here.
    """

    def install_once(self, vendor_list: str = "", install_extra: str = "") -> Path:
        self.manifest_path = self.manifest(
            self.entry(
                "local.nanocobs@r3",
                self.spec("local.nanocobs@r3", vendor_list, install_extra),
            )
        )
        run = self.install(self.manifest_path)
        self.assertVendored(run, "local.nanocobs@r3", "copied", "absent")
        return self.project / "vendor" / "nanocobs"

    # -- a redeploy has to settle -------------------------------------------

    def test_a_redeploy_settles_on_the_next_run(self):
        """Drift, then recovery, then quiet. A redeploy that never converges is the
        failure mode a single drift assertion cannot see."""
        dest = self.install_once()
        (dest / "src" / "lib.c").write_text("tampered\n", encoding="utf-8")

        self.assertVendored(
            self.install(self.manifest_path), "local.nanocobs@r3", "redeployed", "mismatch"
        )
        self.assertVendored(
            self.install(self.manifest_path), "local.nanocobs@r3", "up_to_date", "current"
        )

    # -- stale: the source moved --------------------------------------------

    def test_changed_vendor_list_redeploys(self):
        """Editing a spec's VENDOR list changes nothing about the package's cache key,
        which is exactly why the pristine stamp is named for the selector set."""
        dest = self.install_once('VENDOR = { "include/**" }\n')
        self.assertEqual(
            {"include/lib.h", "include/internal/secret.h"}, set(self.tree_of(dest))
        )

        # Same identity, same options, same payload -- only the selection changed.
        self.spec("local.nanocobs@r3", 'VENDOR = { "include/lib.h", "LICENSE" }\n')
        run = self.install(self.manifest_path)

        self.assertVendored(run, "local.nanocobs@r3", "redeployed", "mismatch")
        self.assertEqual({"include/lib.h", "LICENSE"}, set(self.tree_of(dest)))
        self.assertVendored(
            self.install(self.manifest_path), "local.nanocobs@r3", "up_to_date", "current"
        )

    def test_a_checked_in_vendor_tree_is_recognized_on_a_fresh_machine(self):
        """No project-side state exists, so correct content is correct content.

        This is the committed-vendor-tree case: someone else ran envy, the result went
        into git, and this machine has never vendored anything. The package's own digest
        is the only record, so the copy is adopted rather than rewritten.
        """
        dest = self.install_once()
        contents = self.tree_of(dest)

        # Everything envy could have remembered, gone: a cache root it has never seen.
        # The old one is left to its own cleanup rather than raced with.
        self.cache_root = self.make_temp_dir("cache2")

        run = self.install(self.manifest_path)
        self.assertVendored(run, "local.nanocobs@r3", "up_to_date", "current")
        self.assertEqual(contents, self.tree_of(dest))

    def test_a_hand_built_vendor_tree_with_the_wrong_contents_is_replaced(self):
        """The mirror image: content that merely looks plausible is still replaced."""
        dest = self.project / "vendor" / "nanocobs"
        (dest / "src").mkdir(parents=True)
        (dest / "src" / "lib.c").write_text("someone guessed\n", encoding="utf-8")

        self.manifest_path = self.manifest(
            self.entry("local.nanocobs@r3", self.spec("local.nanocobs@r3"))
        )
        run = self.install(self.manifest_path)

        self.assertVendored(run, "local.nanocobs@r3", "redeployed", "mismatch")
        self.assertEqual(self.tree_of(self.payload), self.tree_of(dest))

    def test_wiped_cache_alone_does_not_force_a_redeploy(self):
        """The pristine digest is recomputed from the rebuilt payload. Same bytes, same
        digest, so an evicted cache costs a hash and not a recopy."""
        dest = self.install_once()
        before = {p: p.stat().st_mtime_ns for p in sorted(dest.rglob("*")) if p.is_file()}

        rmtree_retry(self.cache_root)
        run = self.install(self.manifest_path)

        self.assertVendored(run, "local.nanocobs@r3", "up_to_date", "current")
        after = {p: p.stat().st_mtime_ns for p in sorted(dest.rglob("*")) if p.is_file()}
        self.assertEqual(before, after)

    # -- drifted: the destination moved --------------------------------------

    def test_added_empty_directory_is_a_mismatch(self):
        dest = self.install_once()
        (dest / "brand-new").mkdir()

        self.assertVendored(
            self.install(self.manifest_path), "local.nanocobs@r3", "redeployed", "mismatch"
        )
        self.assertFalse((dest / "brand-new").exists())

    def test_removed_empty_directory_is_a_mismatch(self):
        """Directories fold into the digest precisely so an empty one can go missing."""
        (self.payload / "placeholder").mkdir()
        dest = self.install_once()
        self.assertTrue((dest / "placeholder").is_dir())

        (dest / "placeholder").rmdir()
        self.assertVendored(
            self.install(self.manifest_path), "local.nanocobs@r3", "redeployed", "mismatch"
        )
        self.assertTrue((dest / "placeholder").is_dir())

    def test_stray_file_the_vendor_list_would_not_select_is_still_drift(self):
        """The destination is hashed whole, not through VENDOR. A file the selector
        would have skipped is still a file sitting in the vendored tree."""
        dest = self.install_once('VENDOR = { "include/**" }\n')
        (dest / "notes.md").write_text("mine, not envy's\n", encoding="utf-8")

        self.assertVendored(
            self.install(self.manifest_path), "local.nanocobs@r3", "redeployed", "mismatch"
        )
        self.assertFalse((dest / "notes.md").exists())

    def test_stray_directory_the_vendor_list_would_not_select_is_still_drift(self):
        dest = self.install_once('VENDOR = { "include/**" }\n')
        (dest / "scratch").mkdir()

        self.assertVendored(
            self.install(self.manifest_path), "local.nanocobs@r3", "redeployed", "mismatch"
        )
        self.assertFalse((dest / "scratch").exists())

    def test_emptied_destination_is_a_mismatch_not_absent(self):
        """The directory is still there, so this is not the absent branch."""
        dest = self.install_once()
        for child in dest.iterdir():
            rmtree_retry(child) if child.is_dir() else child.unlink()
        self.assertTrue(dest.is_dir())

        self.assertVendored(
            self.install(self.manifest_path), "local.nanocobs@r3", "redeployed", "mismatch"
        )
        self.assertEqual(self.tree_of(self.payload), self.tree_of(dest))

    def test_file_replaced_by_a_directory_is_a_mismatch(self):
        dest = self.install_once()
        (dest / "LICENSE").unlink()
        (dest / "LICENSE").mkdir()

        self.assertVendored(
            self.install(self.manifest_path), "local.nanocobs@r3", "redeployed", "mismatch"
        )
        self.assertTrue((dest / "LICENSE").is_file())

    def test_touched_mtimes_alone_are_not_a_mismatch(self):
        """The digest is over content, so a checkout or a copy that resets timestamps
        must not trigger a recopy."""
        import os

        dest = self.install_once()
        for path in dest.rglob("*"):
            os.utime(path, (1, 1))

        self.assertVendored(
            self.install(self.manifest_path), "local.nanocobs@r3", "up_to_date", "current"
        )

    @POSIX_ONLY
    def test_repointed_symlink_is_a_mismatch(self):
        # envy.copy follows symlinks, so the payload cannot carry one into the cache --
        # INSTALL makes it there, which is also how a real spec would.
        self.write_payload("real.txt", "target\n")
        dest = self.install_once(
            install_extra='envy.run("ln -s real.txt " .. install_dir .. "/link.txt")'
        )
        self.assertTrue(
            (dest / "link.txt").is_symlink(), "a vendored symlink must stay a symlink"
        )

        (dest / "link.txt").unlink()
        (dest / "link.txt").symlink_to("LICENSE")
        self.assertVendored(
            self.install(self.manifest_path), "local.nanocobs@r3", "redeployed", "mismatch"
        )
        self.assertEqual("real.txt", os.readlink(dest / "link.txt"))

    # -- the destination itself moved ----------------------------------------

    def test_moved_destination_copies_afresh_and_leaves_the_old_one(self):
        """envy does not prune: a destination it no longer owns stays where it is."""
        old = self.install_once()

        manifest = self.manifest(
            self.entry(
                "local.nanocobs@r3", self.spec("local.nanocobs@r3"), vendor='"deps/cobs"'
            )
        )
        run = self.install(manifest)

        self.assertVendored(run, "local.nanocobs@r3", "copied", "absent")
        self.assertEqual(self.tree_of(self.payload),
                         self.tree_of(self.project / "deps" / "cobs"))
        self.assertTrue(old.exists(), "pruning is not a thing envy does")

    def test_one_package_mismatching_leaves_the_other_alone(self):
        manifest = self.manifest(
            self.entry("local.one@r1", self.spec("local.one@r1"))
            + "\n"
            + self.entry("local.two@r1", self.spec("local.two@r1"))
        )
        self.assertEqual(0, self.install(manifest).returncode)

        (self.project / "vendor" / "one" / "LICENSE").write_text("x\n", encoding="utf-8")
        results = self.results(self.install(manifest))

        self.assertEqual("redeployed", results["local.one@r1"]["action"])
        self.assertEqual("mismatch", results["local.one@r1"]["reason"])
        self.assertEqual("up_to_date", results["local.two@r1"]["action"])


class TestVendorAutoSyncExemption(VendorTestCase):
    """`vendor = { auto_sync = false }`: report a mismatch, do not repair it."""

    def exempt_install(self, vendor: str = "{ auto_sync = false }",
                       vendor_list: str = "") -> Path:
        self.manifest_path = self.manifest(
            self.entry(
                "local.nanocobs@r3",
                self.spec("local.nanocobs@r3", vendor_list),
                vendor=vendor,
            )
        )
        run = self.install(self.manifest_path)
        self.assertVendored(run, "local.nanocobs@r3", "copied", "absent")
        return self.project / "vendor" / "nanocobs"

    # -- the exemption applies only to a mismatch ----------------------------

    def test_absent_destination_is_still_copied(self):
        """Nothing to preserve, so exemption has nothing to say."""
        dest = self.exempt_install()
        self.assertEqual(self.tree_of(self.payload), self.tree_of(dest))

    def test_matching_destination_is_still_quiet(self):
        dest = self.exempt_install()
        run = self.install(self.manifest_path)

        self.assertVendored(run, "local.nanocobs@r3", "up_to_date", "current")
        self.assertNotIn("no longer matches", run.stderr)
        self.assertEqual(self.tree_of(self.payload), self.tree_of(dest))

    def test_deleted_destination_is_recreated(self):
        dest = self.exempt_install()
        rmtree_retry(dest)

        run = self.install(self.manifest_path)
        self.assertVendored(run, "local.nanocobs@r3", "copied", "absent")
        self.assertEqual(self.tree_of(self.payload), self.tree_of(dest))

    # -- a mismatch is kept, and said out loud -------------------------------

    def test_edited_file_is_kept_and_reported(self):
        dest = self.exempt_install()
        (dest / "src" / "lib.c").write_text("mine now\n", encoding="utf-8")
        before = self.tree_of(dest)

        run = self.install(self.manifest_path)

        self.assertVendored(run, "local.nanocobs@r3", "kept", "mismatch")
        self.assertEqual(before, self.tree_of(dest), "the edit was overwritten")
        self.assertIn("no longer matches", run.stderr)
        self.assertIn(self.shown(dest), run.stderr)
        self.assertIn("auto_sync", run.stderr)

    def test_stray_file_is_kept(self):
        dest = self.exempt_install()
        (dest / "notes.md").write_text("mine\n", encoding="utf-8")

        self.assertVendored(
            self.install(self.manifest_path), "local.nanocobs@r3", "kept", "mismatch"
        )
        self.assertTrue((dest / "notes.md").exists())

    def test_deleted_file_is_not_restored(self):
        dest = self.exempt_install()
        (dest / "LICENSE").unlink()

        self.assertVendored(
            self.install(self.manifest_path), "local.nanocobs@r3", "kept", "mismatch"
        )
        self.assertFalse((dest / "LICENSE").exists())

    def test_a_changed_package_is_also_kept(self):
        """The mismatch can come from the package side; the answer is the same."""
        dest = self.exempt_install()
        self.write_payload("src/lib.c", "int lib(void){return 1;}\n")

        manifest = self.manifest(
            self.entry(
                "local.nanocobs@r4",
                self.spec("local.nanocobs@r4"),
                vendor='{ path = "vendor/nanocobs", auto_sync = false }',
            )
        )
        run = self.install(manifest)

        self.assertVendored(run, "local.nanocobs@r4", "kept", "mismatch")
        self.assertEqual("int lib(void){return 0;}\n", (dest / "src/lib.c").read_text())

    def test_keeping_is_stable_across_runs(self):
        """It reports every time and never oscillates into repairing."""
        dest = self.exempt_install()
        (dest / "src" / "lib.c").write_text("mine now\n", encoding="utf-8")
        before = self.tree_of(dest)

        for _ in range(3):
            run = self.install(self.manifest_path)
            self.assertVendored(run, "local.nanocobs@r3", "kept", "mismatch")
            self.assertIn("no longer matches", run.stderr)
            self.assertEqual(before, self.tree_of(dest))

    def test_reverting_the_edit_goes_quiet_again(self):
        dest = self.exempt_install()
        original = (dest / "src" / "lib.c").read_text()
        (dest / "src" / "lib.c").write_text("mine now\n", encoding="utf-8")
        self.assertVendored(
            self.install(self.manifest_path), "local.nanocobs@r3", "kept", "mismatch"
        )

        (dest / "src" / "lib.c").write_text(original, encoding="utf-8")
        run = self.install(self.manifest_path)
        self.assertVendored(run, "local.nanocobs@r3", "up_to_date", "current")
        self.assertNotIn("no longer matches", run.stderr)

    # -- it is per package ----------------------------------------------------

    def test_exemption_does_not_leak_to_a_neighbour(self):
        manifest = self.manifest(
            self.entry(
                "local.one@r1",
                self.spec("local.one@r1"),
                vendor="{ auto_sync = false }",
            )
            + "\n"
            + self.entry("local.two@r1", self.spec("local.two@r1"))
        )
        self.assertEqual(0, self.install(manifest).returncode)

        for name in ("one", "two"):
            license = self.project / "vendor" / name / "LICENSE"
            license.write_text("x\n", encoding="utf-8")
        results = self.results(self.install(manifest))

        self.assertEqual("kept", results["local.one@r1"]["action"])
        self.assertEqual("redeployed", results["local.two@r1"]["action"])
        self.assertEqual("x\n", (self.project / "vendor/one/LICENSE").read_text())
        self.assertEqual("MIT\n", (self.project / "vendor/two/LICENSE").read_text())

    def test_explicit_true_syncs_as_usual(self):
        self.manifest_path = self.manifest(
            self.entry(
                "local.nanocobs@r3",
                self.spec("local.nanocobs@r3"),
                vendor="{ auto_sync = true }",
            )
        )
        self.assertEqual(0, self.install(self.manifest_path).returncode)
        dest = self.project / "vendor" / "nanocobs"
        (dest / "LICENSE").write_text("x\n", encoding="utf-8")

        run = self.install(self.manifest_path)
        self.assertVendored(run, "local.nanocobs@r3", "redeployed", "mismatch")
        self.assertEqual("MIT\n", (dest / "LICENSE").read_text())

    # -- the table form spells out what the shorthands mean --------------------

    def test_path_in_the_table_lands_where_the_string_form_would(self):
        for vendor in ('"deps/cobs"', '{ path = "deps/cobs" }'):
            with self.subTest(vendor=vendor):
                self.setUp()
                manifest = self.manifest(
                    self.entry(
                        "local.nanocobs@r3",
                        self.spec("local.nanocobs@r3"),
                        vendor=vendor,
                    )
                )
                self.assertEqual(0, self.install(manifest).returncode)
                self.assertEqual(
                    self.tree_of(self.payload),
                    self.tree_of(self.project / "deps" / "cobs"),
                )

    def test_path_and_auto_sync_together(self):
        manifest = self.manifest(
            self.entry(
                "local.nanocobs@r3",
                self.spec("local.nanocobs@r3"),
                vendor='{ path = "deps/cobs", auto_sync = false }',
            )
        )
        self.assertEqual(0, self.install(manifest).returncode)
        dest = self.project / "deps" / "cobs"
        (dest / "LICENSE").write_text("x\n", encoding="utf-8")

        self.assertVendored(self.install(manifest), "local.nanocobs@r3",
                            "kept", "mismatch")
        self.assertEqual("x\n", (dest / "LICENSE").read_text())

    def test_an_empty_table_behaves_like_vendor_true(self):
        manifest = self.manifest(
            self.entry("local.nanocobs@r3", self.spec("local.nanocobs@r3"), vendor="{}")
        )
        self.assertEqual(0, self.install(manifest).returncode)
        self.assertEqual(
            self.tree_of(self.payload),
            self.tree_of(self.project / "vendor" / "nanocobs"),
        )

    # -- refusing what cannot mean anything -----------------------------------

    def test_vendor_overwrite_is_no_longer_a_key(self):
        manifest = self.manifest(
            self.entry(
                "local.nanocobs@r3",
                self.spec("local.nanocobs@r3"),
                vendor_overwrite="false",
            )
        )
        run = self.install(manifest)
        self.assertNotEqual(0, run.returncode, run.stdout)
        self.assertIn("vendor_overwrite", run.stderr)

    def test_an_unknown_vendor_table_key_is_refused(self):
        manifest = self.manifest(
            self.entry(
                "local.nanocobs@r3",
                self.spec("local.nanocobs@r3"),
                vendor='{ pathh = "deps/cobs" }',
            )
        )
        run = self.install(manifest)
        self.assertNotEqual(0, run.returncode, run.stdout)
        self.assertIn("pathh", run.stderr)

    def test_non_boolean_auto_sync_is_refused(self):
        manifest = self.manifest(
            self.entry(
                "local.nanocobs@r3",
                self.spec("local.nanocobs@r3"),
                vendor='{ auto_sync = "sometimes" }',
            )
        )
        run = self.install(manifest)
        self.assertNotEqual(0, run.returncode, run.stdout)
        self.assertIn("auto_sync", run.stderr)

    def test_an_empty_table_path_is_refused(self):
        manifest = self.manifest(
            self.entry(
                "local.nanocobs@r3", self.spec("local.nanocobs@r3"), vendor='{ path = "" }'
            )
        )
        run = self.install(manifest)
        self.assertNotEqual(0, run.returncode, run.stdout)
        self.assertIn("empty", run.stderr)


class TestVendorProgress(VendorTestCase):
    """The copy draws a bar, and its last frame says what happened."""

    # Every vendor bar's status ends in one of these; the fetch bar draws on the same
    # row, so matching on "%" alone picks up someone else's progress.
    WORDS = ("vendored", "kept:")

    def bar_rows(self, run) -> list[str]:
        """Vendor progress rows envy drew for the package, in order."""
        def is_vendor_bar(ln):
            return ("[local.nanocobs@r3]" in ln and "%" in ln
                    and any(w in ln for w in self.WORDS))

        return [ln.strip() for ln in run.stderr.splitlines() if is_vendor_bar(ln)]

    def install_drawing(self, *extra, **kw):
        """Off a TTY a row's last frame prints as it is set: no throttle to wait out."""
        return self.install(self.manifest_path, *extra, **kw)

    def setUp(self):
        super().setUp()
        # Enough files that the copy is worth drawing at all, spread over subdirectories
        # so the walk has directories to create that the bar does not count.
        for i in range(40):
            self.write_payload(f"gen/{i % 4}/file{i}.txt", f"payload {i}\n")
        self.manifest_path = self.manifest(
            self.entry("local.nanocobs@r3", self.spec("local.nanocobs@r3"))
        )

    def test_first_vendor_draws_a_bar_that_finishes_full(self):
        run = self.install_drawing()
        self.assertVendored(run, "local.nanocobs@r3", "copied", "absent")

        rows = self.bar_rows(run)
        self.assertTrue(rows, f"vendor drew no progress row:\n{run.stderr}")
        self.assertIn("100.0%", rows[-1], f"bar did not finish full: {rows[-1]}")
        self.assertIn("vendored", rows[-1])
        self.assertIn("files", rows[-1])
        self.assertNotIn("re-vendored", rows[-1], "a first copy is not a re-vendor")

    def test_a_dirty_redeploy_says_so_on_the_bar(self):
        self.install_drawing()
        dest = self.project / "vendor" / "nanocobs"
        (dest / "src" / "lib.c").write_text("tampered\n", encoding="utf-8")

        run = self.install_drawing()
        self.assertVendored(run, "local.nanocobs@r3", "redeployed", "mismatch")

        rows = self.bar_rows(run)
        self.assertTrue(rows, f"vendor drew no progress row:\n{run.stderr}")
        self.assertIn("100.0%", rows[-1])
        self.assertIn("re-vendored", rows[-1], f"row does not name the cause: {rows[-1]}")
        self.assertIn("dirty", rows[-1], f"row does not name the cause: {rows[-1]}")

    def test_the_bar_counts_the_files_it_copied(self):
        run = self.install_drawing()
        rows = self.bar_rows(run)

        copied = run.events("vendor_result")[0].raw["files"]
        self.assertGreater(copied, 40)
        self.assertIn(f"{copied} files", rows[-1], f"count not on the row: {rows[-1]}")

    def test_an_up_to_date_package_draws_no_bar(self):
        """Nothing was copied, so there is no progress to report."""
        self.install_drawing()
        run = self.install_drawing()

        self.assertVendored(run, "local.nanocobs@r3", "up_to_date", "current")
        self.assertEqual([], self.bar_rows(run), "nothing was copied, so nothing to draw")

    def test_a_kept_package_reports_in_the_warning_and_draws_nothing(self):
        """`auto_sync = false` copies nothing, so the warning is the whole report.

        A row would say the same thing twice, and the completion phase deletes it again
        for want of a write -- on a terminal that is a frame painted and taken back.
        """
        self.manifest_path = self.manifest(
            self.entry(
                "local.nanocobs@r3",
                self.spec("local.nanocobs@r3"),
                vendor="{ auto_sync = false }",
            )
        )
        self.install_drawing()
        dest = self.project / "vendor" / "nanocobs"
        (dest / "src" / "lib.c").write_text("mine\n", encoding="utf-8")

        run = self.install_drawing()
        self.assertVendored(run, "local.nanocobs@r3", "kept", "mismatch")
        self.assertEqual([], self.bar_rows(run), "nothing was copied, so nothing to draw")
        self.assertIn("no longer matches", run.stderr)


class TestVendorRejections(VendorTestCase):
    def test_user_managed_package_cannot_be_vendored(self):
        spec = self.write_spec(
            "hosted.lua",
            """IDENTITY = "local.hosted@r1"
USER_MANAGED = true
SETUP = { main = { CHECK = function() return true end,
                   INSTALL = function() end } }
""",
            directory=self.project,
        )
        manifest = self.manifest(self.entry("local.hosted@r1", spec))
        run = self.install(manifest)

        self.assertNotEqual(0, run.returncode, run.stdout)
        self.assertIn("local.hosted@r1", run.stderr)
        self.assertIn("USER_MANAGED", run.stderr)
        # Rejected at spec_fetch, so no payload phase ever ran on its behalf.
        self.assertEqual([], run.events("vendor_result"))

    def test_vendor_is_not_a_dependency_entry_key(self):
        dep = self.spec("local.dep@r1")
        spec = self.write_spec(
            "consumer.lua",
            f"""IDENTITY = "local.consumer@r1"
FETCH = {{ source = "file://{self.lua_path(self.seed)}" }}
DEPENDENCIES = {{
  {{ spec = "local.dep@r1", source = "{self.lua_path(dep)}", vendor = true }},
}}
INSTALL = function(install_dir) end
""",
            directory=self.project,
        )
        manifest = self.manifest(self.entry("local.consumer@r1", spec, vendor=None))
        run = self.install(manifest)

        self.assertNotEqual(0, run.returncode, run.stdout)
        self.assertIn("vendor", run.stderr)

    def test_vendor_must_be_a_boolean_or_a_path(self):
        manifest = self.manifest(
            self.entry("local.nanocobs@r3", self.spec("local.nanocobs@r3"), vendor="42")
        )
        run = self.install(manifest)
        self.assertNotEqual(0, run.returncode, run.stdout)
        self.assertIn("vendor", run.stderr)


class TestVendorCommand(VendorTestCase):
    """`envy vendor`: restore a vendored tree on demand rather than as part of a run.

    The command runs its targets to completion like any other, so the assertions are
    the same `vendor_result` ones; what is new is which packages the plan covers and
    whether `auto_sync` still has the last word.
    """

    def vendor(self, manifest: Path, *extra, **kwargs):
        return self.run_envy("vendor", *extra, "--manifest", manifest, **kwargs)

    def one(self, vendor: str = "true") -> Path:
        """One vendored package, installed. Returns its manifest."""
        self.manifest_path = self.manifest(
            self.entry(
                "local.nanocobs@r3", self.spec("local.nanocobs@r3"), vendor=vendor
            )
        )
        self.dest = self.project / "vendor" / "nanocobs"
        self.assertVendored(
            self.install(self.manifest_path), "local.nanocobs@r3", "copied", "absent"
        )
        return self.manifest_path

    def two(self) -> Path:
        manifest = self.manifest(
            self.entry("local.one@r1", self.spec("local.one@r1"))
            + "\n"
            + self.entry("local.two@r1", self.spec("local.two@r1"))
        )
        self.assertEqual(0, self.install(manifest).returncode)
        return manifest

    # -- restoring -----------------------------------------------------------

    def test_a_deleted_destination_is_copied_afresh(self):
        manifest = self.one()
        rmtree_retry(self.dest)

        run = self.vendor(manifest, "local.nanocobs@r3")
        self.assertVendored(run, "local.nanocobs@r3", "copied", "absent")
        self.assertEqual(self.tree_of(self.payload), self.tree_of(self.dest))

    def test_an_edited_destination_is_restored(self):
        manifest = self.one()
        (self.dest / "src" / "lib.c").write_text("mine\n", encoding="utf-8")

        run = self.vendor(manifest, "local.nanocobs@r3")
        self.assertVendored(run, "local.nanocobs@r3", "redeployed", "mismatch")
        self.assertEqual(self.tree_of(self.payload), self.tree_of(self.dest))

    def test_the_command_reports_what_it_did(self):
        """The phase's row is overwritten by the completion row, so the command says it."""
        manifest = self.one()
        (self.dest / "LICENSE").write_text("mine\n", encoding="utf-8")

        run = self.vendor(manifest, "local.nanocobs@r3")
        self.assertIn("re-vendored", run.stderr)
        self.assertIn(self.shown(self.dest), run.stderr)

    def test_the_report_names_the_destination_before_the_cause(self):
        """'N files: contents were dirty to <dir>' reads as if the directory were dirt."""
        manifest = self.one()
        (self.dest / "LICENSE").write_text("mine\n", encoding="utf-8")

        run = self.vendor(manifest, "local.nanocobs@r3")
        self.assertRegex(
            run.stderr, r"re-vendored \d+ files to \S+: contents were dirty"
        )

    def test_two_queries_naming_one_package_report_it_once(self):
        """The dest is on the command's line and nowhere else, so it counts the lines."""
        manifest = self.one()
        (self.dest / "LICENSE").write_text("mine\n", encoding="utf-8")

        run = self.vendor(manifest, "local.nanocobs@r3", "nanocobs")
        self.assertVendored(run, "local.nanocobs@r3", "redeployed", "mismatch")
        self.assertEqual(1, run.stderr.count(self.shown(self.dest)), run.stderr)

    def test_an_up_to_date_destination_is_left_alone(self):
        manifest = self.one()
        before = self.tree_of(self.dest)

        run = self.vendor(manifest, "local.nanocobs@r3")
        self.assertVendored(run, "local.nanocobs@r3", "up_to_date", "current")
        self.assertIn("up to date", run.stderr)
        self.assertEqual(before, self.tree_of(self.dest))

    def test_restoring_converges(self):
        manifest = self.one()
        (self.dest / "stray.txt").write_text("mine\n", encoding="utf-8")

        self.assertVendored(
            self.vendor(manifest, "local.nanocobs@r3"),
            "local.nanocobs@r3",
            "redeployed",
            "mismatch",
        )
        self.assertVendored(
            self.vendor(manifest, "local.nanocobs@r3"),
            "local.nanocobs@r3",
            "up_to_date",
            "current",
        )

    # -- selecting -----------------------------------------------------------

    def test_all_covers_every_vendored_package(self):
        manifest = self.two()
        for name in ("one", "two"):
            (self.project / "vendor" / name / "LICENSE").write_text("x\n")

        run = self.vendor(manifest, "--all")
        self.assertVendored(run, "local.one@r1", "redeployed", "mismatch")
        self.assertVendored(run, "local.two@r1", "redeployed", "mismatch")
        for name in ("one", "two"):
            self.assertEqual(
                "MIT\n", (self.project / "vendor" / name / "LICENSE").read_text()
            )

    def test_a_query_leaves_every_other_destination_as_it_found_it(self):
        manifest = self.two()
        for name in ("one", "two"):
            (self.project / "vendor" / name / "LICENSE").write_text("x\n")

        run = self.vendor(manifest, "local.one@r1")
        self.assertVendored(run, "local.one@r1", "redeployed", "mismatch")
        self.assertNotIn("local.two@r1", self.results(run))
        self.assertEqual("MIT\n", (self.project / "vendor/one/LICENSE").read_text())
        self.assertEqual("x\n", (self.project / "vendor/two/LICENSE").read_text())

    def test_a_vendored_dependency_of_a_target_is_not_swept_along(self):
        """The plan is resolved over the whole manifest, so a collision with a package
        nobody named is still an error -- but only what was named is copied."""
        dep = self.spec("local.dep@r1")
        consumer = self.write_spec(
            "consumer.lua",
            f"""IDENTITY = "local.consumer@r1"
FETCH = {{ source = "file://{self.lua_path(self.seed)}" }}
DEPENDENCIES = {{
  {{ spec = "local.dep@r1", source = "{self.lua_path(dep)}" }},
}}
INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  envy.copy("{self.lua_path(self.payload)}", install_dir)
end
""",
            directory=self.project,
        )
        manifest = self.manifest(
            self.entry("local.consumer@r1", consumer)
            + "\n"
            + self.entry("local.dep@r1", dep)
        )
        self.assertEqual(0, self.install(manifest).returncode)
        for name in ("consumer", "dep"):
            (self.project / "vendor" / name / "LICENSE").write_text("x\n")

        run = self.vendor(manifest, "local.consumer@r1")
        self.assertVendored(run, "local.consumer@r1", "redeployed", "mismatch")
        self.assertNotIn("local.dep@r1", self.results(run))
        self.assertEqual("MIT\n", (self.project / "vendor/consumer/LICENSE").read_text())
        self.assertEqual("x\n", (self.project / "vendor/dep/LICENSE").read_text())

    def test_a_package_that_is_not_vendored_is_an_error(self):
        manifest = self.manifest(
            self.entry("local.one@r1", self.spec("local.one@r1"))
            + "\n"
            + self.entry("local.two@r1", self.spec("local.two@r1"), vendor=None)
        )
        run = self.vendor(manifest, "local.two@r1")

        self.assertNotEqual(0, run.returncode, run.stdout)
        self.assertIn("local.two@r1", run.stderr)
        self.assertIn("not vendored", run.stderr)

    def test_a_manifest_that_vendors_nothing_is_an_error(self):
        manifest = self.manifest(
            self.entry("local.one@r1", self.spec("local.one@r1"), vendor=None)
        )
        run = self.vendor(manifest, "--all")

        self.assertNotEqual(0, run.returncode, run.stdout)
        self.assertIn("asks to be vendored", run.stderr)

    # -- --force -------------------------------------------------------------

    def test_an_exempt_package_is_kept_without_force(self):
        manifest = self.one(vendor="{ auto_sync = false }")
        (self.dest / "src" / "lib.c").write_text("mine\n", encoding="utf-8")

        run = self.vendor(manifest, "local.nanocobs@r3")
        self.assertVendored(run, "local.nanocobs@r3", "kept", "mismatch")
        self.assertEqual("mine\n", (self.dest / "src" / "lib.c").read_text())

    def test_force_repairs_an_exempt_package(self):
        manifest = self.one(vendor="{ auto_sync = false }")
        (self.dest / "src" / "lib.c").write_text("mine\n", encoding="utf-8")

        run = self.vendor(manifest, "local.nanocobs@r3", "--force")
        self.assertVendored(run, "local.nanocobs@r3", "redeployed", "mismatch")
        self.assertEqual(self.tree_of(self.payload), self.tree_of(self.dest))

    def test_the_warning_names_the_flag_that_overrides_it(self):
        manifest = self.one(vendor="{ auto_sync = false }")
        (self.dest / "src" / "lib.c").write_text("mine\n", encoding="utf-8")

        self.assertIn("--force", self.vendor(manifest, "local.nanocobs@r3").stderr)

    def test_force_changes_nothing_where_auto_sync_is_on(self):
        manifest = self.one()
        (self.dest / "src" / "lib.c").write_text("mine\n", encoding="utf-8")

        run = self.vendor(manifest, "local.nanocobs@r3", "--force")
        self.assertVendored(run, "local.nanocobs@r3", "redeployed", "mismatch")
        self.assertEqual(self.tree_of(self.payload), self.tree_of(self.dest))

    def test_force_on_an_up_to_date_package_is_still_a_no_op(self):
        """--force lifts the exemption; it is not "copy regardless"."""
        manifest = self.one()

        run = self.vendor(manifest, "local.nanocobs@r3", "--force")
        self.assertVendored(run, "local.nanocobs@r3", "up_to_date", "current")

    # -- --threads -----------------------------------------------------------

    def test_the_thread_count_does_not_change_what_lands(self):
        """A copy is only parallel if the answer is the same however wide it ran."""
        for i in range(120):
            self.write_payload(f"gen/{i % 5}/file{i}.txt", f"payload {i}\n")
        manifest = self.one()

        for count in ("1", "2", "8"):
            rmtree_retry(self.dest)
            run = self.vendor(manifest, "local.nanocobs@r3", "--threads", count)
            self.assertVendored(run, "local.nanocobs@r3", "copied", "absent")
            self.assertEqual(self.tree_of(self.payload), self.tree_of(self.dest))
            # Same bytes means the same digest, so the next run has nothing to do --
            # a wider copy that quietly dropped a file would redeploy here instead.
            self.assertVendored(
                self.vendor(manifest, "local.nanocobs@r3"),
                "local.nanocobs@r3",
                "up_to_date",
                "current",
            )

    # -- --dry-run -----------------------------------------------------------

    def test_dry_run_reports_a_repair_without_making_it(self):
        manifest = self.one()
        (self.dest / "src" / "lib.c").write_text("mine\n", encoding="utf-8")
        before = self.tree_of(self.dest)

        run = self.vendor(manifest, "local.nanocobs@r3", "--dry-run")
        self.assertVendored(
            run, "local.nanocobs@r3", "redeployed", "mismatch", dry_run=True
        )
        self.assertIn("would re-vendor", run.stderr)
        self.assertEqual(before, self.tree_of(self.dest))

    def test_dry_run_does_not_create_an_absent_destination(self):
        manifest = self.one()
        rmtree_retry(self.dest)

        run = self.vendor(manifest, "local.nanocobs@r3", "--dry-run")
        self.assertVendored(run, "local.nanocobs@r3", "copied", "absent", dry_run=True)
        self.assertIn("would vendor", run.stderr)
        self.assertFalse(self.dest.exists(), "a dry run created the destination")

    def test_dry_run_counts_the_files_it_would_write(self):
        manifest = self.one()
        rmtree_retry(self.dest)

        run = self.vendor(manifest, "local.nanocobs@r3", "--dry-run")
        result = self.results(run)["local.nanocobs@r3"]
        self.assertEqual(len(self.tree_of(self.payload)), result["files"])
        self.assertGreater(result["bytes"], 0)

    def test_dry_run_leaves_the_work_for_the_next_run(self):
        manifest = self.one()
        (self.dest / "src" / "lib.c").write_text("mine\n", encoding="utf-8")

        self.vendor(manifest, "local.nanocobs@r3", "--dry-run")
        run = self.vendor(manifest, "local.nanocobs@r3")
        self.assertVendored(run, "local.nanocobs@r3", "redeployed", "mismatch")
        self.assertEqual(self.tree_of(self.payload), self.tree_of(self.dest))

    def test_dry_run_with_force_reports_the_repair_the_exemption_blocks(self):
        manifest = self.one(vendor="{ auto_sync = false }")
        (self.dest / "src" / "lib.c").write_text("mine\n", encoding="utf-8")
        before = self.tree_of(self.dest)

        run = self.vendor(manifest, "local.nanocobs@r3", "--force", "--dry-run")
        self.assertVendored(
            run, "local.nanocobs@r3", "redeployed", "mismatch", dry_run=True
        )
        self.assertEqual(before, self.tree_of(self.dest))

    def test_dry_run_without_force_still_reports_the_exemption(self):
        manifest = self.one(vendor="{ auto_sync = false }")
        (self.dest / "src" / "lib.c").write_text("mine\n", encoding="utf-8")

        run = self.vendor(manifest, "local.nanocobs@r3", "--dry-run")
        self.assertVendored(run, "local.nanocobs@r3", "kept", "mismatch", dry_run=True)
        self.assertIn("no longer matches", run.stderr)

    def test_dry_run_over_all_of_them_writes_nothing(self):
        manifest = self.two()
        for name in ("one", "two"):
            (self.project / "vendor" / name / "LICENSE").write_text("x\n")

        run = self.vendor(manifest, "--all", "--dry-run")
        for identity in ("local.one@r1", "local.two@r1"):
            self.assertVendored(run, identity, "redeployed", "mismatch", dry_run=True)
        for name in ("one", "two"):
            self.assertEqual(
                "x\n", (self.project / "vendor" / name / "LICENSE").read_text()
            )


class TestVendorFromBundle(VendorTestCase):
    """A spec pulled out of a bundle vendors exactly as one named by `source` does.

    A spec whose whole job is to put a source tree in the work tree is the kind worth
    sharing from a bundle, so the two entry shapes have to agree on `vendor`.
    """

    def bundle(self, identity: str, spec_identity: str) -> Path:
        root = self.make_temp_dir("bundle")
        (root / "specs").mkdir()
        (root / "envy-bundle.lua").write_text(
            f'BUNDLE = "{identity}"\n'
            f'SPECS = {{ ["{spec_identity}"] = "specs/pkg.lua" }}\n',
            encoding="utf-8",
        )
        (root / "specs" / "pkg.lua").write_text(
            SPEC.format(
                identity=spec_identity,
                seed=self.lua_path(self.seed),
                payload=self.lua_path(self.payload),
                vendor_list="",
                install_extra="",
            ),
            encoding="utf-8",
        )
        return root

    def bundled_manifest(self, vendor: str) -> Path:
        root = self.bundle("test.specs@r1", "local.nanocobs@r3")
        path = self.project / "envy.lua"
        path.write_text(
            test_config.make_manifest(
                'VENDOR_ROOT = "vendor"\n'
                "BUNDLES = {\n"
                f'  tools = {{ identity = "test.specs@r1", '
                f'source = "{self.lua_path(root)}" }},\n'
                "}\n"
                "PACKAGES = {\n"
                f'  {{ spec = "local.nanocobs@r3", bundle = "tools", '
                f"vendor = {vendor} }},\n"
                "}\n"
            ),
            encoding="utf-8",
        )
        return path

    def test_vendor_true_on_a_bundled_spec_derives_a_name(self):
        run = self.install(self.bundled_manifest("true"))
        self.assertVendored(run, "local.nanocobs@r3", "copied", "absent")
        self.assertEqual(
            self.tree_of(self.payload),
            self.tree_of(self.project / "vendor" / "nanocobs"),
        )

    def test_vendor_override_on_a_bundled_spec_lands_where_it_says(self):
        run = self.install(self.bundled_manifest('"deps/cobs"'))
        self.assertVendored(run, "local.nanocobs@r3", "copied", "absent")
        self.assertEqual(
            self.tree_of(self.payload), self.tree_of(self.project / "deps" / "cobs")
        )

    def test_vendor_table_on_a_bundled_spec_reads_auto_sync(self):
        manifest = self.bundled_manifest("{ auto_sync = false }")
        dest = self.project / "vendor" / "nanocobs"
        self.assertEqual(0, self.install(manifest).returncode)

        (dest / "LICENSE").write_text("mine now\n", encoding="utf-8")
        run = self.install(manifest)

        self.assertVendored(run, "local.nanocobs@r3", "kept", "mismatch")
        self.assertEqual("mine now\n", (dest / "LICENSE").read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
