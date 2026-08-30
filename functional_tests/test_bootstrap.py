"""Bootstrap script integration tests.

Tests the bootstrap pipeline: parse manifest → download envy → cache → exec.
Uses a mock HTTP server serving the real envy binary.
"""

from __future__ import annotations

import hashlib
import http.server
import io
import os
import platform as plat
import shutil
import socketserver
import stat
import subprocess
import sys
import tarfile
import tempfile
import threading
import unittest
import zipfile
from pathlib import Path

from . import test_config
from .env import EnvyTestCase
from .trace_parser import TraceParser

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

# Inline fixture contents
FIXTURES = {
    "simple.lua": """-- @envy version "1.2.3"

PACKAGES = {
    "local.example@v1",
}
""",
    "missing_version.lua": """-- This manifest has no @envy version directive
-- @envy cache-local "custom/cache"

PACKAGES = {
    "local.example@v1",
}
""",
    "with_escapes.lua": """-- @envy version "1.2.3-\\"beta\\""
-- @envy state-dir "path/with\\\\backslash"

PACKAGES = {
    "local.example@v1",
}
""",
    "whitespace_variants.lua": """--   @envy   version   "1.0.0"
--\t@envy\tcache-local\t"tab/separated"

PACKAGES = {
    "local.example@v1",
}
""",
    "relative_cache.lua": """-- @envy version "1.2.3"
-- @envy cache-local "relcache"

PACKAGES = {
    "local.example@v1",
}
""",
    "all_directives.lua": """-- @envy version "2.0.0"
-- @envy cache-local "out/.envy"
-- @envy mirror "https://internal.corp/envy-releases"

PACKAGES = {
    "local.example@v1",
}
""",
    # The header ends at the first line of code, so neither directive below is one. Matches
    # parse_envy_meta; the launcher that read them would fetch a version the binary it execs
    # never asked for, from a mirror the binary would never use.
    "below_first_code_line.lua": """-- @envy version "1.2.3"

PACKAGES = {
    "local.example@v1",
}
-- @envy version "9.9.9"
-- @envy mirror "http://127.0.0.1:1/never-contacted"
""",
    # Indented, and behind a blank line and a plain comment: still the header. The bash
    # launcher used to anchor its match at column 0 and miss this, while parse_envy_meta
    # took it. `version` sits *under* the tab-indented lines on purpose: envy.bat gave its
    # `for /f` a space-only `delims=`, which left the tab in the first token and ended the
    # header there, so a version above them would have passed a broken parser.
    "indented_directives.lua": """-- a plain comment

\t-- a tab-indented comment
\t-- @envy bin "tools"
  -- @envy version "3.2.1"

PACKAGES = {
    "local.example@v1",
}
""",
    # A CRLF checkout. `\r` is in POSIX [[:space:]], so the blank line still reads as blank
    # and the header does not end early; the value stops at its closing quote either way.
    "crlf_directives.lua": (
        '-- @envy version "5.4.3"\r\n'
        "\r\n"
        "PACKAGES = {\r\n"
        '    "local.example@v1",\r\n'
        "}\r\n"
    ),
    # A `;`-led line is code -- Lua's empty statement prefixing a statement -- so the header
    # ends on it and the version below is not a directive. envy.bat's `for /f` reads `;` as a
    # comment marker unless handed `eol=`, and skipped the line instead of stopping on it, so
    # the 9.9.9 underneath won and the launcher fetched a release the binary never asked for.
    "semicolon_ends_header.lua": (
        '-- @envy version "1.2.3"\n' ";PACKAGES = {}\n" '-- @envy version "9.9.9"\n'
    ),
    # No line of code anywhere, so the header runs to end of file and the scan's early exit
    # never fires. Also the no-trailing-newline case.
    "header_only.lua": '-- @envy version "6.5.4"',
    # A block comment's continuation line does not start with `--`, so it ends the header the
    # same way any other line of code would, and the directive beneath it is not one.
    # parse_envy_meta agrees; pinned on both sides so the two cannot drift apart.
    "block_comment_ends_header.lua": (
        "--[[ a block comment\n"
        "  still inside it ]]\n"
        '-- @envy version "9.9.9"\n'
        "\n"
        "PACKAGES = {\n"
        '    "local.example@v1",\n'
        "}\n"
    ),
    # Inside the block, though, the line does start with `--`, so it parses. A quirk, shared
    # by both parsers; pinned so it stays deliberate rather than becoming a surprise.
    "directive_inside_block_comment.lua": (
        "--[[\n"
        '-- @envy version "7.6.5"\n'
        "]]\n"
        "\n"
        "PACKAGES = {\n"
        '    "local.example@v1",\n'
        "}\n"
    ),
    # A directive past the 20th line. Both launchers used to cap the scan there and fall
    # through to a resolved version, silently disagreeing with the binary's own reading.
    "long_preamble.lua": (
        "-- Some projects open their manifest with a license header.\n"
        + "".join(f"-- preamble line {i}\n" if i % 5 else "\n" for i in range(1, 26))
        + """-- @envy version "4.5.6"

PACKAGES = {
    "local.example@v1",
}
"""
    ),
}


class EnvyServer:
    """Simple HTTP server that serves the envy binary as tar.gz (Unix) or zip (Windows)."""

    def __init__(self, binary_path: Path):
        self.binary_path = binary_path
        self.binary_content = binary_path.read_bytes()
        self.server: socketserver.TCPServer | None = None
        self.thread: threading.Thread | None = None
        self.port: int = 0
        self.request_paths: list[str] = []

        # Pre-create tar.gz archive for Unix
        tar_buffer = io.BytesIO()
        with tarfile.open(fileobj=tar_buffer, mode="w:gz") as tar:
            info = tarfile.TarInfo(name="envy")
            info.size = len(self.binary_content)
            info.mode = 0o755
            tar.addfile(info, io.BytesIO(self.binary_content))
        self.tar_gz_content = tar_buffer.getvalue()

        # Pre-create zip archive for Windows
        zip_buffer = io.BytesIO()
        with zipfile.ZipFile(zip_buffer, "w", zipfile.ZIP_DEFLATED) as zf:
            zf.writestr("envy.exe", self.binary_content)
        self.zip_content = zip_buffer.getvalue()

        # Attestation knobs. corrupt_archive flips the served archive bytes while leaving
        # SHA256SUMS alone (a mirror that tampers with one object); sums_body replaces the
        # sums file itself (a mirror that tampers with both, which only the manifest's pin
        # can catch); serve_sums=False models a mirror missing the file entirely.
        self.host_archive_name = f"envy-{_OS_NAME}-{_ARCH}{_EXT}"
        self.corrupt_archive = False
        self.serve_sums = True
        self.sums_body: bytes | None = None
        # What `GET /latest` answers. None models upstream GitHub, which publishes no such
        # object; bytes model a mirror written by `envy mirror-envy`. Deliberately bytes and
        # not str: the real object carries no trailing newline, and cmd's `set /p` reading a
        # file without a line terminator is exactly the step under test on Windows.
        self.latest_body: bytes | None = None

    @property
    def pristine_archive(self) -> bytes:
        return self.zip_content if sys.platform == "win32" else self.tar_gz_content

    @property
    def published_sums(self) -> bytes:
        """What the mirror serves at v<version>/SHA256SUMS."""
        if self.sums_body is not None:
            return self.sums_body
        digest = hashlib.sha256(self.pristine_archive).hexdigest()
        return f"{digest}  {self.host_archive_name}\n".encode()

    @property
    def sums_pin(self) -> str:
        """The value an `@envy sha256sums` directive would carry for this mirror."""
        return hashlib.sha256(self.published_sums).hexdigest()

    def start(self) -> int:
        """Start the server and return the port number."""
        parent = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self) -> None:
                parent.request_paths.append(self.path)
                if self.path == "/latest":
                    if parent.latest_body is None:
                        self.send_response(404)
                        self.end_headers()
                        return
                    content, content_type = parent.latest_body, "text/plain"
                elif self.path.endswith("SHA256SUMS"):
                    if not parent.serve_sums:
                        self.send_response(404)
                        self.end_headers()
                        return
                    content, content_type = parent.published_sums, "text/plain"
                else:
                    match self.path.rsplit(".", 1)[-1]:
                        case "gz" if self.path.endswith(".tar.gz"):
                            content, content_type = (
                                parent.tar_gz_content,
                                "application/gzip",
                            )
                        case "zip":
                            content, content_type = parent.zip_content, "application/zip"
                        case _:
                            self.send_response(404)
                            self.end_headers()
                            return
                    if parent.corrupt_archive:
                        content = content + b"corrupted"
                self.send_response(200)
                self.send_header("Content-Type", content_type)
                self.send_header("Content-Length", str(len(content)))
                self.end_headers()
                self.wfile.write(content)

            def log_message(self, format: str, *args: object) -> None:
                pass

        self.server = socketserver.TCPServer(("127.0.0.1", 0), Handler)
        self.port = self.server.server_address[1]
        self.thread = threading.Thread(target=self.server.serve_forever)
        self.thread.daemon = True
        self.thread.start()
        return self.port

    def stop(self) -> None:
        """Stop the server."""
        if self.server:
            self.server.shutdown()
            self.server.server_close()


