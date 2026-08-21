"""Functional tests for version re-exec (trampoline).

Tests the behavior where a globally-installed envy detects a version mismatch
against the manifest's @envy version, downloads the correct version to a temp
directory, and re-execs itself. The re-exec'd binary's own cache::ensure_envy()
then installs it into the cache for future fast-path re-exec.

Uses file:// mirror to avoid needing an HTTP server. The test binary (the
functional tester itself) is packaged into a tar.gz/zip archive that the
re-exec download code fetches via libcurl's file:// support.
"""

from __future__ import annotations

import hashlib
import os
import platform as plat
import shutil
import stat
import sys
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path

from . import test_config
from .env import EnvyTestCase

_OS_NAME = (
    "windows"
    if sys.platform == "win32"
    else "darwin"
    if sys.platform == "darwin"
    else "linux"
)
_ARCH = plat.machine().lower()
if _ARCH in ("aarch64", "arm64"):
    _ARCH = "arm64"
elif _ARCH == "amd64":
    _ARCH = "x86_64"
_EXT = ".zip" if sys.platform == "win32" else ".tar.gz"
_BINARY_NAME = "envy.exe" if sys.platform == "win32" else "envy"


def _create_envy_archive(archive_path: Path, envy_binary: Path) -> None:
    """Create a tar.gz (POSIX) or zip (Windows) archive containing the envy binary."""
    if sys.platform == "win32":
        with zipfile.ZipFile(archive_path, "w") as zf:
            zf.write(envy_binary, "envy.exe")
    else:
        with tarfile.open(archive_path, "w:gz") as tar:
            tar.add(envy_binary, arcname="envy")


def _create_empty_archive(archive_path: Path) -> None:
    """Create a valid archive that does NOT contain an envy binary."""
    if sys.platform == "win32":
        with zipfile.ZipFile(archive_path, "w") as zf:
            zf.writestr("not-envy.txt", "this is not the binary you are looking for")
    else:
        import io

        with tarfile.open(archive_path, "w:gz") as tar:
            data = b"this is not the binary you are looking for"
            info = tarfile.TarInfo(name="not-envy.txt")
            info.size = len(data)
            tar.addfile(info, io.BytesIO(data))


class _ReexecTestBase(EnvyTestCase):
    """Shared setup for reexec tests."""

    def setUp(self) -> None:
        self._temp_dir = self.make_temp_dir("_temp_dir").resolve()
        self._envy = test_config.get_envy_executable()

        self._project = self._temp_dir / "project"
        self._project.mkdir()

        self._cache_dir = self._temp_dir / "cache"
        self._cache_dir.mkdir()

        self._releases_dir = self._temp_dir / "releases"
        self._releases_dir.mkdir()

    def tearDown(self) -> None:
        if hasattr(self, "_temp_dir") and self._temp_dir.exists():
            shutil.rmtree(self._temp_dir, ignore_errors=True)

    def _setup_reexec_project(
        self,
        version: str = "1.2.3",
        *,
        mirror: str | None = None,
        sums: str | None = None,
        pin: str | None = None,
    ) -> str:
        """Create a project with version requirement and file:// mirror.

        sums controls what the mirror publishes at v<version>/SHA256SUMS: "good" writes the
        real digest, "wrong" writes a digest that no archive matches, "absent" omits the
        file. pin overrides the manifest's `@envy sha256sums` value; when sums is not
        "absent" and pin is None, the manifest pins whatever was published. Returns the
        published sums file's own hash.
        """
        release_dir = self._releases_dir / f"v{version}"
        release_dir.mkdir(parents=True, exist_ok=True)

        archive_name = f"envy-{_OS_NAME}-{_ARCH}{_EXT}"
        _create_envy_archive(release_dir / archive_name, self._envy)

        published_pin = ""
        if sums is not None and sums != "absent":
            # Hash the archive on disk: the tar.gz header carries an mtime, so rebuilding it
            # would produce different bytes than the ones actually served.
            digest = (
                "f" * 64
                if sums == "wrong"
                else hashlib.sha256((release_dir / archive_name).read_bytes()).hexdigest()
            )
            body = f"{digest}  {archive_name}\n".encode()
            (release_dir / "SHA256SUMS").write_bytes(body)
            published_pin = hashlib.sha256(body).hexdigest()

        mirror_url = mirror or f"file://{self._releases_dir}"
        manifest = f'-- @envy version "{version}"\n'
        manifest += f'-- @envy mirror "{mirror_url}"\n'
        if pin is not None:
            manifest += f'-- @envy sha256sums "{pin}"\n'
        elif sums is not None:
            manifest += f'-- @envy sha256sums "{published_pin or "a" * 64}"\n'
        manifest += '-- @envy bin "tools"\n'
        manifest += "PACKAGES = {}\n"
        (self._project / "envy.lua").write_text(manifest)

        (self._project / "tools").mkdir(exist_ok=True)
        return published_pin

    def _get_env(self, **overrides: str) -> dict[str, str]:
        env = test_config.get_test_env()
        env["ENVY_CACHE_ROOT"] = str(self._cache_dir)
        # Start clean: remove all reexec-related env vars
        for key in (
            "ENVY_REEXEC",
            "ENVY_NO_REEXEC",
            "ENVY_TEST_SELF_VERSION",
            "ENVY_MIRROR",
        ):
            env.pop(key, None)
        env.update(overrides)
        return env

    def _run_envy(self, args: list[str | Path], **kwargs):
        return test_config.run(
            [str(self._envy), *(str(a) for a in args)],
            capture_output=True,
            text=True,
            timeout=30,
            **kwargs,
        )

    def _cached_binary_path(self, version: str = "1.2.3") -> Path:
        return self._cache_dir / "envy" / version / _BINARY_NAME


