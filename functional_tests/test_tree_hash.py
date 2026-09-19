"""Exhaustive coverage for the subtree hasher behind vendoring.

Everything here needs a tree it can mutate -- flip a byte, toggle an exec bit, delete a
directory mid-walk -- which is exactly what unit tests may not do. So the mutable half of
the hasher's contract lives here, driven through `envy hash --tree`, while the read-only
fixture half stays in src/tree_hash_tests.cpp.

The sanitizer shards run this suite, so the walker's threading is race-checked on every
CI run by the same tests that check its answers.
"""

from __future__ import annotations

import hashlib
import json
import os
import random
import stat
import sys
import unittest
from pathlib import Path

from .env import EnvyTestCase


POSIX_ONLY = unittest.skipIf(sys.platform == "win32", "POSIX file modes and symlinks")

# A decorator's argument is evaluated when the class body runs, so an os.geteuid() call
# inside one would raise on Windows before any skip could apply -- and take the whole
# module's import down with it.
RUNNING_AS_ROOT = sys.platform != "win32" and os.geteuid() == 0


class TreeHashMixin(EnvyTestCase):
    """Building trees and asking envy what they hash to."""

    def tree(self, label: str = "tree") -> Path:
        return self.make_temp_dir(label)

    def write(self, root: Path, rel: str, data: bytes = b"payload\n") -> Path:
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    def digest(self, root: Path, *only: str, threads: int | None = None) -> str:
        args = ["hash", "--tree", "--json", str(root)]
        for pattern in only:
            args += ["--only", pattern]
        if threads is not None:
            args += ["--threads", str(threads)]
        run = self.run_envy(*args)
        self.assertEqual(0, run.returncode, run.stderr)
        records = json.loads(run.stdout)
        self.assertEqual(1, len(records), run.stdout)
        return records[0]["digest"]

    def hash_fails(self, root: Path, *only: str) -> str:
        args = ["hash", "--tree", str(root)]
        for pattern in only:
            args += ["--only", pattern]
        run = self.run_envy(*args)
        self.assertNotEqual(0, run.returncode, f"expected failure:\n{run.stdout}")
        return run.stderr

    def sample_tree(self) -> Path:
        root = self.tree()
        self.write(root, "README", b"top\n")
        self.write(root, "include/lib.h", b"#pragma once\n")
        self.write(root, "include/detail/impl.h", b"// detail\n")
        self.write(root, "src/lib.c", b"int main(void){return 0;}\n")
        (root / "docs").mkdir()
        return root


class TestTreeHashSensitivity(TreeHashMixin):
    """Changes that must move the digest."""

    def assertDigestChanges(self, root: Path, mutate):
        before = self.digest(root)
        mutate()
        after = self.digest(root)
        self.assertNotEqual(before, after)
        return before, after

    def test_flipped_byte(self):
        root = self.sample_tree()
        self.assertDigestChanges(
            root, lambda: (root / "src/lib.c").write_bytes(b"int main(void){return 1;}\n")
        )

    def test_truncated_by_one_byte(self):
        root = self.sample_tree()
        target = root / "src/lib.c"
        data = target.read_bytes()
        self.assertDigestChanges(root, lambda: target.write_bytes(data[:-1]))

    def test_extended_by_one_zero_byte(self):
        root = self.sample_tree()
        target = root / "src/lib.c"
        data = target.read_bytes()
        self.assertDigestChanges(root, lambda: target.write_bytes(data + b"\0"))

    def test_swapped_contents_between_two_paths(self):
        # Same multiset of file contents, different pairing with paths. A digest that
        # folded contents without their paths would call these trees equal.
        root = self.tree()
        self.write(root, "a.txt", b"alpha\n")
        self.write(root, "b.txt", b"beta\n")

        def swap():
            (root / "a.txt").write_bytes(b"beta\n")
            (root / "b.txt").write_bytes(b"alpha\n")

        self.assertDigestChanges(root, swap)

    def test_renamed_file(self):
        root = self.sample_tree()
        self.assertDigestChanges(
            root, lambda: (root / "README").rename(root / "READYOU")
        )

    def test_file_moved_between_sibling_directories(self):
        root = self.tree()
        self.write(root, "a/x.txt")
        (root / "b").mkdir()
        self.assertDigestChanges(
            root, lambda: (root / "a/x.txt").rename(root / "b/x.txt")
        )

    def test_added_empty_file(self):
        root = self.sample_tree()
        self.assertDigestChanges(root, lambda: self.write(root, "new", b""))

    def test_added_empty_directory(self):
        root = self.sample_tree()
        self.assertDigestChanges(root, lambda: (root / "brand-new").mkdir())

    def test_removed_empty_directory(self):
        # Directories fold into the digest precisely so this is visible; a file-only
        # hash would report the tree unchanged.
        root = self.sample_tree()
        self.assertDigestChanges(root, lambda: (root / "docs").rmdir())

    @POSIX_ONLY
    def test_toggled_exec_bit(self):
        root = self.sample_tree()
        target = root / "src/lib.c"

        def chmod():
            target.chmod(target.stat().st_mode | stat.S_IXUSR)

        self.assertDigestChanges(root, chmod)

    @POSIX_ONLY
    def test_changed_symlink_target(self):
        root = self.sample_tree()
        (root / "link").symlink_to("README")

        def repoint():
            (root / "link").unlink()
            (root / "link").symlink_to("src/lib.c")

        self.assertDigestChanges(root, repoint)

    @POSIX_ONLY
    def test_file_replaced_by_symlink_with_identical_bytes(self):
        # The bytes readable at the path are the same either way; only the kind differs.
        root = self.tree()
        self.write(root, "real.txt", b"shared\n")
        self.write(root, "copy.txt", b"shared\n")

        def relink():
            (root / "copy.txt").unlink()
            (root / "copy.txt").symlink_to("real.txt")

        self.assertDigestChanges(root, relink)

    @POSIX_ONLY
    def test_symlink_is_not_followed(self):
        # A symlink to a directory must hash as a link, not as a second copy of the
        # subtree -- otherwise a self-referential link would never terminate.
        root = self.tree()
        self.write(root, "real/deep/file.txt", b"x\n")
        (root / "loop").symlink_to(root / "real")
        self.assertEqual(self.digest(root), self.digest(root, threads=4))

        (root / "self").symlink_to(root)
        self.digest(root)  # must terminate rather than recurse forever