class RedirectChainServer:
    """Stands in for GitHub's /releases/latest, which answers only with a redirect.

    `paths` is served in order: every hop 301s to the next, the last answers 200. Two hops
    before the tag page is the shape a renamed or transferred repo produces, and the reason
    reading hop 1's trailing path segment resolves the literal string `latest`.
    """

    def __init__(self, paths: list[str]):
        self.paths = paths
        self.requested: list[str] = []
        self.server: socketserver.TCPServer | None = None
        self.thread: threading.Thread | None = None
        self.port: int = 0

    def start(self) -> int:
        parent = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self) -> None:
                parent.requested.append(self.path)
                if self.path not in parent.paths:
                    self.send_response(404)
                    self.end_headers()
                    return
                hop = parent.paths.index(self.path)
                if hop + 1 < len(parent.paths):
                    self.send_response(301)
                    self.send_header("Location", parent.url_for(parent.paths[hop + 1]))
                    self.end_headers()
                    return
                body = b"<html>release page</html>"
                self.send_response(200)
                self.send_header("Content-Type", "text/html")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, format: str, *args: object) -> None:
                pass

        self.server = socketserver.TCPServer(("127.0.0.1", 0), Handler)
        self.port = self.server.server_address[1]
        self.thread = threading.Thread(target=self.server.serve_forever)
        self.thread.daemon = True
        self.thread.start()
        return self.port

    def url_for(self, path: str) -> str:
        return f"http://127.0.0.1:{self.port}{path}"

    @property
    def entry_url(self) -> str:
        """What a bootstrap script carries stamped in as LATEST_URL."""
        return self.url_for(self.paths[0])

    @property
    def missing_url(self) -> str:
        """A URL this server 404s, standing in for unreachable GitHub."""
        return self.url_for("/no-such-repo/releases/latest")

    def stop(self) -> None:
        if self.server:
            self.server.shutdown()
            self.server.server_close()


