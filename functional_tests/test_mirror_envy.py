"""Functional tests for `envy mirror-envy`.

Staging runs against a file:// source mirror so no network is involved. The S3 upload leg
runs against a local S3 stub reached via AWS_ENDPOINT_URL_S3, so the upload path gets real
coverage without touching AWS.
"""

from __future__ import annotations

import gzip
import hashlib
import io
import os
import re
import shutil
import sys
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path

from . import test_config
from .env import EnvyTestCase
from .s3_stub import S3Stub, hermetic_env, s3_env

# Must stay byte-identical to kEnvyReleaseTargets in src/envy_release.h.
RELEASE_ASSETS = [
    "envy-darwin-arm64.tar.gz",
    "envy-darwin-x86_64.tar.gz",
    "envy-linux-arm64.tar.gz",
    "envy-linux-x86_64.tar.gz",
    "envy-windows-arm64.zip",
    "envy-windows-x86_64.zip",
]


def _archive_for(asset: str) -> bytes:
    """Build a tiny archive shaped like a real release asset."""
    payload = f"fake-envy-binary-for-{asset}".encode()
    if asset.endswith(".zip"):
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as zf:
            zf.writestr("envy.exe", payload)
        return buf.getvalue()
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz") as tf:
        info = tarfile.TarInfo(name="envy")
        info.size = len(payload)
        info.mode = 0o755
        tf.addfile(info, io.BytesIO(payload))
    return buf.getvalue()