class TestTreeHashStability(TreeHashMixin):
    """Changes that must not move the digest."""

    def test_file_mtime_touch(self):
        root = self.sample_tree()
        before = self.digest(root)
        os.utime(root / "src/lib.c", (1, 1))
        self.assertEqual(before, self.digest(root))

    def test_directory_mtime_touch(self):
        root = self.sample_tree()
        before = self.digest(root)
        os.utime(root / "include", (1, 1))
        self.assertEqual(before, self.digest(root))

    def test_same_content_at_a_different_path(self):
        a, b = self.sample_tree(), self.sample_tree()
        self.assertNotEqual(a, b)
        self.assertEqual(self.digest(a), self.digest(b))

    def test_creation_order_does_not_matter(self):
        names = ["m/n.txt", "a/b.txt", "z.txt", "a/c.txt"]
        first, second = self.tree("first"), self.tree("second")
        for rel in names:
            self.write(first, rel, rel.encode())
        for rel in reversed(names):
            self.write(second, rel, rel.encode())
        self.assertEqual(self.digest(first), self.digest(second))

    def test_thread_count_does_not_change_the_answer(self):
        root = self.tree()
        for i in range(60):
            self.write(root, f"d{i % 7}/s{i % 3}/f{i}.bin", bytes([i]) * (i * 97 + 1))
        want = self.digest(root, threads=1)
        for threads in (2, 3, 4, 8, 17, 0):
            self.assertEqual(want, self.digest(root, threads=threads), f"threads={threads}")


class TestTreeHashStructure(TreeHashMixin):
    """Shapes that are easy to get wrong."""

    def test_empty_root_is_stable_and_distinct(self):
        empty_a, empty_b = self.tree("empty-a"), self.tree("empty-b")
        with_file = self.tree("with-file")
        self.write(with_file, "only", b"")

        self.assertEqual(self.digest(empty_a), self.digest(empty_b))
        self.assertNotEqual(self.digest(empty_a), self.digest(with_file))

    def test_single_file_at_root(self):
        root = self.tree()
        self.write(root, "solo.txt", b"just me\n")
        self.assertEqual(self.digest(root), self.digest(root, threads=8))

    def test_deep_nesting_does_not_overflow_the_stack(self):
        # The walk is a queue, not recursion; 200 levels proves it.
        root = self.tree()
        self.write(root, "/".join(f"d{i}" for i in range(200)) + "/leaf.txt", b"deep\n")
        self.assertEqual(self.digest(root, threads=1), self.digest(root, threads=8))

    def test_ten_thousand_entries_in_one_directory(self):
        root = self.tree()
        wide = root / "wide"
        wide.mkdir()
        for i in range(10_000):
            (wide / f"f{i:05d}").write_bytes(b"x")
        self.assertEqual(self.digest(root, threads=1), self.digest(root, threads=8))

    test_ten_thousand_entries_in_one_directory.envy_watchdog_timeout = 120

    def test_non_ascii_and_spaced_filenames(self):
        root = self.tree()
        self.write(root, "\u00fcn\u00efcode.txt", b"utf-8 name\n")
        self.write(root, "a space/and another.txt", b"spaces\n")
        self.write(root, "\u65e5\u672c\u8a9e/\u30d5\u30a1\u30a4\u30eb.bin", b"\xff\xfe")
        self.assertEqual(self.digest(root, threads=1), self.digest(root, threads=4))

    def test_long_paths(self):
        # Past MAX_PATH on Windows, where traversal has to be \\?\-prefixed to work.
        root = self.tree()
        segment = "p" * 60
        self.write(root, "/".join([segment] * 6) + "/leaf.txt", b"long\n")
        self.assertEqual(self.digest(root, threads=1), self.digest(root, threads=4))