class TestReexecDecision(_ReexecTestBase):
    """Tests for when re-exec should and should not trigger."""

    def test_no_envy_version_in_manifest_skips_reexec(self) -> None:
        """Manifest without @envy version should never trigger re-exec."""
        manifest = '-- @envy bin "tools"\nPACKAGES = {}\n'
        (self._project / "envy.lua").write_text(manifest)
        (self._project / "tools").mkdir(exist_ok=True)

        # Even with a fake version mismatch, no re-exec should happen
        env = self._get_env(ENVY_TEST_SELF_VERSION="9.9.9")
        result = self._run_envy(["install"], cwd=self._project, env=env)
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")

    def test_dev_build_skips_reexec(self) -> None:
        """Dev builds (0.0.0) should never re-exec, even with version mismatch."""
        self._setup_reexec_project("1.2.3")
        # Don't set ENVY_TEST_SELF_VERSION — functional tester is version 0.0.0
        env = self._get_env()
        result = self._run_envy(["install"], cwd=self._project, env=env)
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertFalse(
            self._cached_binary_path("1.2.3").exists(),
            "Dev build (0.0.0) should never download/re-exec",
        )

    def test_version_match_skips_reexec(self) -> None:
        """When self version equals manifest version, no re-exec."""
        self._setup_reexec_project("1.2.3")
        env = self._get_env(ENVY_TEST_SELF_VERSION="1.2.3")
        result = self._run_envy(["install"], cwd=self._project, env=env)
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertFalse(
            self._cached_binary_path("1.2.3").exists(),
            "Cached binary should not exist when versions match",
        )

    def test_no_reexec_env_suppresses(self) -> None:
        """ENVY_NO_REEXEC=1 should prevent re-exec even on version mismatch."""
        self._setup_reexec_project("1.2.3")
        env = self._get_env(ENVY_TEST_SELF_VERSION="9.9.9", ENVY_NO_REEXEC="1")
        result = self._run_envy(["install"], cwd=self._project, env=env)
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertFalse(
            self._cached_binary_path("1.2.3").exists(),
            "ENVY_NO_REEXEC should suppress download/re-exec",
        )