class MirrorEnvyFunctionalTest(EnvyTestCase):
    # Downloads six archives and (in the S3 cases) uploads seven objects.
    envy_watchdog_timeout = 60

    @classmethod
    def setUpClass(cls) -> None:
        cls._envy = test_config.get_envy_executable()

    def setUp(self) -> None:
        self._temp = self.make_temp_dir("_temp")
        self._source = self._temp / "upstream"
        (self._source / "v1.2.3").mkdir(parents=True)
        for asset in RELEASE_ASSETS:
            (self._source / "v1.2.3" / asset).write_bytes(_archive_for(asset))
        self._write_upstream_sums()

    def _write_upstream_sums(self, corrupt: str | None = None) -> str:
        """Publish upstream's SHA256SUMS; return the hash a manifest would pin.

        corrupt names an asset whose listed digest is wrong, modelling an upstream (or a
        man-in-the-middle) serving an archive that does not match its own checksum manifest.

        Digests come from the bytes on disk, never from a second `_archive_for` call: the
        tar.gz header carries an mtime, so regenerating an asset a second later yields
        different bytes and the sums file would disagree with the staged archive.
        """
        lines = []
        for asset in RELEASE_ASSETS:
            digest = (
                "f" * 64
                if asset == corrupt
                else hashlib.sha256(
                    (self._source / "v1.2.3" / asset).read_bytes()
                ).hexdigest()
            )
            lines.append(f"{digest}  {asset}\n")
        body = "".join(lines).encode()
        (self._source / "v1.2.3" / "SHA256SUMS").write_bytes(body)
        return hashlib.sha256(body).hexdigest()

    def tearDown(self) -> None:
        if self._temp.exists():
            if os.environ.get("ENVY_TEST_KEEP_TEMP"):
                sys.stderr.write(f"\nENVY_TEST_KEEP_TEMP: kept {self._temp}\n")
                return
            shutil.rmtree(self._temp, ignore_errors=True)

    def _from_uri(self) -> str:
        return self._source.as_uri()

    def _run(self, args: list[str], env_extra: dict[str, str] | None = None):
        env = hermetic_env({"ENVY_CACHE_ROOT": str(self._temp / "cache")})
        if env_extra:
            env.update(env_extra)
        return test_config.run(
            [str(self._envy), *args],
            capture_output=True,
            text=True,
            env=env,
            cwd=self._temp,
            timeout=60,
        )

    # --- staging ------------------------------------------------------------------

    def test_stages_all_six_assets_and_latest(self) -> None:
        dest = self._temp / "staged"
        result = self._run(
            ["mirror-envy", "1.2.3", str(dest), f"--from={self._from_uri()}"]
        )

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        for asset in RELEASE_ASSETS:
            staged = dest / "v1.2.3" / asset
            self.assertTrue(staged.exists(), f"missing {asset}")
            # Byte-identical: mirroring copies release bytes, it never repacks.
            self.assertEqual(
                (self._source / "v1.2.3" / asset).read_bytes(), staged.read_bytes()
            )
        self.assertEqual("1.2.3", (dest / "latest").read_text())

    def test_sums_file_is_mirrored_byte_for_byte(self) -> None:
        """Verbatim, never regenerated: an `@envy sha256sums` pin must stay valid here.

        Regenerating the file -- even with identical digests -- would change its own hash on
        any ordering or formatting difference, making a project's pin mirror-specific.
        """
        dest = self._temp / "staged"
        result = self._run(
            ["mirror-envy", "1.2.3", str(dest), f"--from={self._from_uri()}"]
        )

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        upstream = (self._source / "v1.2.3" / "SHA256SUMS").read_bytes()
        self.assertEqual(upstream, (dest / "v1.2.3" / "SHA256SUMS").read_bytes())

    def test_reports_the_pin_for_the_mirrored_sums(self) -> None:
        dest = self._temp / "staged"
        expected = hashlib.sha256(
            (self._source / "v1.2.3" / "SHA256SUMS").read_bytes()
        ).hexdigest()

        result = self._run(
            ["mirror-envy", "1.2.3", str(dest), f"--from={self._from_uri()}"]
        )

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn(f'-- @envy sha256sums "{expected}"', result.stderr)

    def test_refuses_to_republish_an_archive_that_fails_its_own_sums(self) -> None:
        """A mirror that passes on bad bytes turns one bad fetch into many bad consumers.

        Worse, a project pinning this SHA256SUMS would then attest the corrupted archive as
        authentic, so the run must abort before anything is staged as usable.
        """
        self._write_upstream_sums(corrupt="envy-linux-arm64.tar.gz")
        dest = self._temp / "staged"

        result = self._run(
            ["mirror-envy", "1.2.3", str(dest), f"--from={self._from_uri()}"]
        )

        self.assertNotEqual(0, result.returncode)
        self.assertIn("attestation", result.stderr.lower())
        self.assertIn("envy-linux-arm64.tar.gz", result.stderr)
        # Same invariant as a failed download: never advertise a bad mirror as latest.
        self.assertFalse((dest / "latest").exists())

    def test_missing_upstream_sums_fails_the_mirror(self) -> None:
        (self._source / "v1.2.3" / "SHA256SUMS").unlink()
        dest = self._temp / "staged"

        result = self._run(
            ["mirror-envy", "1.2.3", str(dest), f"--from={self._from_uri()}"]
        )

        self.assertNotEqual(0, result.returncode)
        self.assertIn("failed to download", result.stderr)
        self.assertFalse((dest / "latest").exists())

    def test_staged_windows_archives_are_named_from_any_host(self) -> None:
        """The .zip assets must be produced even when mirroring from a posix host."""
        dest = self._temp / "staged"
        self._run(["mirror-envy", "1.2.3", str(dest), f"--from={self._from_uri()}"])

        for asset in ("envy-windows-arm64.zip", "envy-windows-x86_64.zip"):
            with zipfile.ZipFile(dest / "v1.2.3" / asset) as zf:
                self.assertEqual(["envy.exe"], zf.namelist())

    def test_staged_posix_archives_hold_a_root_level_envy(self) -> None:
        """Flat archives: the bootstrap extracts and execs <tmp>/envy directly."""
        dest = self._temp / "staged"
        self._run(["mirror-envy", "1.2.3", str(dest), f"--from={self._from_uri()}"])

        with tarfile.open(dest / "v1.2.3" / "envy-linux-arm64.tar.gz") as tf:
            self.assertEqual(["envy"], tf.getnames())

    def test_relative_destination_is_cwd_relative(self) -> None:
        result = self._run(
            ["mirror-envy", "1.2.3", "./rel-staged", f"--from={self._from_uri()}"]
        )

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertTrue((self._temp / "rel-staged" / "v1.2.3").is_dir())

    def test_missing_source_asset_fails_and_writes_no_latest(self) -> None:
        """A partial mirror must never advertise itself as latest."""
        (self._source / "v1.2.3" / "envy-windows-arm64.zip").unlink()
        dest = self._temp / "staged"

        result = self._run(
            ["mirror-envy", "1.2.3", str(dest), f"--from={self._from_uri()}"]
        )

        self.assertNotEqual(0, result.returncode)
        self.assertIn("failed to download", result.stderr)
        self.assertFalse((dest / "latest").exists())

    def test_rejects_malformed_and_unsupported_destinations(self) -> None:
        for dest in ("s3:/one-slash-bucket", "https://example.com/x", "./mirror.git"):
            result = self._run(
                ["mirror-envy", "1.2.3", dest, f"--from={self._from_uri()}"]
            )
            self.assertNotEqual(0, result.returncode, f"{dest} should have been rejected")
        # The single-slash typo must not silently create a local directory named "s3:".
        self.assertFalse((self._temp / "s3:").exists())

    def test_rejects_invalid_version(self) -> None:
        result = self._run(
            ["mirror-envy", "../../etc", "./staged", f"--from={self._from_uri()}"]
        )
        self.assertNotEqual(0, result.returncode)
        self.assertIn("invalid version", result.stderr)

    # --- S3 upload ----------------------------------------------------------------

    def _s3_env(self, port: int) -> dict[str, str]:
        return s3_env(port)

    def test_s3_staging_directory_is_cleaned_up(self) -> None:
        """S3 mode stages to a private temp tree; it must not outlive the run.

        Six release archives per invocation adds up, and a predictable name in a shared temp
        directory would let another user pre-create the path.
        """
        # Redirect the product's temp dir instead of diffing the shared system one: sibling
        # S3 cases stage under identical `envy-mirror-<random>` names, and under the
        # parallel runner one of theirs is legitimately mid-flight during our snapshot
        # window. temp_directory_path() reads TMPDIR first on POSIX, TMP/TEMP on Windows.
        scratch = self._temp / "scratch"
        scratch.mkdir()
        stub = S3Stub()
        port = stub.start()
        env = {
            **self._s3_env(port),
            **dict.fromkeys(("TMPDIR", "TMP", "TEMP"), str(scratch)),
        }
        try:
            result = self._run(
                [
                    # --verbose surfaces the "staging in <path>" debug line, which is the
                    # positive control: without it a TMPDIR that went unread would leave
                    # scratch empty and pass vacuously.
                    "--verbose",
                    "mirror-envy",
                    "1.2.3",
                    "s3://test-bucket/releases",
                    f"--from={self._from_uri()}",
                ],
                env_extra=env,
            )
        finally:
            stub.stop()

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        staged_in = re.search(r"staging in (.+)", result.stderr)
        self.assertIsNotNone(staged_in, f"no staging line: {result.stderr}")
        self.assertEqual(
            scratch.resolve(),
            Path(staged_in.group(1).strip()).parent.resolve(),
            "staging did not land in the redirected temp dir",
        )
        leaked = [d for d in os.listdir(scratch) if d.startswith("envy-mirror")]
        self.assertEqual([], leaked, f"staging tree left behind: {leaked}")

    def test_uploads_every_object_to_s3(self) -> None:
        stub = S3Stub()
        port = stub.start()
        try:
            result = self._run(
                [
                    "mirror-envy",
                    "1.2.3",
                    "s3://test-bucket/releases",
                    f"--from={self._from_uri()}",
                ],
                env_extra=self._s3_env(port),
            )
        finally:
            stub.stop()

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        for asset in RELEASE_ASSETS:
            key = f"releases/v1.2.3/{asset}"
            self.assertIn(key, stub.objects, f"missing key {key}")
            self.assertEqual(
                (self._source / "v1.2.3" / asset).read_bytes(), stub.objects[key]
            )
        self.assertEqual(b"1.2.3", stub.objects["releases/latest"])
        # Without this key a pinned project cannot bootstrap from the mirror at all.
        sums_key = "releases/v1.2.3/SHA256SUMS"
        self.assertIn(sums_key, stub.objects)
        self.assertEqual(
            (self._source / "v1.2.3" / "SHA256SUMS").read_bytes(), stub.objects[sums_key]
        )
        # No double-slash keys: S3 keys are opaque, so "a//b" would be unreachable.
        for key in stub.objects:
            self.assertNotIn("//", key)
        # Signed, typed, and checksummed in a trailer S3 verifies against what it stores.
        puts = [r for r in stub.requests if r["method"] == "PUT"]
        self.assertTrue(puts and all(r["signed"] for r in puts), stub.requests)
        for put in puts:
            headers = put["headers"]
            self.assertEqual("x-amz-checksum-crc64nvme", headers.get("x-amz-trailer"))
            expected = (
                "application/gzip" if str(put["key"]).endswith(".tar.gz")
                else "application/zip" if str(put["key"]).endswith(".zip")
                else "application/octet-stream"
            )
            self.assertEqual(expected, headers.get("content-type"), put["key"])

    def test_uploads_to_bucket_root_without_prefix(self) -> None:
        stub = S3Stub()
        port = stub.start()
        try:
            result = self._run(
                ["mirror-envy", "1.2.3", "s3://test-bucket", f"--from={self._from_uri()}"],
                env_extra=self._s3_env(port),
            )
        finally:
            stub.stop()

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn("v1.2.3/envy-linux-arm64.tar.gz", stub.objects)
        self.assertEqual(b"1.2.3", stub.objects["latest"])

    def test_trailing_slash_on_s3_prefix_is_normalized(self) -> None:
        stub = S3Stub()
        port = stub.start()
        try:
            result = self._run(
                [
                    "mirror-envy",
                    "1.2.3",
                    "s3://test-bucket/releases/",
                    f"--from={self._from_uri()}",
                ],
                env_extra=self._s3_env(port),
            )
        finally:
            stub.stop()

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn("releases/v1.2.3/envy-linux-arm64.tar.gz", stub.objects)

    def test_nonexistent_bucket_reports_no_such_bucket(self) -> None:
        """envy never creates buckets; a missing one must say so."""
        stub = S3Stub()
        port = stub.start()
        try:
            result = self._run(
                [
                    "mirror-envy",
                    "1.2.3",
                    "s3://no-such-bucket/releases",
                    f"--from={self._from_uri()}",
                ],
                env_extra=self._s3_env(port),
            )
        finally:
            stub.stop()

        self.assertNotEqual(0, result.returncode)
        self.assertIn("uploads failed", result.stderr)
        self.assertIn("NoSuchBucket", result.stderr)
        self.assertIn("envy never creates buckets", result.stderr)

    def test_s3_error_body_is_quoted_as_received(self) -> None:
        """S3's own code and message, not a rewording that reads like envy's diagnosis."""
        stub = S3Stub()
        port = stub.start()
        stub.put_error = (
            501,
            "NotImplemented",
            "A header you provided implies functionality that is not implemented",
        )
        try:
            result = self._run(
                [
                    "mirror-envy",
                    "1.2.3",
                    "s3://test-bucket/releases",
                    f"--from={self._from_uri()}",
                ],
                env_extra=self._s3_env(port),
            )
        finally:
            stub.stop()

        self.assertNotEqual(0, result.returncode)
        self.assertIn(
            "NotImplemented: A header you provided implies functionality that is not"
            " implemented (HTTP 501)",
            result.stderr,
        )

    def _no_credentials_env(self, port: int) -> dict[str, str]:
        """S3 env with every credential source the chain reads pointed somewhere empty.

        Emptying the variables is what makes this hermetic: the developer running the suite
        very likely has a working session, and the chain would find it.
        """
        aws_dir = self._temp / "home" / ".aws"  # ~/.aws, where `aws sso login` writes
        aws_dir.mkdir(parents=True, exist_ok=True)
        return {
            **self._s3_env(port),
            "AWS_ACCESS_KEY_ID": "",
            "AWS_SECRET_ACCESS_KEY": "",
            "AWS_SESSION_TOKEN": "",
            "AWS_PROFILE": "envy-test-expired",
            "AWS_DEFAULT_PROFILE": "envy-test-expired",
            "AWS_SHARED_CREDENTIALS_FILE": str(aws_dir / "credentials"),
            "AWS_CONFIG_FILE": str(aws_dir / "config"),
            "AWS_WEB_IDENTITY_TOKEN_FILE": "",
            "AWS_ROLE_ARN": "",
            "HOME": str(aws_dir.parent),
            "USERPROFILE": str(aws_dir.parent),
        }

    def _write_expired_sso_session(self) -> None:
        """Plant the exact state `aws sso login` refreshes: a cached token past its expiry.

        Every credential source comes back empty -- the shape that used to reach S3 as an
        unsigned write.
        """
        aws_dir = self._temp / "home" / ".aws"
        aws_dir.mkdir(parents=True, exist_ok=True)
        start_url = "https://envy-test.awsapps.com/start"
        (aws_dir / "config").write_text(
            "[profile envy-test-expired]\n"
            f"sso_start_url = {start_url}\n"
            "sso_region = us-east-1\n"
            "sso_account_id = 123456789012\n"
            "sso_role_name = EnvyTest\n"
            "region = us-east-1\n"
        )
        # ~/.aws/sso/cache/<sha1(start_url)>.json, as the AWS CLI writes it.
        cache = aws_dir / "sso" / "cache"
        cache.mkdir(parents=True, exist_ok=True)
        digest = hashlib.sha1(start_url.encode()).hexdigest()
        (cache / f"{digest}.json").write_text(
            '{"accessToken": "envy-test-token", "expiresAt": "2020-01-01T00:00:00Z"}'
        )

    def test_expired_sso_session_is_named_once_and_not_as_a_protocol_error(self) -> None:
        self._write_expired_sso_session()
        stub = S3Stub()
        port = stub.start()
        try:
            result = self._run(
                [
                    "mirror-envy",
                    "1.2.3",
                    "s3://test-bucket/releases",
                    f"--from={self._from_uri()}",
                ],
                env_extra=self._no_credentials_env(port),
            )
        finally:
            stub.stop()

        self.assertNotEqual(0, result.returncode)
        self.assertIn("no usable AWS credentials", result.stderr)
        self.assertIn("aws sso login", result.stderr)
        self.assertIn(
            "SSO token for profile 'envy-test-expired' expired at 2020-01-01T00:00:00Z",
            result.stderr,
        )
        # Once, up front -- not once per object, and never as an S3 protocol code.
        self.assertEqual(1, result.stderr.count("no usable AWS credentials"))
        self.assertNotIn("uploads failed", result.stderr)
        self.assertNotIn("NotImplemented", result.stderr)
        self.assertEqual({}, stub.objects)

    def test_credentials_are_checked_before_anything_is_downloaded(self) -> None:
        """Six archives are fetched before the first upload; the check must precede them.

        An unreachable source mirror is the control: reaching the download leg at all would
        report that instead.
        """
        stub = S3Stub()
        port = stub.start()
        try:
            result = self._run(
                [
                    "mirror-envy",
                    "1.2.3",
                    "s3://test-bucket/releases",
                    f"--from={(self._temp / 'no-such-mirror').as_uri()}",
                ],
                env_extra=self._no_credentials_env(port),
            )
        finally:
            stub.stop()

        self.assertNotEqual(0, result.returncode)
        self.assertIn("no usable AWS credentials", result.stderr)
        self.assertNotIn("failed to download", result.stderr)

    def test_prints_paste_ready_manifest_directives(self) -> None:
        stub = S3Stub()
        port = stub.start()
        try:
            result = self._run(
                [
                    "mirror-envy",
                    "1.2.3",
                    "s3://test-bucket/releases",
                    f"--from={self._from_uri()}",
                ],
                env_extra=self._s3_env(port),
            )
        finally:
            stub.stop()

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn('-- @envy version "1.2.3"', result.stderr)
        self.assertIn('-- @envy mirror "s3://test-bucket/releases"', result.stderr)
        # All three lines together: a mirror is only as trustworthy as the pin that attests
        # it, so the pin has to be as easy to paste as the mirror URL.
        expected = hashlib.sha256(
            (self._source / "v1.2.3" / "SHA256SUMS").read_bytes()
        ).hexdigest()
        self.assertIn(f'-- @envy sha256sums "{expected}"', result.stderr)


