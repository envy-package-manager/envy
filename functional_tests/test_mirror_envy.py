"""Functional tests for `envy mirror-envy`.

Staging runs against a file:// source mirror so no network is involved. The S3 upload leg
runs against a local S3 stub reached via AWS_ENDPOINT_URL_S3, which the AWS SDK honors
natively -- so the upload path gets real coverage without touching AWS.
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
import threading
import unittest
import zipfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from . import test_config
from .env import EnvyTestCase

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


class S3Stub:
    """Minimal S3-compatible endpoint: just enough for single-part PutObject/GetObject.

    Staged archives are tiny, so TransferManager stays under its 5MiB buffer size and never
    reaches for multipart -- which is why Create/UploadPart/CompleteMultipartUpload are not
    implemented here. If that assumption ever breaks, the SDK will 501 and the test fails
    loudly rather than silently skipping coverage.
    """

    def __init__(self) -> None:
        self.objects: dict[str, bytes] = {}
        # (status, <Code>, <Message>) to answer every PutObject with, or None to store it.
        self.put_error: tuple[int, str, str] | None = None
        self._server: ThreadingHTTPServer | None = None
        self._thread: threading.Thread | None = None
        self.port = 0

    def start(self) -> int:
        parent = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, fmt: str, *args: object) -> None:  # noqa: A003
                return

            def _key(self) -> str:
                # Path-style addressing: /<bucket>/<key>. The SDK uses path-style
                # automatically when the endpoint host is an IP literal.
                return self.path.lstrip("/").split("?", 1)[0]

            def _read_body(self) -> bytes:
                """Read the request body, unwrapping both framings the SDK applies.

                A PutObject from aws-sdk-cpp arrives double-framed: HTTP
                `Transfer-Encoding: chunked` on the outside, and `Content-Encoding:
                aws-chunked` on the inside with an `x-amz-trailer` checksum after the
                terminating zero chunk. There is no Content-Length. Reading the body fully
                also keeps HTTP/1.1 keep-alive framing intact for the next request.
                """
                if "chunked" in (self.headers.get("Transfer-Encoding") or "").lower():
                    body = self._read_http_chunked()
                else:
                    length = int(self.headers.get("Content-Length") or 0)
                    body = self.rfile.read(length) if length else b""

                if "aws-chunked" in (self.headers.get("Content-Encoding") or "").lower():
                    body = self._decode_aws_chunked(body)
                return body

            def _read_http_chunked(self) -> bytes:
                chunks: list[bytes] = []
                while True:
                    line = self.rfile.readline().strip()
                    if not line:
                        continue
                    size = int(line.split(b";", 1)[0], 16)
                    if size == 0:
                        break
                    chunks.append(self.rfile.read(size))
                    self.rfile.read(2)  # chunk-terminating CRLF
                while True:  # trailer headers, terminated by a blank line
                    line = self.rfile.readline()
                    if not line or line in (b"\r\n", b"\n"):
                        break
                return b"".join(chunks)

            @staticmethod
            def _decode_aws_chunked(buf: bytes) -> bytes:
                out = bytearray()
                pos = 0
                while True:
                    eol = buf.find(b"\r\n", pos)
                    if eol < 0:
                        break
                    size = int(buf[pos:eol].split(b";", 1)[0] or b"0", 16)
                    pos = eol + 2
                    if size == 0:
                        break  # trailers follow; the payload is complete
                    out += buf[pos : pos + size]
                    pos += size + 2  # data plus its CRLF
                return bytes(out)

            def do_PUT(self) -> None:
                body = self._read_body()  # Drained even on failure, to keep framing.
                if parent.put_error is not None:
                    parent._send_error(self, *parent.put_error)
                    return
                # Fail loudly if the stub mis-framed the body rather than silently storing
                # an empty object, which would make byte-for-byte assertions meaningless.
                declared = self.headers.get("x-amz-decoded-content-length")
                if declared is not None and len(body) != int(declared):
                    parent._send_error(self, 400, "IncompleteBody")
                    return
                key = self._key()
                if "/" not in key:
                    self.send_response(400)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                bucket, obj = key.split("/", 1)
                if bucket != "test-bucket":
                    parent._send_error(self, 404, "NoSuchBucket")
                    return
                parent.objects[obj] = body
                self.send_response(200)
                self.send_header("ETag", f'"{hashlib.md5(body).hexdigest()}"')
                self.send_header("Content-Length", "0")
                self.end_headers()

            def do_GET(self) -> None:
                key = self._key()
                bucket, _, obj = key.partition("/")
                if bucket != "test-bucket":
                    parent._send_error(self, 404, "NoSuchBucket")
                    return
                if obj not in parent.objects:
                    parent._send_error(self, 404, "NoSuchKey")
                    return
                body = parent.objects[obj]
                self.send_response(200)
                self.send_header("Content-Length", str(len(body)))
                self.send_header("ETag", f'"{hashlib.md5(body).hexdigest()}"')
                self.end_headers()
                self.wfile.write(body)

            def do_HEAD(self) -> None:
                key = self._key()
                bucket, _, obj = key.partition("/")
                if bucket != "test-bucket" or obj not in parent.objects:
                    self.send_response(404)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                self.send_response(200)
                self.send_header("Content-Length", str(len(parent.objects[obj])))
                self.end_headers()

        self._server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.port = self._server.server_address[1]
        self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)
        self._thread.start()
        return self.port

    @staticmethod
    def _send_error(
        handler: BaseHTTPRequestHandler,
        code: int,
        aws_code: str,
        message: str | None = None,
    ) -> None:
        body = (
            f'<?xml version="1.0" encoding="UTF-8"?><Error><Code>{aws_code}</Code>'
            f"<Message>{message or aws_code}</Message></Error>"
        ).encode()
        handler.send_response(code)
        handler.send_header("Content-Type", "application/xml")
        handler.send_header("Content-Length", str(len(body)))
        handler.end_headers()
        handler.wfile.write(body)

    def stop(self) -> None:
        if self._server is not None:
            self._server.shutdown()
            self._server.server_close()
        if self._thread is not None:
            self._thread.join(timeout=5)


class MirrorEnvyFunctionalTest(EnvyTestCase):
    # Downloads six archives and (in the S3 cases) uploads seven objects.
    envy_watchdog_timeout = 60

    @classmethod
    def setUpClass(cls) -> None:
        cls._envy = test_config.get_envy_production_executable()

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
        env = os.environ.copy()
        env["ENVY_CACHE_ROOT"] = str(self._temp / "cache")
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
        return {
            "AWS_ENDPOINT_URL": f"http://127.0.0.1:{port}",
            "AWS_ENDPOINT_URL_S3": f"http://127.0.0.1:{port}",
            "AWS_REGION": "us-east-1",
            "AWS_DEFAULT_REGION": "us-east-1",
            "AWS_ACCESS_KEY_ID": "test",
            "AWS_SECRET_ACCESS_KEY": "test",
            "AWS_EC2_METADATA_DISABLED": "1",
            "AWS_MAX_ATTEMPTS": "2",
        }

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

    def test_unmapped_error_body_is_quoted_rather_than_renamed(self) -> None:
        """An error code the SDK cannot map must be reported as received.

        The marshaller wraps such a body as "Unable to parse ExceptionName: <code> Message:
        <text>", which reads like a diagnosis envy made about the request it sent.
        """
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
        self.assertIn("unrecognized S3 error code NotImplemented", result.stderr)
        self.assertIn("implies functionality that is not implemented", result.stderr)
        self.assertNotIn("Unable to parse ExceptionName", result.stderr)

    def _no_credentials_env(self, port: int) -> dict[str, str]:
        """S3 env with every credential source the chain reads pointed somewhere empty.

        Emptying the variables is what makes this hermetic: the developer running the suite
        very likely has a working session, and the chain would find it.
        """
        aws_dir = self._temp / "aws"
        aws_dir.mkdir(exist_ok=True)
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
            "HOME": str(aws_dir),
            "USERPROFILE": str(aws_dir),
        }

    def _write_expired_sso_session(self) -> None:
        """Plant the exact state `aws sso login` refreshes: a cached token past its expiry.

        The SDK finds the profile, loads the token, sees it stale, and hands back empty
        credentials -- the shape that used to reach S3 as an unsigned write.
        """
        aws_dir = self._temp / "aws"
        aws_dir.mkdir(exist_ok=True)
        start_url = "https://envy-test.awsapps.com/start"
        (aws_dir / "config").write_text(
            "[profile envy-test-expired]\n"
            f"sso_start_url = {start_url}\n"
            "sso_region = us-east-1\n"
            "sso_account_id = 123456789012\n"
            "sso_role_name = EnvyTest\n"
            "region = us-east-1\n"
        )
        # Path and name are the SDK's: <credentials dir>/sso/cache/<sha1(start_url)>.json.
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
        # The SDK's own account of why, which is otherwise discarded.
        self.assertIn("Cached Token expired at 2020-01-01T00:00:00Z", result.stderr)
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
        cls._envy = test_config.get_envy_production_executable()

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
