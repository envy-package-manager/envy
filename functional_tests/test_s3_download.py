"""`envy fetch s3://...` against the local S3 stub: the download leg of envy's S3 client."""

from __future__ import annotations

import hashlib
import os
import socket
import unittest

from .env import EnvyTestCase
from .s3_stub import BUCKET, S3Stub, hermetic_env, s3_env


class S3DownloadTest(EnvyTestCase):
    envy_watchdog_timeout = 30

    def setUp(self) -> None:
        super().setUp()
        self.stub = S3Stub()
        self.stub.start()
        self.addCleanup(self.stub.stop)
        self.dest = self.work / "out" / "payload.bin"

    def fetch(self, key: str, env: dict[str, str] | None = None, bucket: str = BUCKET):
        return self.run_envy(
            "fetch",
            f"s3://{bucket}/{key}",
            self.dest,
            env=env if env is not None else self.stub.env(),
            timeout=60,
        )

    def no_credentials_env(self) -> dict[str, str]:
        """Every credential source the chain reads, pointed somewhere empty.

        The developer running the suite very likely has a working session; emptying the
        variables keeps the chain from finding it.
        """
        home = self.make_temp_dir("home")
        aws_dir = home / ".aws"  # ~/.aws, where both SDKs and the AWS CLI agree
        aws_dir.mkdir()
        return {
            **self.stub.env(),
            "AWS_ACCESS_KEY_ID": "",
            "AWS_SECRET_ACCESS_KEY": "",
            "AWS_SESSION_TOKEN": "",
            "AWS_PROFILE": "envy-test-expired",
            "AWS_DEFAULT_PROFILE": "envy-test-expired",
            "AWS_SHARED_CREDENTIALS_FILE": str(aws_dir / "credentials"),
            "AWS_CONFIG_FILE": str(aws_dir / "config"),
            "AWS_WEB_IDENTITY_TOKEN_FILE": "",
            "AWS_ROLE_ARN": "",
            "HOME": str(home),
            "USERPROFILE": str(home),
        }

    def test_downloads_an_object_byte_for_byte_with_signed_requests(self) -> None:
        payload = os.urandom(4096)
        self.stub.objects["dir/payload.bin"] = payload

        run = self.fetch("dir/payload.bin")

        self.assertEqual(0, run.returncode, run.stderr)
        self.assertEqual(payload, self.dest.read_bytes())
        gets = [r for r in self.stub.requests if r["method"] == "GET"]
        self.assertTrue(gets and all(r["signed"] for r in gets), self.stub.requests)

    def test_large_object_arrives_in_ranged_parts_and_reassembles(self) -> None:
        payload = os.urandom(20 * 1024 * 1024)
        self.stub.objects["big.bin"] = payload

        run = self.fetch("big.bin")

        self.assertEqual(0, run.returncode, run.stderr)
        self.assertEqual(
            hashlib.sha256(payload).hexdigest(),
            hashlib.sha256(self.dest.read_bytes()).hexdigest(),
        )
        ranged = [r for r in self.stub.requests if r["method"] == "GET" and r["range"]]
        self.assertGreater(len(ranged), 1, self.stub.requests)

    def test_public_object_downloads_unsigned_without_credentials(self) -> None:
        self.stub.objects["public.bin"] = b"public bytes"

        run = self.fetch("public.bin", env=self.no_credentials_env())

        self.assertEqual(0, run.returncode, run.stderr)
        self.assertEqual(b"public bytes", self.dest.read_bytes())
        self.assertFalse(any(r["signed"] for r in self.stub.requests), self.stub.requests)

    def test_transient_server_error_is_retried(self) -> None:
        self.stub.objects["flaky.bin"] = b"eventually"
        self.stub.get_failures = 1

        run = self.fetch("flaky.bin")

        self.assertEqual(0, run.returncode, run.stderr)
        self.assertEqual(b"eventually", self.dest.read_bytes())

    def test_missing_object_or_bucket_is_a_404_with_a_hint(self) -> None:
        """Checksum validation sizes the object with a HEAD first; its 404 has no S3 code."""
        for bucket, key in ((BUCKET, "no/such/key"), ("no-such-bucket", "key")):
            with self.subTest(bucket=bucket):
                run = self.fetch(key, bucket=bucket)

                self.assertNotEqual(0, run.returncode)
                self.assertIn("HTTP 404", run.stderr)
                self.assertIn("no such object, or no such bucket", run.stderr)
                self.assertFalse(self.dest.exists())

    def test_denied_request_with_an_expired_sso_session_says_to_log_in(self) -> None:
        """An expired SSO token falls back to unsigned requests; the 403 must name it."""
        env = self.no_credentials_env()
        aws_dir = os.path.dirname(env["AWS_CONFIG_FILE"])
        start_url = "https://envy-test.awsapps.com/start"
        with open(env["AWS_CONFIG_FILE"], "w", encoding="utf-8") as f:
            f.write(
                "[profile envy-test-expired]\n"
                f"sso_start_url = {start_url}\n"
                "sso_region = us-east-1\n"
                "sso_account_id = 123456789012\n"
                "sso_role_name = EnvyTest\n"
                "region = us-east-1\n"
            )
        # ~/.aws/sso/cache/<sha1(start_url)>.json: what `aws sso login` refreshes.
        cache = os.path.join(aws_dir, "sso", "cache")
        os.makedirs(cache)
        digest = hashlib.sha1(start_url.encode()).hexdigest()
        with open(os.path.join(cache, f"{digest}.json"), "w", encoding="utf-8") as f:
            f.write(
                '{"startUrl": "%s", "region": "us-east-1", "accessToken": "envy-test",'
                ' "expiresAt": "2020-01-01T00:00:00Z"}' % start_url
            )
        self.stub.objects["private.bin"] = b"secret"
        self.stub.require_auth = True

        run = self.fetch("private.bin", env=env)

        self.assertNotEqual(0, run.returncode)
        self.assertIn("HTTP 403", run.stderr)
        self.assertIn(
            "SSO token for profile 'envy-test-expired' expired at 2020-01-01T00:00:00Z",
            run.stderr,
        )
        self.assertIn("aws sso login", run.stderr)

    def test_checksum_s3_stored_is_validated(self) -> None:
        payload = os.urandom(4096)
        self.stub.objects["summed.bin"] = payload
        self.stub.checksum = "good"
        self.assertEqual(0, self.fetch("summed.bin").returncode)
        self.assertEqual(payload, self.dest.read_bytes())

        self.stub.checksum = "bad"
        run = self.fetch("summed.bin")
        self.assertNotEqual(0, run.returncode)
        self.assertIn("checksum", run.stderr.lower())
        self.assertFalse(self.dest.exists(), "bytes that failed their checksum were kept")

    def test_throttled_request_is_retried_by_envy(self) -> None:
        """aws-c-s3 retries only 500/503; 429/502/504 go through envy's fetch policy."""
        self.stub.objects["busy.bin"] = b"after a pause"
        self.stub.get_failures = 1
        self.stub.get_failure_status = 429
        env = {**self.stub.env(), "ENVY_FETCH_ATTEMPTS": "3", "ENVY_FETCH_RETRY_BASE_MS": "10"}

        run = self.fetch("busy.bin", env=env)

        self.assertEqual(0, run.returncode, run.stderr)
        self.assertEqual(b"after a pause", self.dest.read_bytes())

    def test_exhausted_retries_report_the_last_error(self) -> None:
        self.stub.objects["down.bin"] = b"never"
        self.stub.get_failures = 99

        run = self.fetch("down.bin")

        # aws-c-s3 ends a retried-out 500 with its own error and no status: name the host.
        self.assertNotEqual(0, run.returncode)
        self.assertIn("internal server error", run.stderr)
        self.assertIn(f"http://127.0.0.1:{self.stub.port}", run.stderr)

    def test_unreachable_endpoint_is_named(self) -> None:
        with socket.socket() as s:  # a port nothing listens on once closed
            s.bind(("127.0.0.1", 0))
            port = s.getsockname()[1]

        run = self.fetch("key", env=hermetic_env(s3_env(port)))

        self.assertNotEqual(0, run.returncode)
        self.assertIn(f"http://127.0.0.1:{port}", run.stderr)

    def test_bucket_in_another_region_names_it(self) -> None:
        self.stub.head_override = (301, {"x-amz-bucket-region": "eu-west-1"})

        run = self.fetch("key")

        self.assertNotEqual(0, run.returncode)
        self.assertIn("HTTP 301", run.stderr)
        self.assertIn("the bucket is in eu-west-1", run.stderr)
        self.assertIn("AWS_DEFAULT_REGION=eu-west-1", run.stderr)


if __name__ == "__main__":
    unittest.main()