class TestTreeHashSelectors(TreeHashMixin):
    """The VENDOR selector language, end to end."""

    def selector_tree(self) -> Path:
        root = self.tree()
        self.write(root, "include/lib.h", b"h\n")
        self.write(root, "include/internal/secret.h", b"secret\n")
        self.write(root, "src/lib.c", b"c\n")
        self.write(root, "src/lib_test.c", b"t\n")
        self.write(root, "LICENSE", b"MIT\n")
        return root

    def test_include_only_narrows_the_digest(self):
        root = self.selector_tree()
        self.assertNotEqual(self.digest(root), self.digest(root, "include/**"))

    def test_exclude_removes_what_it_names(self):
        root = self.selector_tree()
        everything = self.digest(root)
        without_tests = self.digest(root, "!src/lib_test.c")
        self.assertNotEqual(everything, without_tests)

        # Deleting the excluded file must leave the filtered digest untouched: the
        # exclusion and the deletion have to mean the same thing.
        (root / "src/lib_test.c").unlink()
        self.assertEqual(without_tests, self.digest(root, "!src/lib_test.c"))

    def test_exclude_beats_include(self):
        root = self.selector_tree()
        both = self.digest(root, "include/**", "!include/internal/**")
        (root / "include/internal/secret.h").unlink()
        (root / "include/internal").rmdir()
        self.assertEqual(both, self.digest(root, "include/**"))

    def test_selector_reaches_under_an_unselected_directory(self):
        root = self.selector_tree()
        self.assertNotEqual(self.digest(root), self.digest(root, "**/secret.h"))

    def test_selector_matching_nothing_is_not_an_error(self):
        root = self.selector_tree()
        empty = self.tree("empty")
        self.assertEqual(self.digest(empty), self.digest(root, "no/such/thing"))

    def test_malformed_selector_is_rejected(self):
        root = self.selector_tree()
        for bad in ("[unterminated", "../escape", "!", "/abs"):
            stderr = self.hash_fails(root, bad)
            self.assertIn("hash --only", stderr, f"pattern {bad!r}: {stderr}")


class TestTreeHashStreams(TreeHashMixin):
    """stdout carries the result; everything else stays out of it."""

    def test_stats_go_to_stderr_and_stdout_stays_the_digest(self):
        root = self.sample_tree()
        run = self.run_envy("hash", "--tree", "--stats", str(root))
        self.assertEqual(0, run.returncode, run.stderr)

        # stdout is one "<64 hex>  <path>" line and nothing else, so it still pipes into
        # a checksum file or a diff with --stats on.
        lines = run.stdout.splitlines()
        self.assertEqual(1, len(lines), run.stdout)
        digest, _, path = lines[0].partition("  ")
        self.assertEqual(64, len(digest))
        self.assertEqual(digest, self.digest(root))
        self.assertIn(str(root), path)

        for field in ("threads", "scan", "read", "hash", "wait", "files/worker"):
            self.assertIn(field, run.stderr)

    def test_without_stats_stderr_carries_no_report(self):
        root = self.sample_tree()
        run = self.run_envy("hash", "--tree", str(root))
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertEqual(1, len(run.stdout.splitlines()))
        self.assertNotIn("files/worker", run.stderr)

    def test_json_keeps_the_breakdown_in_the_payload(self):
        # A JSON consumer parses stdout, so the numbers belong inside the object rather
        # than on stderr where the human form goes.
        root = self.sample_tree()
        run = self.run_envy("hash", "--tree", "--json", "--stats", str(root))
        self.assertEqual(0, run.returncode, run.stderr)

        record = json.loads(run.stdout)[0]
        for key in ("digest", "dirs", "scan_ns", "read_ns", "hash_ns", "wait_ns",
                    "fold_ns", "files_per_worker"):
            self.assertIn(key, record)
        self.assertNotIn("files/worker", run.stderr)