class BootstrapIntegrationTest(EnvyTestCase):
    """Integration tests for the bootstrap scripts."""

    # These cases spawn a bootstrap that downloads and re-execs a real envy; the 5s default
    # watchdog trips first and os._exit(1)s the whole run, which also makes the 30s
    # subprocess timeouts below unreachable.
    envy_watchdog_timeout = 60

    @classmethod
    def setUpClass(cls) -> None:
        cls._project_root = Path(__file__).resolve().parent.parent
        cls._envy_binary = test_config.get_envy_production_executable()
        cls._bootstrap_unix = cls._project_root / "src/resources/envy"
        cls._bootstrap_windows = cls._project_root / "src/resources/envy.bat"

    def setUp(self) -> None:
        if sys.platform == "win32":
            self.assertTrue(
                self._bootstrap_windows.exists(),
                f"Windows bootstrap script not found at {self._bootstrap_windows}",
            )
        else:
            self.assertTrue(
                self._bootstrap_unix.exists(),
                f"Unix bootstrap script not found at {self._bootstrap_unix}",
            )

        self._temp_dir = self.make_temp_dir("_temp_dir")
        self._server = EnvyServer(self._envy_binary)
        self._port = self._server.start()
        # Every stamped script points LATEST_URL here instead of at github.com, so a test
        # can assert either that the chain resolved a tag or -- for the tiers that must not
        # need the network at all -- that this server was never asked anything.
        self._github = RedirectChainServer(
            [
                "/oldowner/envy/releases/latest",
                "/envy-package-manager/envy/releases/latest",
                "/envy-package-manager/envy/releases/tag/v4.5.6",
            ]
        )
        self._github.start()

    def tearDown(self) -> None:
        if hasattr(self, "_github"):
            self._github.stop()
        if hasattr(self, "_server"):
            self._server.stop()
        if hasattr(self, "_temp_dir") and self._temp_dir.exists():
            # Set ENVY_TEST_KEEP_TEMP to inspect the mock-aws invocation log and the staged
            # tree after a failure. Tests embed the log in their assertion messages, so the
            # default is still to leave nothing behind.
            if os.environ.get("ENVY_TEST_KEEP_TEMP"):
                sys.stderr.write(f"\nENVY_TEST_KEEP_TEMP: kept {self._temp_dir}\n")
                return
            shutil.rmtree(self._temp_dir, ignore_errors=True)

    # --- mock AWS CLI -------------------------------------------------------------
    #
    # The bootstrap's s3:// branch shells out to `aws`. Rather than reach S3, drop a mock
    # first on PATH that logs its argv and serves objects from a local tree. The log is the
    # proof the mock (and not a real aws) ran.

    def _install_mock_aws(self) -> tuple[Path, Path, Path]:
        """Create the mock aws CLI. Returns (bindir, s3root, logfile)."""
        bindir = self._temp_dir / "mockbin"
        s3root = self._temp_dir / "s3root"
        logfile = self._temp_dir / "aws-invocations.log"
        bindir.mkdir(parents=True)
        s3root.mkdir(parents=True)

        # Argument positions are fixed by the bootstrap's own call shape
        # (`aws s3 cp --only-show-errors <uri> <dest>`); a mock that assumes them fails
        # loudly if that shape ever changes.
        (bindir / "aws").write_text(
            "#!/bin/sh\n"
            'printf "%s\\n" "$*" >> "$MOCK_AWS_LOG"\n'
            '[ "$1" = "s3" ] && [ "$2" = "cp" ] || { echo "mock aws: unexpected argv: $*" >&2; exit 64; }\n'
            'uri="$4"; dest="$5"\n'
            'key="${uri#*://}"; key="${key#*/}"\n'
            'src="$MOCK_AWS_ROOT/$key"\n'
            '[ -f "$src" ] || { echo "mock aws: NoSuchKey: $key" >&2; exit 1; }\n'
            'if [ "$dest" = "-" ]; then cat "$src"; else cp "$src" "$dest"; fi\n'
        )
        (bindir / "aws").chmod(0o755)

        (bindir / "aws.bat").write_text(
            "@echo off\r\n"
            "setlocal EnableDelayedExpansion\r\n"
            '>>"%MOCK_AWS_LOG%" echo %*\r\n'
            'if not "%~1"=="s3" (echo mock aws: unexpected argv: %* >&2 & exit /b 64)\r\n'
            'if not "%~2"=="cp" (echo mock aws: unexpected argv: %* >&2 & exit /b 64)\r\n'
            'set "URI=%~4"\r\n'
            'set "DEST=%~5"\r\n'
            'set "KEY=!URI:*://=!"\r\n'
            "for /f \"tokens=1,* delims=/\" %%a in (\"!KEY!\") do set \"KEY=%%b\"\r\n"
            'set "SRC=%MOCK_AWS_ROOT%\\!KEY:/=\\!"\r\n'
            'if not exist "!SRC!" (echo mock aws: NoSuchKey: !KEY! >&2 & exit /b 1)\r\n'
            'copy /y "!SRC!" "!DEST!" >nul || exit /b 1\r\n'
        )

        return bindir, s3root, logfile

    def _mock_aws_env(self, bindir: Path, s3root: Path, logfile: Path) -> dict[str, str]:
        """PATH-prepend the mock and poison real AWS access.

        If PATH injection ever regresses, `aws` resolves to the runner image's real CLI --
        preinstalled on every GitHub hosted runner. The poisoned endpoint and config paths
        make that physically unable to reach S3 instead of merely failing the assertions
        after a live request.
        """
        env = {
            "PATH": f"{bindir}{os.pathsep}{os.environ.get('PATH', '')}",
            "MOCK_AWS_LOG": str(logfile),
            "MOCK_AWS_ROOT": str(s3root),
            "AWS_ENDPOINT_URL": "http://127.0.0.1:1",
            "AWS_ENDPOINT_URL_S3": "http://127.0.0.1:1",
            "AWS_CONFIG_FILE": str(self._temp_dir / "no-such-aws-config"),
            "AWS_SHARED_CREDENTIALS_FILE": str(self._temp_dir / "no-such-aws-creds"),
            "AWS_EC2_METADATA_DISABLED": "1",
            "AWS_MAX_ATTEMPTS": "1",
            "AWS_ACCESS_KEY_ID": "",
            "AWS_SECRET_ACCESS_KEY": "",
            "AWS_SESSION_TOKEN": "",
            "AWS_PROFILE": "",
        }
        return env

    def _seed_s3_release(self, s3root: Path, prefix: str, version: str) -> None:
        """Write the host-platform archive plus a `latest` file under a bucket prefix."""
        base = s3root / prefix if prefix else s3root
        (base / f"v{version}").mkdir(parents=True, exist_ok=True)
        content = (
            self._server.zip_content
            if sys.platform == "win32"
            else self._server.tar_gz_content
        )
        (base / f"v{version}" / f"envy-{_OS_NAME}-{_ARCH}{_EXT}").write_bytes(content)
        (base / "latest").write_text(version)

    def _get_bootstrap_script(self) -> Path:
        if sys.platform == "win32":
            return self._bootstrap_windows
        return self._bootstrap_unix

    @staticmethod
    def _write_verbatim(path: Path, text: str) -> Path:
        """Write LF-terminated text as bytes, defeating Python's newline translation.

        `write_text` would emit CRLF on Windows. Neither envy nor a checked-out repo does
        for a *manifest*: envy writes them byte for byte and consumer repos carry `* -text`
        to keep git from touching them. Anything cmd.exe or `for /f` does differently with
        LF has to be caught here or not at all.

        The launchers are the exception -- see _stamp_bootstrap.

        The encoding is explicit for the same reason: this helper's contract is exact bytes.
        """
        path.write_bytes(text.encode("utf-8"))
        return path

    def _stamp_bootstrap(
        self,
        dest: Path,
        fallback_version: str = "1.2.3",
        latest_url: str | None = None,
        min_directive_version: str | None = None,
    ) -> Path:
        """Write the bootstrap script with every placeholder `envy init` fills in.

        LATEST_URL defaults to a path the redirect stub 404s, which is what an unreachable
        github.com looks like to the script. It used to be left unstamped, so the
        GitHub-redirect tier failed on a nonsense URL and its parsing was never covered.
        DEFAULT_MIRROR points at the stub too: no test should resolve through it, so any
        that does fails loudly on a 404 naming the URL instead of quietly succeeding.
        """
        # Explicit utf-8: read_text() otherwise decodes with locale.getpreferredencoding(),
        # which is cp1252 on a Windows runner outside UTF-8 mode.
        content = self._get_bootstrap_script().read_text(encoding="utf-8")
        content = content.replace("@@ENVY_VERSION@@", fallback_version)
        content = content.replace("@@LATEST_URL@@", latest_url or self._github.missing_url)
        content = content.replace(
            "@@DOWNLOAD_URL@@", self._github.url_for("/upstream-not-a-mirror")
        )
        # Left unstamped by default. The guard's own shape test then skips it, which is the
        # behavior an unstamped template must have -- reaching shell arithmetic with
        # '@@MIN_DIRECTIVE_VERSION@@' would abort under `set -e`.
        if min_directive_version is not None:
            content = content.replace(
                "@@MIN_DIRECTIVE_VERSION@@", min_directive_version
            )
        # CRLF, matching stamp_bootstrap(): this harness stamps the template itself rather
        # than running `envy init`, so it must reproduce that or test a file envy never writes.
        if sys.platform == "win32":
            content = content.replace("\n", "\r\n")
        self._write_verbatim(dest, content)
        if sys.platform != "win32":
            dest.chmod(dest.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
        return dest

    def _setup_test_project(
        self,
        fixture_name: str,
        fallback_version: str = "1.2.3",
        latest_url: str | None = None,
    ) -> Path:
        """Set up a test project with manifest and bootstrap script."""
        project_dir = self._temp_dir / "project"
        bin_dir = project_dir / "tools"
        project_dir.mkdir(parents=True)
        bin_dir.mkdir(parents=True)

        # Write fixture content from inline string
        self._write_verbatim(project_dir / "envy.lua", FIXTURES[fixture_name])

        return self._stamp_bootstrap(
            bin_dir / ("envy.bat" if sys.platform == "win32" else "envy"),
            fallback_version,
            latest_url,
        )

    def _run_bootstrap(
        self,
        bootstrap_script: Path,
        args: list[str],
        cache_dir: Path | None = None,
        env_overrides: dict[str, str] | None = None,
        set_mirror: bool = True,
        set_cache_root: bool = True,
        cwd: Path | None = None,
        drop_env: tuple[str, ...] = (),
    ) -> subprocess.CompletedProcess[str]:
        """Run the bootstrap script and return the result.

        set_mirror=False drops ENVY_MIRROR entirely, which is what a manifest-mirror test
        needs now that env wins over the manifest. It must be dropped rather than set to "":
        cmd.exe has no concept of an empty-but-defined variable, so `if defined` would
        disagree with bash's `${VAR:-}` and the two scripts would diverge under test.

        set_cache_root=False does the same for ENVY_CACHE_ROOT, which a manifest-cache test
        needs for the same reason, and redirects every platform's default-root variable into
        the temp tree so a resolution that falls all the way through lands somewhere
        observable instead of in the developer's real cache.
        """
        env = os.environ.copy()
        if set_mirror:
            env["ENVY_MIRROR"] = f"http://127.0.0.1:{self._port}"
        else:
            env.pop("ENVY_MIRROR", None)
        if set_cache_root:
            env["ENVY_CACHE_ROOT"] = str(cache_dir or self._temp_dir / "cache")
        else:
            env = test_config.sandbox_home_env(self._temp_dir / "home", env)
        if env_overrides:
            env.update(env_overrides)
        # Deleted, never set to "": cmd.exe has no empty-but-defined variable, so `if
        # defined` and bash's `${VAR:-}` would disagree and the two launchers diverge.
        for name in drop_env:
            env.pop(name, None)

        if sys.platform == "win32":
            cmd = ["cmd.exe", "/c", str(bootstrap_script), *args]
        else:
            cmd = [str(bootstrap_script), *args]

        return test_config.run(
            cmd,
            capture_output=True,
            text=True,
            env=env,
            cwd=cwd or bootstrap_script.parent.parent,
            timeout=30,
        )

    def test_bootstrap_downloads_and_executes(self) -> None:
        """Test that bootstrap downloads envy and executes it."""
        bootstrap = self._setup_test_project("simple.lua")
        result = self._run_bootstrap(bootstrap, ["version"])

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        # envy version outputs to stderr
        self.assertIn("envy version", result.stderr)

    # A loadable project: the anchor tests below run a real subcommand, so the manifest has
    # to carry '@envy bin' the way every other fixture here deliberately does not.
    _ANCHORED_MANIFEST = '-- @envy version "1.2.3"\n-- @envy bin "tools"\nPACKAGES = {}\n'

    def _anchored_project(self, name: str) -> Path:
        """A second project the launcher must not resolve, and its directory."""
        root = self._temp_dir / name
        (root / "tools").mkdir(parents=True)
        self._write_verbatim(root / "envy.lua", self._ANCHORED_MANIFEST)
        return root

    def _resolved_manifest(self, result, trace: Path) -> str:
        """The manifest path the re-exec'd binary settled on, per its own trace.

        Asserting on the trace rather than stdout because the bootstrap's whole job is to
        hand off: what the binary decided after the exec is the only thing under test.
        """
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertTrue(trace.exists(), f"no trace written; stderr: {result.stderr}")
        events = [e for e in TraceParser(trace).parse() if e.event == "manifest_resolved"]
        self.assertTrue(events, f"no manifest_resolved event; stderr: {result.stderr}")
        return events[0].raw["path"]

    def test_bootstrap_anchors_the_binary_on_its_own_directory(self) -> None:
        """The launcher's project must survive the exec, whatever CWD invoked it.

        Runs the download tier deliberately: the cached-binary tier and this one exec
        separately, and only one of them carried the injected --project at first.
        """
        bootstrap = self._setup_test_project("simple.lua")
        self._write_verbatim(
            bootstrap.parent.parent / "envy.lua", self._ANCHORED_MANIFEST
        )
        elsewhere = self._anchored_project("elsewhere")

        trace = self._temp_dir / "anchor.jsonl"
        result = self._run_bootstrap(
            bootstrap, [f"--trace=file:{trace}", "product"], cwd=elsewhere
        )

        self.assertPathEndsWith(self._resolved_manifest(result, trace), "project/envy.lua")

    def test_bootstrap_anchor_yields_to_a_typed_one(self) -> None:
        """The injection is a default: it leads, so the caller's own --project wins."""
        bootstrap = self._setup_test_project("simple.lua")
        self._write_verbatim(
            bootstrap.parent.parent / "envy.lua", self._ANCHORED_MANIFEST
        )
        elsewhere = self._anchored_project("elsewhere")

        trace = self._temp_dir / "anchor-typed.jsonl"
        result = self._run_bootstrap(
            bootstrap,
            [f"--trace=file:{trace}", "--project", str(elsewhere), "product"],
        )

        self.assertPathEndsWith(
            self._resolved_manifest(result, trace), "elsewhere/envy.lua"
        )

    @unittest.skipUnless(
        sys.platform == "win32",
        "exercises the Windows envy.bat native curl.exe/tar.exe path",
    )
    def test_bootstrap_succeeds_without_powershell(self) -> None:
        """Bootstrap must not depend on PowerShell to download and extract.

        Machine policy (WDAC/AppLocker constrained-language mode, disabled module
        autoloading, a tampered PSModulePath) can block the Microsoft.PowerShell.Archive
        script module that `Expand-Archive` lives in, while compiled binaries still run.
        The bootstrap prefers native curl.exe/tar.exe; PowerShell is only a fallback.
        Shadow `powershell`/`pwsh` with always-failing stubs earlier in PATH (a tripwire:
        any PowerShell use in the happy path would fail the operation) and assert the
        bootstrap still downloads, extracts, and execs.
        """
        bootstrap = self._setup_test_project("simple.lua")

        sabotage = self._temp_dir / "sabotage"
        sabotage.mkdir()
        for name in ("powershell.bat", "pwsh.bat"):
            (sabotage / name).write_text(
                "@echo PowerShell blocked by policy (test) 1>&2\r\n@exit /b 1\r\n"
            )
        scrubbed_path = f"{sabotage}{os.pathsep}{os.environ.get('PATH', '')}"

        result = self._run_bootstrap(
            bootstrap, ["version"], env_overrides={"PATH": scrubbed_path}
        )

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn("envy version", result.stderr)
        # Confirms the download happened over the network (via curl.exe), not a cache hit.
        self.assertTrue(
            any(p.endswith(".zip") for p in self._server.request_paths),
            f"expected a .zip download request, got: {self._server.request_paths}",
        )

    def test_bootstrap_caches_binary(self) -> None:
        """Test that bootstrap uses cached binary when present."""
        bootstrap = self._setup_test_project("simple.lua")
        cache_dir = self._temp_dir / "cache"

        # First run downloads (to temp, envy would self-deploy but we simulate it)
        result1 = self._run_bootstrap(bootstrap, ["version"], cache_dir)
        self.assertEqual(0, result1.returncode, f"stderr: {result1.stderr}")
        self.assertIn("Downloading envy", result1.stderr)
        self.assertIn("envy version", result1.stderr)

        # Manually populate cache to simulate envy self-deployment
        cached_binary = (
            cache_dir
            / "envy"
            / "1.2.3"
            / ("envy.exe" if sys.platform == "win32" else "envy")
        )
        cached_binary.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(self._envy_binary, cached_binary)
        if sys.platform != "win32":
            cached_binary.chmod(cached_binary.stat().st_mode | stat.S_IXUSR)

        # Second run uses cache (no download message)
        result2 = self._run_bootstrap(bootstrap, ["version"], cache_dir)
        self.assertEqual(0, result2.returncode, f"stderr: {result2.stderr}")
        self.assertNotIn("Downloading", result2.stderr)
        self.assertIn("envy version", result2.stderr)

    def test_bootstrap_relative_cache_anchors_to_manifest(self) -> None:
        """A relative cache directive resolves against the manifest, not the caller's cwd.

        find_manifest already walks from the script's own directory, so CACHE was the one
        thing left that drifted with the cwd -- into a fresh tree that refetches every
        package. Seed the manifest-anchored tree and run from an unrelated directory: only
        a correct resolution finds the seeded binary, and anything else downloads.
        """
        bootstrap = self._setup_test_project("relative_cache.lua")
        project_dir = bootstrap.parent.parent
        cached_binary = (
            project_dir
            / "relcache"
            / "envy"
            / "1.2.3"
            / ("envy.exe" if sys.platform == "win32" else "envy")
        )
        cached_binary.parent.mkdir(parents=True)
        shutil.copy(self._envy_binary, cached_binary)
        if sys.platform != "win32":
            cached_binary.chmod(cached_binary.stat().st_mode | stat.S_IXUSR)

        elsewhere = self._temp_dir / "elsewhere"
        elsewhere.mkdir()
        result = self._run_bootstrap(
            bootstrap, ["version"], set_cache_root=False, cwd=elsewhere
        )

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn("envy version", result.stderr)
        self.assertNotIn("Downloading", result.stderr)
        self.assertFalse(
            (elsewhere / "relcache").exists(),
            "cache tree was anchored to the cwd",
        )

    def test_bootstrap_rejects_an_absolute_cache_local(self) -> None:
        """An absolute '@envy cache-local' is an error, not a path taken verbatim.

        cache-local names a project-local tree, so it is a relative literal by definition;
        an absolute cache root is ENVY_CACHE_ROOT's job. The launcher does not validate --
        recognizing the forms it rejects would need the very parser this change deleted --
        so it anchors, misses, and the binary it execs produces the error. That the error
        arrives at all is the contract; which layer authored it is not.

        Driven through `cache --root`, a command that resolves the project's cache. A
        manifest-free command like `version` deliberately does *not* fail here: main()'s
        pre-dispatch root resolution is best-effort precisely so an unrelated bad directive
        cannot break commands that never needed the manifest.
        """
        project_dir = self._temp_dir / "project"
        bin_dir = project_dir / "tools"
        bin_dir.mkdir(parents=True)

        cache = self._temp_dir / "absolute-cache"
        value = str(cache).replace("\\", "\\\\")
        self._write_verbatim(
            project_dir / "envy.lua",
            f'-- @envy version "1.2.3"\n'
            f'-- @envy cache-local "{value}"\n'
            f"\nPACKAGES = {{}}\n",
        )
        bootstrap = self._stamp_bootstrap(
            bin_dir / ("envy.bat" if sys.platform == "win32" else "envy"), "1.2.3"
        )

        # Seed the binary where an absolute directive would have put it, so the test fails
        # loudly if the value is ever honored again instead of rejected.
        cached_binary = (
            cache / "envy" / "1.2.3" / ("envy.exe" if sys.platform == "win32" else "envy")
        )
        cached_binary.parent.mkdir(parents=True)
        shutil.copy(self._envy_binary, cached_binary)
        if sys.platform != "win32":
            cached_binary.chmod(cached_binary.stat().st_mode | stat.S_IXUSR)

        result = self._run_bootstrap(
            bootstrap, ["cache", "--root"], set_cache_root=False
        )

        self.assertNotEqual(0, result.returncode, f"stdout: {result.stdout}")
        self.assertIn("cache-local", result.stderr)

    def _write_project_with_cache_local(self, version: str) -> Path:
        project_dir = self._temp_dir / "project"
        bin_dir = project_dir / "tools"
        bin_dir.mkdir(parents=True)
        self._write_verbatim(
            project_dir / "envy.lua",
            f'-- @envy version "{version}"\n'
            f'-- @envy cache-local "out/.envy"\n'
            f"\nPACKAGES = {{}}\n",
        )
        return bin_dir / ("envy.bat" if sys.platform == "win32" else "envy")

    def test_bootstrap_refuses_an_envy_that_predates_the_cache_directives(self) -> None:
        """An older envy ignores cache-local and would silently use the shared cache.

        The worst failure mode in this area: the project asks for a hermetic tree, the old
        binary drops the unknown directive, installs everything into the user's home, and
        exits 0. This is a regression guard, since cache-posix *was* understood by old
        binaries -- so the launcher has to refuse before it downloads anything.
        """
        dest = self._write_project_with_cache_local("0.1.9")
        bootstrap = self._stamp_bootstrap(dest, "0.1.9", min_directive_version="0.2.0")

        result = self._run_bootstrap(
            bootstrap, ["cache", "--root"], set_cache_root=False
        )

        self.assertNotEqual(0, result.returncode, f"stdout: {result.stdout}")
        self.assertIn("cache-local", result.stderr)
        self.assertNotIn("Downloading", result.stderr)

    def test_bootstrap_allows_an_envy_at_the_minimum_version(self) -> None:
        """The guard is 'older than', not 'other than'."""
        dest = self._write_project_with_cache_local("0.2.0")
        bootstrap = self._stamp_bootstrap(dest, "0.2.0", min_directive_version="0.2.0")

        result = self._run_bootstrap(bootstrap, ["version"], set_cache_root=False)

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")

    def test_bootstrap_compares_version_fields_numerically(self) -> None:
        """0.10.0 is newer than 0.2.0; a string compare says otherwise."""
        dest = self._write_project_with_cache_local("0.10.0")
        bootstrap = self._stamp_bootstrap(dest, "0.10.0", min_directive_version="0.2.0")

        result = self._run_bootstrap(bootstrap, ["version"], set_cache_root=False)

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")

    def test_bootstrap_lets_a_dev_build_through_the_version_guard(self) -> None:
        """0.0.0 is built from a working tree, so its support cannot be read off a number.

        reexec_should() in src/reexec.cpp lets a 0.0.0 self through for the same reason.
        """
        dest = self._write_project_with_cache_local("0.0.0")
        bootstrap = self._stamp_bootstrap(dest, "0.0.0", min_directive_version="0.2.0")

        result = self._run_bootstrap(bootstrap, ["version"], set_cache_root=False)

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")

    def test_bootstrap_uses_fallback_when_version_missing(self) -> None:
        """Test that bootstrap resolves a version when @envy version is missing.

        Without a latest file or GitHub access, falls through to FALLBACK_VERSION.
        The mock server serves any .tar.gz path, so whichever version is resolved works.
        """
        bootstrap = self._setup_test_project(
            "missing_version.lua", fallback_version="9.9.9"
        )
        result = self._run_bootstrap(bootstrap, ["version"])

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn("envy version", result.stderr)

    def test_bootstrap_uses_latest_file_when_version_missing(self) -> None:
        """Test that bootstrap reads $CACHE/envy/latest when @envy version is absent."""
        bootstrap = self._setup_test_project(
            "missing_version.lua", fallback_version="9.9.9"
        )
        cache_dir = self._temp_dir / "cache"

        # Pre-populate the latest pointer and binary
        latest_ver = "5.5.5"
        (cache_dir / "envy").mkdir(parents=True, exist_ok=True)
        (cache_dir / "envy" / "latest").write_text(latest_ver)
        cached_binary = (
            cache_dir
            / "envy"
            / latest_ver
            / ("envy.exe" if sys.platform == "win32" else "envy")
        )
        cached_binary.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(self._envy_binary, cached_binary)
        if sys.platform != "win32":
            cached_binary.chmod(cached_binary.stat().st_mode | stat.S_IXUSR)

        result = self._run_bootstrap(bootstrap, ["version"], cache_dir)
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertNotIn("Downloading", result.stderr)
        self.assertIn("envy version", result.stderr)

    def test_bootstrap_ignores_latest_when_version_present(self) -> None:
        """Test that @envy version in manifest takes precedence over latest file."""
        bootstrap = self._setup_test_project("simple.lua")
        cache_dir = self._temp_dir / "cache"

        # Pre-populate latest pointing to a different version
        (cache_dir / "envy").mkdir(parents=True, exist_ok=True)
        (cache_dir / "envy" / "latest").write_text("7.7.7")

        # Pre-populate the cache binary at the manifest version (1.2.3)
        cached_binary = (
            cache_dir
            / "envy"
            / "1.2.3"
            / ("envy.exe" if sys.platform == "win32" else "envy")
        )
        cached_binary.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(self._envy_binary, cached_binary)
        if sys.platform != "win32":
            cached_binary.chmod(cached_binary.stat().st_mode | stat.S_IXUSR)

        result = self._run_bootstrap(bootstrap, ["version"], cache_dir)
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertNotIn("Downloading", result.stderr)
        self.assertIn("envy version", result.stderr)

    def test_bootstrap_falls_through_stale_latest(self) -> None:
        """Test that bootstrap falls through when latest points to missing binary."""
        bootstrap = self._setup_test_project(
            "missing_version.lua", fallback_version="9.9.9"
        )
        cache_dir = self._temp_dir / "cache"

        # Write latest pointing to a version whose binary doesn't exist
        (cache_dir / "envy").mkdir(parents=True, exist_ok=True)
        (cache_dir / "envy" / "latest").write_text("0.0.1")

        result = self._run_bootstrap(bootstrap, ["version"], cache_dir)
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        # Should have fallen through and downloaded
        self.assertIn("Downloading", result.stderr)
        self.assertIn("envy version", result.stderr)

    def test_bootstrap_parses_version_with_escapes(self) -> None:
        """Test that bootstrap correctly parses version with escaped characters."""
        bootstrap = self._setup_test_project("with_escapes.lua")
        result = self._run_bootstrap(bootstrap, ["version"])

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn("envy version", result.stderr)

    def test_bootstrap_requests_correct_architecture(self) -> None:
        """Test that bootstrap constructs the download URL with the correct arch."""
        bootstrap = self._setup_test_project("simple.lua")
        self._server.request_paths.clear()
        result = self._run_bootstrap(bootstrap, ["version"])

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")

        expected = f"/v1.2.3/envy-{_OS_NAME}-{_ARCH}{_EXT}"
        self.assertEqual(1, len(self._server.request_paths))
        self.assertEqual(expected, self._server.request_paths[0])

    # --- where the header ends ---------------------------------------------------------
    #
    # The launchers read the same header parse_envy_meta does: comments and blank lines, up
    # to the first line of code, with no line cap. The cap they used to apply cut both ways
    # -- it dropped directives under a long preamble and honored directive-shaped comments
    # sitting in the body -- and either way the launcher resolved a version or a mirror the
    # binary it execs would not.

    def test_bootstrap_ignores_a_version_below_the_first_code_line(self) -> None:
        bootstrap = self._setup_test_project("below_first_code_line.lua")
        self._server.request_paths.clear()
        result = self._run_bootstrap(bootstrap, ["version"])

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(
            [f"/v1.2.3/envy-{_OS_NAME}-{_ARCH}{_EXT}"], self._archive_requests()
        )

    def test_bootstrap_ignores_a_mirror_below_the_first_code_line(self) -> None:
        """The body directive names an unroutable mirror, so honoring it is visible.

        With ENVY_MIRROR dropped, resolution falls to DEFAULT_MIRROR, which the fixture
        stamps at the redirect stub's 404 path -- so the failure names the URL that was
        actually used, and a body comment steering the bootstrap fetch cannot hide.
        """
        bootstrap = self._setup_test_project("below_first_code_line.lua")
        result = self._run_bootstrap(bootstrap, ["version"], set_mirror=False)

        self.assertNotEqual(0, result.returncode, f"stdout: {result.stdout}")
        self.assertIn("upstream-not-a-mirror", result.stderr)
        self.assertNotIn("127.0.0.1:1/", result.stderr)

    def test_bootstrap_parses_an_indented_directive(self) -> None:
        bootstrap = self._setup_test_project(
            "indented_directives.lua", fallback_version="9.9.9"
        )
        self._server.request_paths.clear()
        result = self._run_bootstrap(bootstrap, ["version"])

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(
            [f"/v3.2.1/envy-{_OS_NAME}-{_ARCH}{_EXT}"], self._archive_requests()
        )

    def test_bootstrap_parses_a_directive_past_the_twentieth_line(self) -> None:
        bootstrap = self._setup_test_project(
            "long_preamble.lua", fallback_version="9.9.9"
        )
        self._server.request_paths.clear()
        result = self._run_bootstrap(bootstrap, ["version"])

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(
            [f"/v4.5.6/envy-{_OS_NAME}-{_ARCH}{_EXT}"], self._archive_requests()
        )

    def test_bootstrap_parses_a_crlf_manifest(self) -> None:
        """A CRLF checkout must not carry `\\r` into the version or end the header early."""
        bootstrap = self._setup_test_project(
            "crlf_directives.lua", fallback_version="9.9.9"
        )
        self._server.request_paths.clear()
        result = self._run_bootstrap(bootstrap, ["version"])

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(
            [f"/v5.4.3/envy-{_OS_NAME}-{_ARCH}{_EXT}"], self._archive_requests()
        )

    def test_bootstrap_ends_the_header_at_a_semicolon_led_line(self) -> None:
        """Lua's empty statement is code, so the version under it is not a directive.

        A distinct fallback from either directive, so a scan that took neither is not
        mistaken for one that stopped in the right place.
        """
        bootstrap = self._setup_test_project(
            "semicolon_ends_header.lua", fallback_version="8.8.8"
        )
        self._server.request_paths.clear()
        result = self._run_bootstrap(bootstrap, ["version"])

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(
            [f"/v1.2.3/envy-{_OS_NAME}-{_ARCH}{_EXT}"], self._archive_requests()
        )

    def test_bootstrap_parses_a_manifest_that_is_only_a_header(self) -> None:
        """No code line and no trailing newline: the header runs to end of file."""
        bootstrap = self._setup_test_project("header_only.lua", fallback_version="9.9.9")
        self._server.request_paths.clear()
        result = self._run_bootstrap(bootstrap, ["version"])

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(
            [f"/v6.5.4/envy-{_OS_NAME}-{_ARCH}{_EXT}"], self._archive_requests()
        )

    def test_bootstrap_ends_the_header_at_a_block_comment_continuation(self) -> None:
        """`  still inside it ]]` starts no `--`, so it ends the header like any code line.

        The directive under it is therefore not one, and resolution falls through to the
        stamped fallback -- which is what the binary does with the same bytes.
        """
        bootstrap = self._setup_test_project(
            "block_comment_ends_header.lua", fallback_version="1.2.3"
        )
        self._server.request_paths.clear()
        result = self._run_bootstrap(bootstrap, ["version"])

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(
            [f"/v1.2.3/envy-{_OS_NAME}-{_ARCH}{_EXT}"], self._archive_requests()
        )

    def test_bootstrap_reads_a_directive_inside_a_block_comment(self) -> None:
        """The line starts with `--`, so both parsers take it. Pinned, not endorsed."""
        bootstrap = self._setup_test_project(
            "directive_inside_block_comment.lua", fallback_version="9.9.9"
        )
        self._server.request_paths.clear()
        result = self._run_bootstrap(bootstrap, ["version"])

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(
            [f"/v7.6.5/envy-{_OS_NAME}-{_ARCH}{_EXT}"], self._archive_requests()
        )

    # --- version resolution over the network ---------------------------------------
    #
    # Priority: `@envy version` > $CACHE/envy/latest > $MIRROR/latest > the GitHub redirect
    # > FALLBACK_VERSION. Only the two network tiers can resolve a string that is not a
    # version, and a `v<that>/` URL 404s at download time -- reported as a 403 by any mirror
    # bucket without s3:ListBucket.

    def _archive_requests(self) -> list[str]:
        return [p for p in self._server.request_paths if p.endswith(_EXT)]

    def test_bootstrap_follows_a_multi_hop_releases_latest_redirect(self) -> None:
        """The tag lives at the end of the chain, not in the first Location header.

        Renaming a repo or moving it between orgs makes GitHub answer /releases/latest with
        a redirect to the *new* /releases/latest, so hop 1's trailing path segment is the
        literal string `latest`. Reading it produced a `vlatest/` download URL.
        """
        bootstrap = self._setup_test_project(
            "missing_version.lua",
            fallback_version="9.9.9",
            latest_url=self._github.entry_url,
        )
        result = self._run_bootstrap(bootstrap, ["version"])

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(
            [f"/v4.5.6/envy-{_OS_NAME}-{_ARCH}{_EXT}"], self._archive_requests()
        )
        # Every hop, so a script that stopped early cannot pass by coincidence.
        self.assertEqual(self._github.paths, self._github.requested)

    def test_bootstrap_rejects_a_redirect_that_never_reaches_a_tag(self) -> None:
        """A chain ending somewhere other than /releases/tag/vX.Y.Z resolves nothing."""
        stub = RedirectChainServer(["/envy-package-manager/envy/releases/latest"])
        stub.start()
        self.addCleanup(stub.stop)

        bootstrap = self._setup_test_project(
            "missing_version.lua", fallback_version="9.9.9", latest_url=stub.entry_url
        )
        result = self._run_bootstrap(bootstrap, ["version"])

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn("implausible envy version 'latest'", result.stderr)
        self.assertEqual(
            [f"/v9.9.9/envy-{_OS_NAME}-{_ARCH}{_EXT}"], self._archive_requests()
        )

    def test_bootstrap_rejects_a_nonsense_mirror_latest(self) -> None:
        """A mirror `latest` holding something that is not a version is not a version."""
        self._server.latest_body = b"latest"
        bootstrap = self._setup_test_project(
            "missing_version.lua", fallback_version="9.9.9"
        )
        result = self._run_bootstrap(bootstrap, ["version"])

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn("implausible envy version 'latest'", result.stderr)
        self.assertEqual(
            [f"/v9.9.9/envy-{_OS_NAME}-{_ARCH}{_EXT}"], self._archive_requests()
        )

    def test_bootstrap_resolves_from_an_https_mirror_latest(self) -> None:
        """An https mirror answers for itself, so github is never consulted.

        The body carries no trailing newline, matching what `envy mirror-envy` publishes.
        """
        self._server.latest_body = b"4.4.4"
        bootstrap = self._setup_test_project(
            "missing_version.lua",
            fallback_version="9.9.9",
            latest_url=self._github.entry_url,
        )
        result = self._run_bootstrap(bootstrap, ["version"])

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(
            [f"/v4.4.4/envy-{_OS_NAME}-{_ARCH}{_EXT}"], self._archive_requests()
        )
        self.assertEqual([], self._github.requested)

    def test_bootstrap_with_a_pinned_version_never_contacts_github(self) -> None:
        """`@envy version` resolves with no network at all beyond the download itself."""
        bootstrap = self._setup_test_project(
            "simple.lua", latest_url=self._github.entry_url
        )
        result = self._run_bootstrap(bootstrap, ["version"])

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(
            [f"/v1.2.3/envy-{_OS_NAME}-{_ARCH}{_EXT}"], self._server.request_paths
        )
        self.assertEqual([], self._github.requested)

    # --- s3:// mirrors ------------------------------------------------------------

    def _run_s3_bootstrap(
        self,
        manifest: str,
        *,
        version: str = "5.6.7",
        prefix: str = "releases",
        seed: bool = True,
        extra_env: dict[str, str] | None = None,
    ) -> tuple[subprocess.CompletedProcess[str], Path]:
        bindir, s3root, logfile = self._install_mock_aws()
        if seed:
            self._seed_s3_release(s3root, prefix, version)

        project_dir = self._temp_dir / "project"
        bin_dir = project_dir / "tools"
        bin_dir.mkdir(parents=True)
        self._write_verbatim(project_dir / "envy.lua", manifest)

        dest = self._stamp_bootstrap(
            bin_dir / ("envy.bat" if sys.platform == "win32" else "envy"), "0.0.1"
        )

        env = self._mock_aws_env(bindir, s3root, logfile)
        if extra_env:
            env.update(extra_env)
        result = self._run_bootstrap(dest, ["version"], env_overrides=env, set_mirror=False)
        return result, logfile

    def _mock_log(self, logfile: Path) -> str:
        # Asserted separately from its contents: a missing log means PATH injection failed
        # and a real aws ran, which is a different bug from a wrong object key.
        self.assertTrue(
            logfile.exists(),
            "mock aws was never invoked -- PATH injection failed and a real aws CLI may "
            "have run",
        )
        return logfile.read_text()

    def test_bootstrap_s3_mirror_downloads_via_aws_cli(self) -> None:
        """An s3:// mirror shells out to aws, never to curl."""
        manifest = (
            '-- @envy version "5.6.7"\n'
            '-- @envy mirror "s3://fake-bucket/releases"\n\nPACKAGES = {}\n'
        )
        result, logfile = self._run_s3_bootstrap(manifest)
        log = self._mock_log(logfile)

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}\nlog: {log}")
        self.assertIn("envy version", result.stderr)
        self.assertIn(
            f"s3://fake-bucket/releases/v5.6.7/envy-{_OS_NAME}-{_ARCH}{_EXT}", log
        )
        # Proves the http branch was not taken as well.
        self.assertEqual([], self._server.request_paths)

    def test_bootstrap_s3_mirror_resolves_version_from_mirror_latest(self) -> None:
        """With no @envy version, the mirror's own `latest` answers -- not github.com."""
        manifest = (
            '-- @envy mirror "s3://fake-bucket/releases"\n\nPACKAGES = {}\n'
        )
        result, logfile = self._run_s3_bootstrap(manifest)
        log = self._mock_log(logfile)

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}\nlog: {log}")
        self.assertIn("s3://fake-bucket/releases/latest", log)
        self.assertIn(
            f"s3://fake-bucket/releases/v5.6.7/envy-{_OS_NAME}-{_ARCH}{_EXT}", log
        )
        self.assertNotIn("0.0.1", log)  # the stamped fallback was not used

    def test_bootstrap_s3_mirror_bucket_root_prefix(self) -> None:
        """A bucket-root mirror produces keys with no leading prefix and no double slash."""
        manifest = (
            '-- @envy version "5.6.7"\n'
            '-- @envy mirror "s3://fake-bucket"\n\nPACKAGES = {}\n'
        )
        result, logfile = self._run_s3_bootstrap(manifest, prefix="")
        log = self._mock_log(logfile)

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}\nlog: {log}")
        self.assertIn(f"s3://fake-bucket/v5.6.7/envy-{_OS_NAME}-{_ARCH}{_EXT}", log)
        self.assertNotIn("//v5.6.7", log)

    def test_bootstrap_s3_mirror_trailing_slash_does_not_double(self) -> None:
        """A trailing slash on the mirror must not mint a distinct //-containing key."""
        manifest = (
            '-- @envy version "5.6.7"\n'
            '-- @envy mirror "s3://fake-bucket/releases/"\n\nPACKAGES = {}\n'
        )
        result, logfile = self._run_s3_bootstrap(manifest)
        log = self._mock_log(logfile)

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}\nlog: {log}")
        self.assertNotIn("releases//", log)

    def test_bootstrap_s3_mirror_missing_object_fails_clearly(self) -> None:
        """A missing object reports the URL rather than falling through to exec."""
        manifest = (
            '-- @envy version "9.9.9"\n'
            '-- @envy mirror "s3://fake-bucket/releases"\n\nPACKAGES = {}\n'
        )
        result, logfile = self._run_s3_bootstrap(manifest, seed=False)
        self._mock_log(logfile)

        self.assertNotEqual(0, result.returncode)
        self.assertIn("Failed to download envy", result.stderr)

    def test_bootstrap_env_mirror_overrides_manifest_mirror(self) -> None:
        """ENVY_MIRROR beats @envy mirror, matching the runtime resolver in reexec.cpp."""
        bindir, s3root, logfile = self._install_mock_aws()
        project_dir = self._temp_dir / "project"
        bin_dir = project_dir / "tools"
        bin_dir.mkdir(parents=True)
        # The manifest points at a bucket the mock cannot serve; the env var points at the
        # http server. If the manifest won, aws would be invoked and the run would fail.
        self._write_verbatim(
            project_dir / "envy.lua",
            '-- @envy version "1.2.3"\n'
            '-- @envy mirror "s3://wrong-bucket/nope"\n\nPACKAGES = {}\n',
        )
        dest = self._stamp_bootstrap(
            bin_dir / ("envy.bat" if sys.platform == "win32" else "envy")
        )

        result = self._run_bootstrap(
            dest, ["version"], env_overrides=self._mock_aws_env(bindir, s3root, logfile)
        )

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn("envy version", result.stderr)
        self.assertFalse(
            logfile.exists(),
            f"aws was invoked, so the manifest mirror won over ENVY_MIRROR: "
            f"{logfile.read_text() if logfile.exists() else ''}",
        )
        self.assertNotEqual([], self._server.request_paths)

    @unittest.skipIf(
        sys.platform == "win32", "PATH minimization to exclude a real aws.exe is fragile"
    )
    def test_bootstrap_s3_mirror_without_aws_reports_missing_cli(self) -> None:
        """s3:// without the AWS CLI must say so, not fail obscurely."""
        project_dir = self._temp_dir / "project"
        bin_dir = project_dir / "tools"
        bin_dir.mkdir(parents=True)
        self._write_verbatim(
            project_dir / "envy.lua",
            '-- @envy version "1.2.3"\n'
            '-- @envy mirror "s3://fake-bucket/releases"\n\nPACKAGES = {}\n',
        )
        dest = self._stamp_bootstrap(bin_dir / "envy")

        # AWS CLI v2 installs to /usr/local/bin, so a minimal PATH excludes it while still
        # providing the coreutils the script needs.
        result = self._run_bootstrap(
            dest, ["version"], env_overrides={"PATH": "/usr/bin:/bin"}, set_mirror=False
        )

        self.assertNotEqual(0, result.returncode)
        self.assertIn("aws CLI was not found", result.stderr)

    def test_bootstrap_fails_without_manifest(self) -> None:
        """Test that bootstrap fails gracefully when envy.lua is not found."""
        project_dir = self._temp_dir / "no-manifest"
        bin_dir = project_dir / "tools"
        project_dir.mkdir(parents=True)
        bin_dir.mkdir(parents=True)

        bootstrap_dest = self._stamp_bootstrap(
            bin_dir / ("envy.bat" if sys.platform == "win32" else "envy"), "1.0.0"
        )

        env = os.environ.copy()
        env["ENVY_MIRROR"] = f"http://127.0.0.1:{self._port}"
        env["ENVY_CACHE_ROOT"] = str(self._temp_dir / "cache")

        if sys.platform == "win32":
            cmd = ["cmd.exe", "/c", str(bootstrap_dest), "version"]
        else:
            cmd = [str(bootstrap_dest), "version"]

        result = test_config.run(
            cmd, capture_output=True, text=True, env=env, cwd=project_dir, timeout=30
        )

        self.assertNotEqual(0, result.returncode)
        self.assertIn("envy.lua", result.stderr.lower())

    # --- attestation (@envy sha256sums) ------------------------------------------
    #
    # The chain: the manifest pins SHA256SUMS's own hash, SHA256SUMS names the archive's
    # hash, the archive is what gets executed. Break any link and the bootstrap must refuse
    # to exec rather than degrade to an unverified download.

    def _setup_attested_project(
        self,
        pin: str | None,
        version: str | None = "1.2.3",
    ) -> Path:
        """Write a manifest with an optional sums pin, plus the bootstrap script."""
        project_dir = self._temp_dir / "project"
        bin_dir = project_dir / "tools"
        bin_dir.mkdir(parents=True, exist_ok=True)

        lines = []
        if version is not None:
            lines.append(f'-- @envy version "{version}"')
        if pin is not None:
            lines.append(f'-- @envy sha256sums "{pin}"')
        self._write_verbatim(
            project_dir / "envy.lua", "\n".join(lines) + "\n\nPACKAGES = {}\n"
        )

        return self._stamp_bootstrap(
            bin_dir / ("envy.bat" if sys.platform == "win32" else "envy")
        )

    def test_bootstrap_attests_a_matching_archive(self) -> None:
        bootstrap = self._setup_attested_project(self._server.sums_pin)
        result = self._run_bootstrap(bootstrap, ["version"])

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn("envy version", result.stderr)
        # Proves the sums file was actually consulted, not that verification was skipped.
        self.assertTrue(
            any(p.endswith("SHA256SUMS") for p in self._server.request_paths),
            f"SHA256SUMS was never fetched: {self._server.request_paths}",
        )

    def test_bootstrap_rejects_a_tampered_archive(self) -> None:
        """Mirror serves modified archive bytes but an untouched SHA256SUMS."""
        self._server.corrupt_archive = True
        bootstrap = self._setup_attested_project(self._server.sums_pin)

        result = self._run_bootstrap(bootstrap, ["version"])

        self.assertNotEqual(0, result.returncode)
        self.assertIn("attestation", result.stderr.lower())
        # The whole point: nothing ran. A corrupted archive that still extracted and exec'd
        # would make the check decorative.
        self.assertNotIn("envy version", result.stderr)

    def test_bootstrap_rejects_a_tampered_sums_file(self) -> None:
        """Mirror rewrites the archive *and* SHA256SUMS; only the manifest pin catches it."""
        pin = self._server.sums_pin  # captured before the mirror is rewritten
        self._server.corrupt_archive = True
        corrupted = self._server.pristine_archive + b"corrupted"
        digest = hashlib.sha256(corrupted).hexdigest()
        self._server.sums_body = (
            f"{digest}  {self._server.host_archive_name}\n".encode()
        )

        bootstrap = self._setup_attested_project(pin)
        result = self._run_bootstrap(bootstrap, ["version"])

        self.assertNotEqual(0, result.returncode)
        self.assertIn("sha256sums", result.stderr.lower())
        self.assertNotIn("envy version", result.stderr)

    def test_bootstrap_rejects_sums_without_an_entry_for_this_platform(self) -> None:
        self._server.sums_body = (
            f"{'a' * 64}  envy-some-other-platform.tar.gz\n".encode()
        )
        bootstrap = self._setup_attested_project(self._server.sums_pin)

        result = self._run_bootstrap(bootstrap, ["version"])

        self.assertNotEqual(0, result.returncode)
        self.assertIn("no entry", result.stderr.lower())

    def test_bootstrap_fails_when_pinned_sums_are_unavailable(self) -> None:
        """A mirror without SHA256SUMS cannot satisfy a pin, so the run must stop."""
        self._server.serve_sums = False
        bootstrap = self._setup_attested_project(self._server.sums_pin)

        result = self._run_bootstrap(bootstrap, ["version"])

        self.assertNotEqual(0, result.returncode)
        self.assertIn("SHA256SUMS", result.stderr)
        self.assertNotIn("envy version", result.stderr)

    def test_bootstrap_rejects_a_pin_without_a_pinned_version(self) -> None:
        """A sums pin names one release, so a dynamically resolved version cannot use it.

        Fails before any network traffic: silently skipping verification would be worse
        than having no pin, because the manifest still advertises attestation.
        """
        bootstrap = self._setup_attested_project(self._server.sums_pin, version=None)

        result = self._run_bootstrap(bootstrap, ["version"])

        self.assertNotEqual(0, result.returncode)
        self.assertIn("@envy version", result.stderr)
        self.assertEqual([], self._server.request_paths)

    def test_bootstrap_without_a_pin_does_not_fetch_sums(self) -> None:
        """Attestation is opt-in: an unpinned manifest keeps working, and pays nothing."""
        self._server.serve_sums = False  # would 404 if the script asked for it
        bootstrap = self._setup_attested_project(None)

        result = self._run_bootstrap(bootstrap, ["version"])

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn("envy version", result.stderr)
        self.assertFalse(
            any(p.endswith("SHA256SUMS") for p in self._server.request_paths),
            f"unpinned bootstrap fetched SHA256SUMS: {self._server.request_paths}",
        )

    def test_bootstrap_accepts_an_uppercase_pin(self) -> None:
        """certutil and Get-FileHash emit uppercase, so a hand-pasted pin often is."""
        bootstrap = self._setup_attested_project(self._server.sums_pin.upper())
        result = self._run_bootstrap(bootstrap, ["version"])

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn("envy version", result.stderr)

    # --- borrowing an envy binary from the user-wide cache --------------------------
    # A local cache constrains *writes*: a local tree reads the user-wide one for a binary.

    _LOCAL_CACHE_BIN = "tools"

    def _local_cache_project(
        self,
        name: str,
        version: str = "1.2.3",
        extra_directives: str = "",
        markers: tuple[str, ...] = (),
        **stamp_kwargs,
    ) -> tuple[Path, Path]:
        """A `cache-local` project and its launcher. Returns (project_dir, script)."""
        project = self._temp_dir / name
        bin_dir = project / self._LOCAL_CACHE_BIN
        bin_dir.mkdir(parents=True)
        header = f'-- @envy version "{version}"\n' if version else ""
        self._write_verbatim(
            project / "envy.lua",
            header
            + f'-- @envy bin "{self._LOCAL_CACHE_BIN}"\n'
            + '-- @envy cache-local "out/.envy"\n'
            + extra_directives
            + "PACKAGES = {}\n",
        )
        for marker in markers:
            self._write_verbatim(project / marker, "")
        script = self._stamp_bootstrap(
            bin_dir / ("envy.bat" if sys.platform == "win32" else "envy"),
            version or "1.2.3",
            **stamp_kwargs,
        )
        return project, script

    def _sandbox_user_wide_root(self) -> Path:
        """The user-wide root `_run_bootstrap(set_cache_root=False)` resolves to."""
        return test_config.sandbox_user_wide_root(
            test_config.sandbox_home_env(self._temp_dir / "home")
        )

    def _run_local(self, script: Path, args: list[str], **kwargs):
        return self._run_bootstrap(script, args, set_cache_root=False, **kwargs)

    def test_cache_shared_borrows_an_existing_envy_and_leaves_no_local_tree(self) -> None:
        """The reported case: opting a fresh clone out of its local cache costs nothing.

        The launcher used to miss the empty local tree, download 20 MB, self-deploy it into
        the very directory the user was abandoning, and then download it a second time on
        the next command because the shared tree still lacked it.
        """
        project, script = self._local_cache_project("optout")
        test_config.seed_cached_envy(self._sandbox_user_wide_root(), "1.2.3")

        result = self._run_local(script, ["cache", "--shared"])

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual([], self._archive_requests(), "downloaded an envy it already had")
        self.assertNotIn("Downloading envy", result.stderr)
        self.assertFalse(
            (project / "out").exists(),
            "opting out of the local cache created the local cache",
        )
        self.assertTrue((project / ".envy-cache-shared").is_file())

    def test_cache_shared_with_a_new_version_lands_in_the_user_wide_tree(self) -> None:
        """One download, into the tree the project is about to start using."""
        project, script = self._local_cache_project("optout-newver", version="2.5.0")
        user_wide = self._sandbox_user_wide_root()
        test_config.seed_cached_envy(user_wide, "1.2.3")  # some other version

        result = self._run_local(script, ["cache", "--shared"])

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(1, len(self._archive_requests()), self._server.request_paths)
        deployed = user_wide / "envy" / test_config.get_envy_version()
        self.assertTrue(deployed.is_dir(), f"nothing deployed under {user_wide}")
        self.assertFalse((project / "out").exists())

    def test_a_local_project_self_deploys_from_the_borrowed_binary(self) -> None:
        """Borrowing is read-only; the local tree still ends up self-contained.

        This is what keeps a populated `out/.envy` tarball runnable on a box with no
        user-wide cache at all.
        """
        project, script = self._local_cache_project("borrow")
        test_config.seed_cached_envy(self._sandbox_user_wide_root(), "1.2.3")

        result = self._run_local(script, ["cache", "--root"])

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual([], self._archive_requests())
        local_deploy = (
            project / "out" / ".envy" / "envy" / test_config.get_envy_version()
        )
        self.assertTrue(local_deploy.is_dir(), f"expected a deploy at {local_deploy}")

    def test_a_sums_pin_fails_closed_and_downloads(self) -> None:
        """A pinned project must not run bytes it never attested.

        The cache fast path never re-hashes, and the user-wide tree is written by every
        other project on the box -- so the pin, not the tree, is the trust boundary.
        """
        _, script = self._local_cache_project(
            "pinned",
            extra_directives=f'-- @envy sha256sums "{self._server.sums_pin}"\n',
        )
        test_config.seed_cached_envy(self._sandbox_user_wide_root(), "1.2.3")

        result = self._run_local(script, ["cache", "--root"])

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(1, len(self._archive_requests()), self._server.request_paths)
        self.assertTrue(
            any(p.endswith("SHA256SUMS") for p in self._server.request_paths),
            f"pinned bootstrap skipped attestation: {self._server.request_paths}",
        )

    def test_an_override_names_exactly_one_tree(self) -> None:
        """`ENVY_CACHE_ROOT` must not be quietly widened to two trees."""
        _, script = self._local_cache_project("override")
        test_config.seed_cached_envy(self._sandbox_user_wide_root(), "1.2.3")
        empty = self._temp_dir / "empty-override"
        empty.mkdir()

        result = self._run_bootstrap(script, ["cache", "--root"], cache_dir=empty)

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(1, len(self._archive_requests()), self._server.request_paths)

    def test_a_corrupt_local_binary_falls_through_instead_of_aborting(self) -> None:
        """`[[ -x ]]` is true for a directory and for a truncated file that kept the bit.

        Testing it alone handed both to exec, which fails with 126 and takes the whole
        launcher down rather than falling through to another candidate.
        """
        for label, make in (
            ("directory", lambda p: p.mkdir(parents=True)),
            ("empty-executable", self._write_empty_executable),
        ):
            with self.subTest(corrupt=label):
                project, script = self._local_cache_project(f"corrupt-{label}")
                test_config.seed_cached_envy(self._sandbox_user_wide_root(), "1.2.3")
                bad = (
                    project
                    / "out"
                    / ".envy"
                    / "envy"
                    / "1.2.3"
                    / ("envy.exe" if sys.platform == "win32" else "envy")
                )
                bad.parent.mkdir(parents=True, exist_ok=True)
                make(bad)

                result = self._run_local(script, ["cache", "--root"])

                self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
                self.assertEqual([], self._archive_requests())
                self._server.request_paths.clear()

    @staticmethod
    def _write_empty_executable(path: Path) -> None:
        path.write_bytes(b"")
        if sys.platform != "win32":
            path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

    @unittest.skipIf(sys.platform == "win32", "POSIX HOME semantics")
    def test_a_local_project_runs_with_no_user_wide_root_at_all(self) -> None:
        """The tarball case: populate a local tree, move it to a box with no HOME.

        `set -u` used to abort the launcher here, because the platform default was expanded
        unconditionally rather than only where shared mode needs it.
        """
        project, script = self._local_cache_project("airgapped")
        test_config.seed_cached_envy(project / "out" / ".envy", "1.2.3")

        result = self._run_local(
            script,
            ["cache", "--root"],
            drop_env=("HOME", "XDG_CACHE_HOME", "USERPROFILE", "LOCALAPPDATA"),
        )

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual([], self._archive_requests())

    def test_a_recorded_local_mode_refuses_a_pre_marker_envy(self) -> None:
        """The guard keyed on directives alone, and a marker leaves none behind.

        `envy cache --local` puts a project on its own tree with nothing in the manifest to
        show for it, so an envy that predates the markers would read the same manifest,
        resolve the *shared* cache, and exit 0.
        """
        project = self._temp_dir / "marker-only"
        bin_dir = project / self._LOCAL_CACHE_BIN
        bin_dir.mkdir(parents=True)
        self._write_verbatim(
            project / "envy.lua",
            '-- @envy version "0.1.0"\n'
            f'-- @envy bin "{self._LOCAL_CACHE_BIN}"\n'
            "PACKAGES = {}\n",
        )
        self._write_verbatim(project / ".envy-cache-local", "")
        script = self._stamp_bootstrap(
            bin_dir / ("envy.bat" if sys.platform == "win32" else "envy"),
            "0.1.0",
            min_directive_version="0.2.0",
        )

        result = self._run_local(script, ["cache", "--root"])

        self.assertNotEqual(0, result.returncode)
        self.assertIn("marker", result.stderr)
        self.assertEqual([], self._archive_requests(), "refused, but downloaded anyway")

    def test_a_local_project_never_writes_to_the_user_wide_cache(self) -> None:
        """The invariant, asserted rather than reasoned about.

        Every write site under a local run is supposed to land in the project's own tree.
        Shell hooks were the one that did not, and a snapshot is what catches the next one.
        """
        project, script = self._local_cache_project("invariant")
        user_wide = self._sandbox_user_wide_root()
        test_config.seed_cached_envy(user_wide, "1.2.3")

        def snapshot() -> dict[str, tuple[int, int]]:
            return {
                str(p.relative_to(user_wide)): (p.stat().st_size, p.stat().st_mtime_ns)
                for p in sorted(user_wide.rglob("*"))
                if p.is_file()
            }

        before = snapshot()
        for args in (["cache", "--root"], ["cache"], ["shell", "zsh"], ["--version"]):
            with self.subTest(args=args):
                # Exit code is not the point -- `shell zsh` legitimately fails when no hook
                # has ever been written. Touching this tree is the point.
                self._run_local(script, args)
                self.assertEqual(before, snapshot(), f"{args} wrote to {user_wide}")

        self.assertFalse((user_wide / "shell").exists(), "hooks written from a local tree")
        self.assertFalse(
            (project / "out" / ".envy" / "shell").exists(),
            "hooks written into the project tree",
        )


if __name__ == "__main__":
    unittest.main()