class TestReexecDownload(_ReexecTestBase):
    """Tests for the download + re-exec chain."""

    def test_version_mismatch_downloads_and_reexecs(self) -> None:
        """Full chain: mismatch → download → extract → re-exec → child succeeds.

        Exit code 0 proves re-exec worked: if re-exec failed, the version
        mismatch would produce a non-zero exit code.
        """
        self._setup_reexec_project("1.2.3")
        env = self._get_env(ENVY_TEST_SELF_VERSION="9.9.9")
        result = self._run_envy(["install"], cwd=self._project, env=env)
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")

    def test_cached_binary_reused_no_redownload(self) -> None:
        """Pre-populated cache should be used without downloading again."""
        self._setup_reexec_project("1.2.3")

        # Pre-populate the cache with the test binary
        cached_dir = self._cache_dir / "envy" / "1.2.3"
        cached_dir.mkdir(parents=True)
        cached_binary = cached_dir / _BINARY_NAME
        shutil.copy2(self._envy, cached_binary)
        if sys.platform != "win32":
            cached_binary.chmod(
                cached_binary.stat().st_mode
                | stat.S_IXUSR
                | stat.S_IXGRP
                | stat.S_IXOTH
            )

        # Delete the release archive so download would fail if attempted
        shutil.rmtree(self._releases_dir)

        env = self._get_env(ENVY_TEST_SELF_VERSION="9.9.9")
        result = self._run_envy(["install"], cwd=self._project, env=env)
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")


class TestReexecAttestation(_ReexecTestBase):
    """`@envy sha256sums` must gate the re-exec download, not just the bootstrap script.

    Re-exec is the other way a project acquires an envy binary, so leaving it unattested
    would let a hostile mirror win simply by being reached through a binary that is already
    installed rather than through the shell script.
    """

    def test_attested_download_reexecs(self) -> None:
        self._setup_reexec_project("1.2.3", sums="good")
        env = self._get_env(ENVY_TEST_SELF_VERSION="9.9.9")

        result = self._run_envy(["install"], cwd=self._project, env=env)

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")

    def test_tampered_archive_aborts_before_extract(self) -> None:
        """SHA256SUMS names a digest the archive does not have."""
        self._setup_reexec_project("1.2.3", sums="wrong")
        env = self._get_env(ENVY_TEST_SELF_VERSION="9.9.9")

        result = self._run_envy(["install"], cwd=self._project, env=env)

        self.assertNotEqual(0, result.returncode)
        self.assertIn("attestation", result.stderr.lower())
        # Nothing may reach the cache: a re-exec'd binary self-deploys, so an unattested
        # archive that got as far as extract would install itself for every later run.
        self.assertFalse(self._cached_binary_path("1.2.3").exists())

    def test_sums_file_not_matching_the_pin_aborts(self) -> None:
        """Mirror rewrote both archive and SHA256SUMS; only the manifest pin catches it."""
        self._setup_reexec_project("1.2.3", sums="good", pin="b" * 64)
        env = self._get_env(ENVY_TEST_SELF_VERSION="9.9.9")

        result = self._run_envy(["install"], cwd=self._project, env=env)

        self.assertNotEqual(0, result.returncode)
        self.assertIn("sha256sums", result.stderr.lower())
        self.assertFalse(self._cached_binary_path("1.2.3").exists())

    def test_missing_sums_file_aborts_when_pinned(self) -> None:
        self._setup_reexec_project("1.2.3", sums="absent", pin="c" * 64)
        env = self._get_env(ENVY_TEST_SELF_VERSION="9.9.9")

        result = self._run_envy(["install"], cwd=self._project, env=env)

        self.assertNotEqual(0, result.returncode)
        self.assertIn("SHA256SUMS", result.stderr)
        self.assertFalse(self._cached_binary_path("1.2.3").exists())

    def test_unpinned_manifest_never_fetches_sums(self) -> None:
        """Opt-in: no pin means no extra request, so old mirrors keep working."""
        self._setup_reexec_project("1.2.3")  # no sums published, no pin
        env = self._get_env(ENVY_TEST_SELF_VERSION="9.9.9")

        result = self._run_envy(["install"], cwd=self._project, env=env)

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")