class TestTreeHashErrors(TreeHashMixin):
    """Failure is the right answer more often than a partial digest is."""

    def test_missing_root(self):
        stderr = self.hash_fails(self.tree() / "nope")
        self.assertIn("directory", stderr)

    def test_root_is_a_file(self):
        root = self.tree()
        target = self.write(root, "plain.txt")
        self.assertIn("directory", self.hash_fails(target))

    @POSIX_ONLY
    @unittest.skipIf(RUNNING_AS_ROOT, "root reads unreadable directories anyway")
    def test_unreadable_directory_fails_rather_than_being_skipped(self):
        # A digest that silently omits what it could not read would report "unchanged"
        # for a tree it never looked at.
        root = self.tree()
        self.write(root, "open/file.txt")
        locked = root / "locked"
        locked.mkdir()
        (locked / "hidden.txt").write_bytes(b"x\n")
        locked.chmod(0o000)
        self.addCleanup(locked.chmod, 0o755)

        stderr = self.hash_fails(root)
        self.assertIn("locked", stderr)


class TestTreeHashEquivalence(TreeHashMixin):
    """Random trees: the digest's equivalence relation, checked independently.

    Python has no BLAKE3 in its standard library, so this cannot recompute the digest --
    src/tree_hash_tests.cpp owns that cross-check against a std::filesystem oracle. What
    it can do is decide, on its own terms, whether two trees are structurally identical,
    and demand that digest equality agrees with it on every pair. A hasher that ignored a
    field (paths, kinds, exec bits, empty directories) would collapse a pair this sees as
    distinct; one that folded in something incidental (order, inode, mtime) would split a
    pair this sees as identical.
    """

    def fingerprint(self, root: Path) -> str:
        """A structural fingerprint built with hashlib, sharing no code with envy."""
        entries = []
        for path in sorted(root.rglob("*"), key=lambda p: p.relative_to(root).as_posix()):
            rel = path.relative_to(root).as_posix()
            if path.is_symlink():
                entries.append(f"l\0{rel}\0{os.readlink(path)}")
            elif path.is_dir():
                entries.append(f"d\0{rel}")
            else:
                executable = sys.platform != "win32" and bool(
                    path.stat().st_mode & stat.S_IXUSR
                )
                content = hashlib.sha256(path.read_bytes()).hexdigest()
                entries.append(f"f\0{rel}\0{int(executable)}\0{content}")
        return hashlib.sha256("\n".join(entries).encode()).hexdigest()

    def test_digest_equality_matches_structural_equality(self):
        seeds = (1, 7, 31, 1009)
        trees = []
        for seed in seeds:
            root = self.tree(f"rand-{seed}")
            _populate_random(root, random.Random(seed))
            trees.append(root)

        # Every tree built twice from the same seed, so the corpus holds known-equal
        # pairs as well as known-different ones.
        for seed in seeds:
            root = self.tree(f"twin-{seed}")
            _populate_random(root, random.Random(seed))
            trees.append(root)

        digests = [self.digest(root) for root in trees]
        prints = [self.fingerprint(root) for root in trees]

        for i in range(len(trees)):
            for j in range(i + 1, len(trees)):
                self.assertEqual(
                    prints[i] == prints[j],
                    digests[i] == digests[j],
                    f"{trees[i]} vs {trees[j]}: structural equality and digest "
                    f"equality disagree",
                )

    test_digest_equality_matches_structural_equality.envy_watchdog_timeout = 120


def _populate_random(root: Path, rng: random.Random) -> None:
    """A tree of random shape: nested dirs, empty dirs, empty files, binary payloads."""
    dirs = [root]
    for _ in range(rng.randint(5, 20)):
        parent = rng.choice(dirs)
        child = parent / f"d{rng.randrange(1000)}"
        child.mkdir(exist_ok=True)
        dirs.append(child)
    for _ in range(rng.randint(10, 60)):
        parent = rng.choice(dirs)
        size = rng.choice([0, 1, 17, 4096, 1 << 18])
        (parent / f"f{rng.randrange(10000)}.bin").write_bytes(
            bytes(rng.randrange(256) for _ in range(min(size, 4096)))
            * max(1, size // 4096)
            if size
            else b""
        )


if __name__ == "__main__":
    unittest.main()
