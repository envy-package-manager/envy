"""`envy use` retargets a manifest's pinned envy version, pin and all.

Two things above all: a failure leaves the manifest byte-for-byte as it was, and the pin it
writes is the hash of what the mirror actually serves.
"""

from __future__ import annotations

import hashlib
import unittest
from pathlib import Path

from .env import EnvyTestCase

_STALE_PIN = "0" * 64
_SUMS_BODY = b"deadbeef  envy-darwin-arm64.tar.gz\n"
_FRESH_PIN = hashlib.sha256(_SUMS_BODY).hexdigest()

_UNREACHABLE = "http://127.0.0.1:1/never-contacted"  # --force is only proven if a fetch can't


class UseTests(EnvyTestCase):
    def setUp(self):
        super().setUp()
        self.mirror = self.make_temp_dir("mirror")
        self.mirror_url = self.serve_directory(self.mirror)
        self.publish("0.1.6")

    def publish(self, version: str) -> None:
        """Put a SHA256SUMS at v<version>/ on the stub mirror, as a release does."""
        release = self.mirror / f"v{version}"
        release.mkdir(parents=True, exist_ok=True)
        (release / "SHA256SUMS").write_bytes(_SUMS_BODY)

    def manifest(self, header: str, directory: Path | None = None) -> Path:
        path = (directory or self.work) / "envy.lua"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(
            f"-- envy.lua - Project manifest\n{header}PACKAGES = {{\n}}\n",
            encoding="utf-8",
        )
        return path

    def use(self, manifest: Path, *extra, **kwargs):
        return self.run_envy("use", "0.1.6", "--manifest", manifest, *extra, **kwargs)

    # -- the happy paths ----------------------------------------------------

    def test_retargets_an_unpinned_manifest(self):
        path = self.manifest('-- @envy version "0.1.5"\n')

        run = self.use(path, "--mirror", self.mirror_url)

        self.assertEqual(0, run.returncode, run.stderr)
        self.assertEqual(
            '-- envy.lua - Project manifest\n-- @envy version "0.1.6"\nPACKAGES = {\n}\n',
            path.read_text(encoding="utf-8"),
        )
        self.assertIn("0.1.5", run.stderr)
        self.assertIn("0.1.6", run.stderr)

    def test_repins_an_already_pinned_manifest(self):
        path = self.manifest(
            f'-- @envy version "0.1.5"\n-- @envy sha256sums "{_STALE_PIN}"\n'
        )

        run = self.use(path, "--mirror", self.mirror_url)

        self.assertEqual(0, run.returncode, run.stderr)
        content = path.read_text(encoding="utf-8")
        self.assertIn(f'-- @envy sha256sums "{_FRESH_PIN}"', content)
        self.assertNotIn(_STALE_PIN, content)
        self.assertIn('-- @envy version "0.1.6"', content)

    def test_reads_the_mirror_out_of_the_manifest(self):
        # No --mirror: the pin must come from wherever this project bootstraps from, or it
        # would attest against bytes it never downloads.
        path = self.manifest(
            f'-- @envy version "0.1.5"\n'
            f'-- @envy mirror "{self.mirror_url}"\n'
            f'-- @envy sha256sums "{_STALE_PIN}"\n'
        )

        run = self.use(path)

        self.assertEqual(0, run.returncode, run.stderr)
        self.assertIn(_FRESH_PIN, path.read_text(encoding="utf-8"))

    def test_pin_sums_adds_a_pin_below_the_version(self):
        path = self.manifest('-- @envy version "0.1.5"\n-- @envy bin "tools"\n')

        run = self.use(path, "--mirror", self.mirror_url, "--pin-sums")

        self.assertEqual(0, run.returncode, run.stderr)
        self.assertEqual(
            "-- envy.lua - Project manifest\n"
            '-- @envy version "0.1.6"\n'
            f'-- @envy sha256sums "{_FRESH_PIN}"\n'
            '-- @envy bin "tools"\n'
            "PACKAGES = {\n}\n",
            path.read_text(encoding="utf-8"),
        )

    def test_no_pin_sums_drops_the_pin(self):
        path = self.manifest(
            f'-- @envy version "0.1.5"\n'
            f'-- @envy sha256sums "{_STALE_PIN}"\n'
            f'-- @envy bin "tools"\n'
        )

        run = self.use(path, "--mirror", self.mirror_url, "--no-pin-sums")

        self.assertEqual(0, run.returncode, run.stderr)
        self.assertEqual(
            "-- envy.lua - Project manifest\n"
            '-- @envy version "0.1.6"\n'
            '-- @envy bin "tools"\n'
            "PACKAGES = {\n}\n",
            path.read_text(encoding="utf-8"),
        )

    def test_reports_no_change_when_already_current(self):
        path = self.manifest(
            f'-- @envy version "0.1.6"\n-- @envy sha256sums "{_FRESH_PIN}"\n'
        )
        before = path.read_bytes()

        run = self.use(path, "--mirror", self.mirror_url)

        self.assertEqual(0, run.returncode, run.stderr)
        self.assertEqual(before, path.read_bytes())
        self.assertIn("already", run.stderr)

    def test_subproject_edits_the_nearest_manifest(self):
        root = self.manifest('-- @envy version "0.1.5"\n-- @envy root "true"\n')
        nested = self.manifest(
            '-- @envy version "0.1.5"\n-- @envy root "false"\n',
            directory=self.work / "sub",
        )
        root_before = root.read_bytes()

        run = self.run_envy(
            "use",
            "0.1.6",
            "--subproject",
            "--mirror",
            self.mirror_url,
            cwd=nested.parent,
        )

        self.assertEqual(0, run.returncode, run.stderr)
        self.assertIn('"0.1.6"', nested.read_text(encoding="utf-8"))
        self.assertEqual(root_before, root.read_bytes())

    # -- the failure paths, which must not touch the file -------------------

    def test_unpublished_version_fails_and_leaves_the_manifest_alone(self):
        # Why the fetch happens even with no pin to write: an unpublished version has to fail
        # here, not on the next person's bootstrap.
        path = self.manifest('-- @envy version "0.1.5"\n')
        before = path.read_bytes()

        run = self.run_envy(
            "use", "9.9.9", "--manifest", path, "--mirror", self.mirror_url
        )

        self.assertNotEqual(0, run.returncode)
        self.assertEqual(before, path.read_bytes())
        self.assertIn("9.9.9", run.stderr)

    def test_missing_version_directive_is_fatal(self):
        path = self.manifest('-- @envy bin "tools"\n')
        before = path.read_bytes()

        run = self.use(path, "--mirror", self.mirror_url)

        self.assertNotEqual(0, run.returncode)
        self.assertEqual(before, path.read_bytes())
        self.assertIn("floats to the latest release", run.stderr)

    def test_force_skips_the_fetch(self):
        path = self.manifest('-- @envy version "0.1.5"\n')

        run = self.use(path, "--mirror", _UNREACHABLE, "--force")

        self.assertEqual(0, run.returncode, run.stderr)
        self.assertIn('"0.1.6"', path.read_text(encoding="utf-8"))

    def test_force_refuses_to_leave_a_pin_stale(self):
        # A pin comes only from the sums file, so --force here would have to invent a value or
        # silently unpin. It errors instead.
        path = self.manifest(
            f'-- @envy version "0.1.5"\n-- @envy sha256sums "{_STALE_PIN}"\n'
        )
        before = path.read_bytes()

        run = self.use(path, "--mirror", _UNREACHABLE, "--force")

        self.assertNotEqual(0, run.returncode)
        self.assertEqual(before, path.read_bytes())
        self.assertIn("--no-pin-sums", run.stderr)

    def test_invalid_version_is_rejected_before_the_network(self):
        path = self.manifest('-- @envy version "0.1.5"\n')
        before = path.read_bytes()

        run = self.run_envy(
            "use", "../escape", "--manifest", path, "--mirror", self.mirror_url
        )

        self.assertNotEqual(0, run.returncode)
        self.assertEqual(before, path.read_bytes())


if __name__ == "__main__":
    unittest.main()