class TestReexecErrors(_ReexecTestBase):
    """Tests for error conditions."""

    def test_download_failure_produces_error_with_version(self) -> None:
        """Bad mirror URL should produce a clear error mentioning the version."""
        manifest = '-- @envy version "1.2.3"\n'
        manifest += '-- @envy mirror "file:///nonexistent/path"\n'
        manifest += '-- @envy bin "tools"\n'
        manifest += "PACKAGES = {}\n"
        (self._project / "envy.lua").write_text(manifest)
        (self._project / "tools").mkdir(exist_ok=True)

        env = self._get_env(ENVY_TEST_SELF_VERSION="9.9.9")
        result = self._run_envy(["install"], cwd=self._project, env=env)
        self.assertNotEqual(0, result.returncode)
        self.assertIn("1.2.3", result.stderr)

    def test_archive_missing_binary_produces_error(self) -> None:
        """Archive that extracts but doesn't contain 'envy' binary should error."""
        version = "4.5.6"
        release_dir = self._releases_dir / f"v{version}"
        release_dir.mkdir(parents=True)

        archive_name = f"envy-{_OS_NAME}-{_ARCH}{_EXT}"
        _create_empty_archive(release_dir / archive_name)

        manifest = f'-- @envy version "{version}"\n'
        manifest += f'-- @envy mirror "file://{self._releases_dir}"\n'
        manifest += '-- @envy bin "tools"\n'
        manifest += "PACKAGES = {}\n"
        (self._project / "envy.lua").write_text(manifest)
        (self._project / "tools").mkdir(exist_ok=True)

        env = self._get_env(ENVY_TEST_SELF_VERSION="9.9.9")
        result = self._run_envy(["install"], cwd=self._project, env=env)
        self.assertNotEqual(0, result.returncode)
        self.assertIn("archive did not contain expected binary", result.stderr)


class TestReexecMirrorPriority(_ReexecTestBase):
    """Tests for mirror resolution priority: ENVY_MIRROR env > meta.mirror > default."""

    def test_envy_mirror_env_overrides_manifest_mirror(self) -> None:
        """ENVY_MIRROR env var should take priority over manifest @envy mirror."""
        self._setup_reexec_project("1.2.3")

        # Set up a second releases directory for ENVY_MIRROR
        alt_releases = self._temp_dir / "alt-releases"
        release_dir = alt_releases / "v1.2.3"
        release_dir.mkdir(parents=True)
        archive_name = f"envy-{_OS_NAME}-{_ARCH}{_EXT}"
        _create_envy_archive(release_dir / archive_name, self._envy)

        # Delete the original releases so manifest mirror would fail
        shutil.rmtree(self._releases_dir)

        env = self._get_env(
            ENVY_TEST_SELF_VERSION="9.9.9",
            ENVY_MIRROR=f"file://{alt_releases}",
        )
        result = self._run_envy(["install"], cwd=self._project, env=env)
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")