class InitMirrorSurvivesSyncTest(EnvyTestCase):
    """`envy init --mirror` must record the mirror in the manifest.

    Before this, the mirror was stamped only into the bootstrap script, and the first
    `envy sync` re-stamped it from the manifest's (absent) @envy mirror -- silently
    reverting a configured mirror back to envy's GitHub releases.
    """

    envy_watchdog_timeout = 60

    @classmethod
    def setUpClass(cls) -> None:
        cls._envy = test_config.get_envy_executable()

    def setUp(self) -> None:
        self._temp = self.make_temp_dir("_temp")

    def tearDown(self) -> None:
        if self._temp.exists():
            shutil.rmtree(self._temp, ignore_errors=True)

    def _run(self, args: list[str]):
        env = os.environ.copy()
        env["ENVY_CACHE_ROOT"] = str(self._temp / "cache")
        return test_config.run(
            [str(self._envy), *args],
            capture_output=True,
            text=True,
            env=env,
            cwd=self._temp,
            timeout=60,
        )

    def _script(self) -> Path:
        name = "envy.bat" if sys.platform == "win32" else "envy"
        return self._temp / "tools" / name

    def test_init_mirror_is_written_to_manifest_and_survives_sync(self) -> None:
        mirror = "s3://my-envy-mirror/releases"

        init = self._run(["init", ".", "./tools", f"--mirror={mirror}"])
        self.assertEqual(0, init.returncode, f"stderr: {init.stderr}")

        manifest = (self._temp / "envy.lua").read_text()
        self.assertIn(f'-- @envy mirror "{mirror}"', manifest)

        sync = self._run(["sync"])
        self.assertEqual(0, sync.returncode, f"stderr: {sync.stderr}")

        # The manifest is the only home for the mirror. A stamped copy in the script is
        # unreachable (the parsed directive outranks it) except when the directive is
        # deleted -- and then it resolved the script to the stale mirror while the
        # re-exec'd binary went to upstream. The placeholder check keeps this from passing
        # vacuously against an unstamped template.
        after = self._script().read_text()
        self.assertNotIn(mirror, after)
        self.assertNotIn("@@DOWNLOAD_URL@@", after)

    def test_deleting_mirror_directive_resolves_script_to_upstream(self) -> None:
        """Script and binary must agree on the last mirror tier.

        `envy init --mirror` then removing the directive leaves nothing project-specific
        behind, so the script falls back to envy upstream -- exactly what reexec.cpp does.
        """
        mirror = "s3://my-envy-mirror/releases"
        init = self._run(["init", ".", "./tools", f"--mirror={mirror}"])
        self.assertEqual(0, init.returncode, f"stderr: {init.stderr}")

        manifest_path = self._temp / "envy.lua"
        kept = [
            line
            for line in manifest_path.read_text().splitlines(keepends=True)
            if "@envy mirror" not in line
        ]
        manifest_path.write_text("".join(kept))

        self.assertNotIn(mirror, self._script().read_text())

    def test_init_rejects_mirrors_that_cannot_be_stamped(self) -> None:
        """A mirror is stamped into a quoted directive and quoted shell/batch assignments.

        A newline would append arbitrary directives to envy.lua; a quote or backslash would
        produce a malformed one. Rejected before anything is written.
        """
        for bad in (
            'https://x/y"\n-- @envy version "9.9.9"',  # directive injection
            'https://x/a"b',
            "https://x/a\\b",
            "https://x/a!b",  # envy.bat runs under EnableDelayedExpansion
        ):
            result = self._run(["init", ".", "./tools", f"--mirror={bad}"])
            self.assertNotEqual(0, result.returncode, f"accepted bad mirror: {bad!r}")
            self.assertIn("mirror contains", result.stderr)
            self.assertFalse(
                (self._temp / "envy.lua").exists(),
                f"manifest was written despite rejecting {bad!r}",
            )

    def test_init_without_mirror_writes_no_directive(self) -> None:
        init = self._run(["init", ".", "./tools"])
        self.assertEqual(0, init.returncode, f"stderr: {init.stderr}")

        manifest = (self._temp / "envy.lua").read_text()
        self.assertNotIn("@envy mirror", manifest)
        # No stray blank line where the directive would have gone.
        self.assertNotIn("\n\n\n", manifest)


if __name__ == "__main__":
    unittest.main()
