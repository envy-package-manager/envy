"""Functional tests for vendoring: cached payloads copied into the project tree.

Assertions read the trace rather than stdout -- `vendor_result` carries the action and
the reason envy chose, so a test can tell "nothing was there" from "somebody edited it"
without matching prose.
"""

from __future__ import annotations

import os
import sys
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

    def assertVendored(self, run, identity: str, action: str, reason: str):
        self.assertEqual(0, run.returncode, run.stderr)
        results = self.results(run)
        self.assertIn(identity, results, f"no vendor_result for {identity}")
        self.assertEqual(action, results[identity]["action"], results[identity])
        self.assertEqual(reason, results[identity]["reason"], results[identity])

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
        import shutil

        shutil.rmtree(dest)

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
            self.entry("local.nanocobs@r4", self.spec("local.nanocobs@r4"), vendor='"vendor/nanocobs"')
        )
        run = self.install(manifest)
        self.assertVendored(run, "local.nanocobs@r4", "redeployed", "mismatch")
        self.assertEqual(self.tree_of(self.payload), self.tree_of(dest))

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

        # Everything envy could have remembered, gone: a different cache root, and a
        # destination this envy never wrote.
        import shutil

        shutil.rmtree(self.cache_root)
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
        import shutil

        dest = self.install_once()
        before = {p: p.stat().st_mtime_ns for p in sorted(dest.rglob("*")) if p.is_file()}

        shutil.rmtree(self.cache_root)
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
        import shutil

        dest = self.install_once()
        for child in dest.iterdir():
            shutil.rmtree(child) if child.is_dir() else child.unlink()
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
        self.assertEqual(self.tree_of(self.payload), self.tree_of(self.project / "deps" / "cobs"))
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


if __name__ == "__main__":
    unittest.main()