class TestReexecEnvLeakage(_ReexecTestBase):
    """Tests that env vars don't leak through the re-exec boundary."""

    @unittest.skipIf(sys.platform == "win32", "POSIX env leak tests")
    def test_envy_reexec_does_not_leak_to_child_commands(self) -> None:
        """After re-exec, ENVY_REEXEC should not be visible to spawned processes.

        Use 'envy run sh -c printenv' to inspect the child's environment.
        If ENVY_REEXEC leaked, nested envy invocations would skip their own
        version checks, which would be wrong.
        """
        self._setup_reexec_project("1.2.3")
        env = self._get_env(ENVY_TEST_SELF_VERSION="9.9.9")

        result = self._run_envy(
            ["run", "sh", "-c", "printenv | grep ENVY_REEXEC || true"],
            cwd=self._project,
            env=env,
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertNotIn(
            "ENVY_REEXEC",
            result.stdout,
            "ENVY_REEXEC leaked to child process environment",
        )

    @unittest.skipIf(sys.platform == "win32", "POSIX env leak tests")
    def test_envy_test_self_version_does_not_propagate(self) -> None:
        """ENVY_TEST_SELF_VERSION should be stripped from the re-exec'd child's env.

        If it leaked, the child would see the fake version and attempt another
        re-exec (blocked by ENVY_REEXEC, but still wrong in principle).
        """
        self._setup_reexec_project("1.2.3")
        env = self._get_env(ENVY_TEST_SELF_VERSION="9.9.9")

        result = self._run_envy(
            ["run", "sh", "-c", "printenv | grep ENVY_TEST_SELF_VERSION || true"],
            cwd=self._project,
            env=env,
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertNotIn(
            "ENVY_TEST_SELF_VERSION",
            result.stdout,
            "ENVY_TEST_SELF_VERSION leaked to child process environment",
        )


class TestReexecInit(_ReexecTestBase):
    """`envy init --envy-version` re-execs before writing anything.

    Every version init stamps -- the manifest directive, the bootstrap fallback, the
    extracted types -- is the running binary's, so the requested version has to be the one
    doing the writing. There is no manifest yet, so the flag is the only source.
    """

    MARKER = "REEXEC-MARKER"

    def _publish(self, version: str = "1.2.3") -> None:
        release_dir = self._releases_dir / f"v{version}"
        release_dir.mkdir(parents=True, exist_ok=True)
        _create_envy_archive(release_dir / f"envy-{_OS_NAME}-{_ARCH}{_EXT}", self._envy)

    def _make_marker(self) -> Path:
        """A stand-in `envy` that reports its argv instead of initializing anything.

        Which binary ran the init is the whole claim here, and log lines cannot carry it:
        `reexec: switching to envy at ...` is queued on the tui thread and lost when execve
        replaces the process. A stand-in that leaves its own trace is race-free.
        """
        marker = self.make_temp_dir("marker") / "envy"
        marker.write_text(f'#!/bin/sh\necho "{self.MARKER} $*"\n')
        marker.chmod(0o755)
        return marker

    def _publish_marker(self, version: str = "1.2.3") -> None:
        release_dir = self._releases_dir / f"v{version}"
        release_dir.mkdir(parents=True, exist_ok=True)
        _create_envy_archive(
            release_dir / f"envy-{_OS_NAME}-{_ARCH}{_EXT}", self._make_marker()
        )

    def _init_target(self) -> tuple[Path, Path]:
        target = self._temp_dir / "fresh"
        return target, target / "tools"

    def _init_env(self) -> dict[str, str]:
        # ENVY_MIRROR rather than --mirror: the flag's value is written verbatim into a
        # manifest directive, which rejects the backslashes a Windows file:// URL carries.
        return self._get_env(
            ENVY_TEST_SELF_VERSION="9.9.9",
            ENVY_MIRROR=f"file://{self._releases_dir}",
        )

    def _init(self, *extra: str):
        target, tools = self._init_target()
        return target, self._run_envy(
            ["init", target, tools, *extra], env=self._init_env()
        )

    def test_unpublished_envy_version_fails(self) -> None:
        """The one assertion only a real fetch can satisfy, on every platform.

        A success-path init writes the same tree whether or not the parent re-execs -- the
        stand-in tests below prove the handoff, but they need a POSIX shell script. Asking
        for a version the mirror does not carry can only pass if the parent truly went
        looking for it, so this is what keeps the feature covered on Windows.
        """
        self._publish("1.2.3")  # a release exists, just not the one asked for

        target, result = self._init("--envy-version", "9.8.7")

        self.assertNotEqual(0, result.returncode)
        self.assertIn("9.8.7", result.stderr)
        self.assertFalse(target.exists(), "the re-exec precedes every mkdir")

    def test_envy_version_downloads_and_initializes(self) -> None:
        """End to end against a real binary: the requested version need not be present."""
        self._publish("1.2.3")

        target, result = self._init("--envy-version", "1.2.3")

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertTrue((target / "envy.lua").exists())

    @unittest.skipIf(sys.platform == "win32", "POSIX stand-in script")
    def test_downloaded_version_is_the_one_that_inits(self) -> None:
        """The whole point: this binary must not be the one writing the project."""
        self._publish_marker("1.2.3")

        target, result = self._init("--envy-version", "1.2.3")

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn(f"{self.MARKER} init", result.stdout)
        self.assertFalse(
            (target / "envy.lua").exists(),
            "the requested version was handed the init, so this binary wrote nothing",
        )

    @unittest.skipIf(sys.platform == "win32", "POSIX stand-in script")
    def test_flag_does_not_travel_to_the_child(self) -> None:
        """Every release predating the flag rejects it, so the child must never see it.

        The child re-runs argv verbatim otherwise, and an unknown option is a usage error
        there -- which would make '--envy-version' work only against releases that already
        have it.
        """
        self._publish_marker("1.2.3")

        _, result = self._init("--envy-version", "1.2.3", "--pin-sums")

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn(f"{self.MARKER} init", result.stdout)
        self.assertNotIn("--envy-version", result.stdout)
        self.assertNotIn("1.2.3", result.stdout)  # the value went with the flag
        self.assertIn("--pin-sums", result.stdout)  # everything else is forwarded intact

    @unittest.skipIf(sys.platform == "win32", "POSIX stand-in script")
    def test_cached_envy_version_needs_no_mirror(self) -> None:
        """Cache hit: nothing is published, so a download attempt would fail outright.

        The flag assertion is not redundant with the download path's: each path throws its
        own reexec_request, so only this catches a fast path that forgets to carry the
        options the child must not see -- and it is the path a second init takes.
        """
        cached = self._cached_binary_path("1.2.3")
        cached.parent.mkdir(parents=True)
        shutil.copy2(self._make_marker(), cached)

        _, result = self._init("--envy-version", "1.2.3", "--pin-sums")

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn(f"{self.MARKER} init", result.stdout)
        self.assertNotIn("--envy-version", result.stdout)
        self.assertIn("--pin-sums", result.stdout)

    @unittest.skipIf(sys.platform == "win32", "POSIX stand-in script")
    def test_no_envy_version_never_reexecs(self) -> None:
        """Omitted means "this binary": init has no manifest version to disagree with."""
        self._publish_marker("1.2.3")

        target, result = self._init()

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertNotIn(self.MARKER, result.stdout)
        self.assertTrue((target / "envy.lua").exists())

    def test_invalid_envy_version_rejected_before_writing(self) -> None:
        """The version becomes a cache path component, so traversal never gets that far."""
        target, tools = self._init_target()

        result = self._run_envy(
            ["init", target, tools, "--envy-version", ".."],
            env=self._init_env(),
        )

        self.assertNotEqual(0, result.returncode)
        self.assertIn("invalid version string", result.stderr)
        self.assertFalse(target.exists())


class TestReexecCachePath(_ReexecTestBase):
    """Tests for cache path resolution during re-exec."""

    def test_manifest_cache_directive_used_for_fast_path(self) -> None:
        """@envy cache-posix in manifest should be used for fast-path cache lookup."""
        version = "1.2.3"
        custom_cache = self._temp_dir / "custom-cache"
        custom_cache.mkdir()

        # Pre-populate the custom cache with the test binary
        cached_dir = custom_cache / "envy" / version
        cached_dir.mkdir(parents=True)
        cached_binary = cached_dir / _BINARY_NAME
        shutil.copy2(self._envy, cached_binary)
        if sys.platform != "win32":
            cached_binary.chmod(
                cached_binary.stat().st_mode
                | stat.S_IXUSR
                | stat.S_IXGRP
                | stat.S_IXOTH
            )

        # Manifest points cache to custom dir; delete releases so download would fail
        cache_key = "cache-win" if sys.platform == "win32" else "cache-posix"
        manifest = f'-- @envy version "{version}"\n'
        manifest += f'-- @envy mirror "file://{self._releases_dir}"\n'
        manifest += f'-- @envy {cache_key} "{custom_cache}"\n'
        manifest += '-- @envy bin "tools"\n'
        manifest += "PACKAGES = {}\n"
        (self._project / "envy.lua").write_text(manifest)
        (self._project / "tools").mkdir(exist_ok=True)
        shutil.rmtree(self._releases_dir)

        # Don't set ENVY_CACHE_ROOT — let the manifest cache directive win
        env = self._get_env(ENVY_TEST_SELF_VERSION="9.9.9")
        env.pop("ENVY_CACHE_ROOT", None)
        result = self._run_envy(["install"], cwd=self._project, env=env)
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")

    def test_cli_cache_root_overrides_manifest_cache(self) -> None:
        """--cache-root CLI flag should take priority over manifest cache directive."""
        version = "1.2.3"
        cli_cache = self._temp_dir / "cli-cache"
        cli_cache.mkdir()

        # Pre-populate the CLI cache with the test binary
        cached_dir = cli_cache / "envy" / version
        cached_dir.mkdir(parents=True)
        cached_binary = cached_dir / _BINARY_NAME
        shutil.copy2(self._envy, cached_binary)
        if sys.platform != "win32":
            cached_binary.chmod(
                cached_binary.stat().st_mode
                | stat.S_IXUSR
                | stat.S_IXGRP
                | stat.S_IXOTH
            )

        # Manifest points cache to a DIFFERENT dir (which doesn't have the binary)
        bad_cache = self._temp_dir / "bad-cache"
        cache_key = "cache-win" if sys.platform == "win32" else "cache-posix"
        manifest = f'-- @envy version "{version}"\n'
        manifest += f'-- @envy mirror "file:///nonexistent"\n'
        manifest += f'-- @envy {cache_key} "{bad_cache}"\n'
        manifest += '-- @envy bin "tools"\n'
        manifest += "PACKAGES = {}\n"
        (self._project / "envy.lua").write_text(manifest)
        (self._project / "tools").mkdir(exist_ok=True)

        # --cache-root should override both ENVY_CACHE_ROOT and manifest
        env = self._get_env(ENVY_TEST_SELF_VERSION="9.9.9")
        env.pop("ENVY_CACHE_ROOT", None)
        result = self._run_envy(
            ["--cache-root", cli_cache, "install"],
            cwd=self._project,
            env=env,
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")


class TestReexecEnvCleanup(_ReexecTestBase):
    """Tests that ENVY_REEXEC is consumed even when no re-exec is needed."""

    @unittest.skipIf(sys.platform == "win32", "POSIX env cleanup tests")
    def test_envy_reexec_consumed_when_versions_match(self) -> None:
        """ENVY_REEXEC should be unset even if no re-exec is triggered.

        A prior re-exec sets ENVY_REEXEC=1 in the child. The child's
        reexec_if_needed unsets it so it doesn't leak to further commands
        (e.g., envy run → child process).
        """
        self._setup_reexec_project("1.2.3")
        # Version matches, but ENVY_REEXEC is set (as if we were re-exec'd)
        env = self._get_env(ENVY_TEST_SELF_VERSION="1.2.3", ENVY_REEXEC="1")

        result = self._run_envy(
            ["run", "sh", "-c", "printenv | grep ENVY_REEXEC || true"],
            cwd=self._project,
            env=env,
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertNotIn(
            "ENVY_REEXEC",
            result.stdout,
            "ENVY_REEXEC should be consumed even when versions match",
        )


class TestReexecAcrossCommands(_ReexecTestBase):
    """Tests that re-exec works across all manifest-aware commands."""

    def test_reexec_with_install(self) -> None:
        self._setup_reexec_project("1.2.3")
        env = self._get_env(ENVY_TEST_SELF_VERSION="9.9.9")
        result = self._run_envy(["install"], cwd=self._project, env=env)
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")

    def test_reexec_with_sync(self) -> None:
        self._setup_reexec_project("1.2.3")
        env = self._get_env(ENVY_TEST_SELF_VERSION="9.9.9")
        result = self._run_envy(["sync"], cwd=self._project, env=env)
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")

    @unittest.skipIf(sys.platform == "win32", "POSIX run tests")
    def test_reexec_with_run(self) -> None:
        self._setup_reexec_project("1.2.3")
        env = self._get_env(ENVY_TEST_SELF_VERSION="9.9.9")
        result = self._run_envy(["run", "echo", "hello"], cwd=self._project, env=env)
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn("hello", result.stdout)

    def test_reexec_with_product(self) -> None:
        self._setup_reexec_project("1.2.3")
        env = self._get_env(ENVY_TEST_SELF_VERSION="9.9.9")
        result = self._run_envy(["product"], cwd=self._project, env=env)
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")

    def test_reexec_with_package(self) -> None:
        """Package command reaches package logic after re-exec.

        The error 'no package matching' proves re-exec succeeded and
        the child reached package resolution (past the reexec check).
        """
        self._setup_reexec_project("1.2.3")
        env = self._get_env(ENVY_TEST_SELF_VERSION="9.9.9")
        result = self._run_envy(["package", "nonexistent"], cwd=self._project, env=env)
        self.assertNotEqual(0, result.returncode)
        self.assertIn("no package matching", result.stderr)

    @unittest.skipIf(sys.platform == "win32", "POSIX exit code tests")
    def test_reexec_preserves_exit_code(self) -> None:
        """The exit code from the re-exec'd child should propagate back."""
        self._setup_reexec_project("1.2.3")
        env = self._get_env(ENVY_TEST_SELF_VERSION="9.9.9")
        result = self._run_envy(
            ["run", "sh", "-c", "exit 42"], cwd=self._project, env=env
        )
        self.assertEqual(42, result.returncode)


if __name__ == "__main__":
    unittest.main()
