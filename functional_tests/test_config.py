"""Shared configuration for functional tests."""

import os
import shlex
import shutil
import signal
import stat
import subprocess
import sys
import time
import unittest
from pathlib import Path

# Subprocess text mode kwargs - use UTF-8 to avoid cp1252 decode errors on Windows
SUBPROCESS_TEXT_MODE = {"text": True, "encoding": "utf-8", "errors": "replace"}


def _is_envy_cmd(cmd) -> bool:
    """Return True if cmd invokes the envy functional tester binary."""
    if not cmd:
        return False
    exe = str(cmd[0] if not isinstance(cmd, str) else cmd)
    return "envy_functional_tester" in exe


def _wrap_cmd(cmd):
    """Prepend ENVY_TEST_WRAPPER to a command list if set (envy commands only)."""
    wrapper = os.environ.get("ENVY_TEST_WRAPPER")
    if wrapper and _is_envy_cmd(cmd):
        return shlex.split(wrapper) + list(cmd)
    return cmd


def run(*args, **kwargs) -> subprocess.CompletedProcess[str]:
    """Wrapper for subprocess.run with UTF-8 encoding and optional command wrapping."""
    kwargs.setdefault("encoding", "utf-8")
    kwargs.setdefault("errors", "replace")
    if "text" not in kwargs and "encoding" in kwargs:
        kwargs["text"] = True
    args = list(args)
    if args and not kwargs.get("shell"):
        args[0] = _wrap_cmd(args[0])
    return subprocess.run(*args, **kwargs)


def describe_exit(result: subprocess.CompletedProcess) -> str:
    """Human-readable exit status: the signal name when killed, else the code."""
    rc = result.returncode
    if rc >= 0:
        return f"exit {rc}"
    try:
        return f"killed by {signal.Signals(-rc).name}"
    except ValueError:
        return f"killed by signal {-rc}"


def is_pwsh_runtime_crash(result: subprocess.CompletedProcess) -> bool:
    """True if pwsh's .NET runtime died instead of running the script to a verdict.

    pwsh intermittently dies on resource-constrained CI runners (observed on
    linux-arm64 under ASAN). Three CoreCLR failure modes seen:
    - death by signal, usually SIGSEGV, with no output whatsoever
    - "Unhandled exception ... The given assembly name was invalid" (SIGABRT)
    - a bare "Stack overflow." (SIGABRT)

    All are nondeterministic and unrelated to the behavior under test. The signal
    check is the one that generalizes: pwsh reports a script's verdict through its
    own exit code, so a genuine hook failure always terminates normally with a
    non-negative code. Death by signal therefore means the runtime went down
    without producing a verdict at all -- including the SIGSEGV case, which writes
    nothing to stderr and so is invisible to the text signatures below. A crash in
    a program the script invokes (envy, say) does not reach here: pwsh survives it
    and exits normally with a non-negative code.

    Retrying these cannot mask a real bug: a deterministic crash still fails once
    the retries are exhausted.
    """
    if result.returncode == 0:
        return False
    # POSIX-only arm: Windows reports no negative codes, so this never fires there.
    if result.returncode < 0:
        return True
    stderr = result.stderr or ""
    if "Unhandled exception" in stderr and (
        "FileLoadException" in stderr or "assembly" in stderr
    ):
        return True
    return "Stack overflow." in stderr


def run_pwsh(cmd, retries: int = 3, delay: float = 0.5, **kwargs):
    """Run a pwsh command, retrying .NET-runtime crashes.

    See is_pwsh_runtime_crash; genuine failures are returned as-is. When every
    attempt hits the runtime-crash signature, pwsh itself is broken on this
    runner (observed on some GitHub linux-x64 images where CoreCLR aborts at
    startup for every invocation) — that is environmental, not the behavior under
    test, so the test is skipped rather than failed. A real hook failure exits
    cleanly with the wrong output (never a managed-runtime abort), so it is never
    skipped here.

    The retry line reports the exit status, not just stderr: the SIGSEGV mode
    prints nothing, so a stderr-only message logged a blank reason.
    """
    last = ""
    for attempt in range(retries):
        result = run(cmd, **kwargs)
        if not is_pwsh_runtime_crash(result):
            return result
        detail = (result.stderr or "").strip()[:120]
        last = f"{describe_exit(result)}: {detail}".strip(": ")
        sys.stderr.write(
            f"pwsh runtime crash (attempt {attempt + 1}/{retries}), retrying: {last}\n"
        )
        time.sleep(delay)
    raise unittest.SkipTest(
        f"pwsh .NET runtime crashes on this runner ({retries}/{retries} attempts, "
        f"last: {last}); skipping pwsh-dependent test"
    )


def popen(*args, **kwargs) -> subprocess.Popen[str]:
    """Wrapper for subprocess.Popen with UTF-8 encoding and optional command wrapping."""
    kwargs.setdefault("encoding", "utf-8")
    kwargs.setdefault("errors", "replace")
    if "text" not in kwargs and "encoding" in kwargs:
        kwargs["text"] = True
    args = list(args)
    if args and not kwargs.get("shell"):
        args[0] = _wrap_cmd(args[0])
    return subprocess.Popen(*args, **kwargs)


# Required manifest header for all manifests (bin is mandatory)
MANIFEST_HEADER = '-- @envy bin "envy-bin"\n'

# Manifest header with deployment enabled (for sync tests)
MANIFEST_HEADER_DEPLOY = '-- @envy bin "envy-bin"\n-- @envy deploy "true"\n'


def make_manifest(packages_content: str, deploy: bool = False) -> str:
    """Create a manifest string with required headers.

    Args:
        packages_content: The PACKAGES table content (should start with 'PACKAGES = {')
        deploy: If True, include deploy directive for product script creation

    Returns:
        Complete manifest string with required bin directive.
    """
    header = MANIFEST_HEADER_DEPLOY if deploy else MANIFEST_HEADER
    return header + packages_content


def spec_entry(identity, source, setup=None, options=None, needed_by=None) -> str:
    """Render one PACKAGES entry pointing at a local spec file.

    `options` is a Lua table literal (e.g. '{ version = "1.0" }'), matching what
    a manifest author would write.
    """
    fields = [f'spec = "{identity}"', f'source = "{Path(source).as_posix()}"']
    if setup:
        fields.append("setup = {" + ", ".join(f'"{s}"' for s in setup) + "}")
    if options:
        fields.append(f"options = {options}")
    if needed_by:
        fields.append(f'needed_by = "{needed_by}"')
    return "  { " + ", ".join(fields) + " },"


def write_spec_manifest(manifest_dir, entries, deploy: bool = False) -> Path:
    """Write an envy.lua pulling in local spec files; returns its path.

    Each entry is either a rendered string from spec_entry() or an
    (identity, source) pair. This is the manifest-driven way to run specs
    through the engine via the public `envy install`, so tests need no
    test-only CLI surface.
    """
    lines = [e if isinstance(e, str) else spec_entry(*e) for e in entries]
    body = "PACKAGES = {\n" + "\n".join(lines) + "\n}\n"
    path = Path(manifest_dir) / "envy.lua"
    path.write_text(make_manifest(body, deploy=deploy), encoding="utf-8")
    return path


def parse_export_line(line):
    """Parse an export output line '<hash>  <path>' -> (hash, Path)."""
    parts = line.strip().split("  ", 1)
    if len(parts) != 2:
        raise ValueError(f"Expected '<hash>  <path>', got: {line}")
    return parts[0], Path(parts[1])


_cached_executable: dict[str, Path] = {}


def _get_executable(name: str, label: str) -> Path:
    if cached := _cached_executable.get(name):
        return cached

    root = Path(__file__).parent.parent / "out" / "build"
    exe = root / (f"{name}.exe" if sys.platform == "win32" else name)

    if not exe.exists():
        raise RuntimeError(
            f"{label} not found at {exe}. "
            "Build with: ./build.sh (or ./build.bat on Windows)"
        )

    if sys.platform != "win32" and not os.access(exe, os.X_OK):
        raise RuntimeError(f"{label} at {exe} is not executable")

    _cached_executable[name] = exe
    return exe


def get_envy_executable() -> Path:
    """Path to the functional test executable. The default for tests.

    It carries the sanitizers and the test-only subcommands, so anything that
    does not specifically need the shipped binary should use this one.
    """
    return _get_executable("envy_functional_tester", "Functional tester")


def get_envy_production_executable() -> Path:
    """Path to the shipped `envy` binary -- unsanitized, no test-only commands.

    Only for tests that are about the artifact users get: bootstrap, re-exec,
    mirroring, and anything that copies or serves the binary itself. Everything
    else should prefer get_envy_executable() for the sanitizer coverage.
    """
    return _get_executable("envy", "envy binary")


def get_test_env() -> dict[str, str]:
    """Get environment variables for running tests."""
    env = os.environ.copy()

    # Sanitizers not supported on Windows
    if sys.platform == "win32":
        return env

    root = Path(__file__).parent.parent

    # Point sanitizers to suppression files
    tsan_supp = root / "tsan.supp"
    if tsan_supp.exists():
        env.setdefault("TSAN_OPTIONS", f"suppressions={tsan_supp}")

    asan_supp = root / "asan.supp"
    if asan_supp.exists():
        env.setdefault("ASAN_OPTIONS", f"suppressions={asan_supp}")

    return env


# Cache-root sandboxing: redirect the platform default, or self-deploy writes the
# developer's real cache. Four modules grew their own copy; one now, so a new tier lands once.


def sandbox_home_env(home: Path, base: dict[str, str] | None = None) -> dict[str, str]:
    """`base` (default get_test_env()) with every default-cache variable under `home`.

    Drops ENVY_CACHE_ROOT: an override short-circuits the very tiers a sandbox exists to
    make observable. Creates `home`, since a caller that forgot left the resolution
    pointing at a directory that never appeared.
    """
    env = dict(base if base is not None else get_test_env())
    env.pop("ENVY_CACHE_ROOT", None)
    home.mkdir(parents=True, exist_ok=True)
    env["HOME"] = str(home)
    env["USERPROFILE"] = str(home)
    env["XDG_CACHE_HOME"] = str(home / "cache")
    env["LOCALAPPDATA"] = str(home / "AppData" / "Local")
    return env


def sandbox_user_wide_root(env: dict[str, str]) -> Path:
    """The user-wide cache root a sandbox_home_env() environment resolves to.

    Computed, not approximated, and per platform_posix.cpp/platform_win.cpp: a sandbox
    necessarily puts HOME inside the test tree, so "not under the test directory" is not
    the assertion available -- the exact path is, and only it holds on all three platforms.
    """
    if sys.platform == "win32":
        return Path(env["LOCALAPPDATA"]) / "envy"
    if sys.platform == "darwin":
        return Path(env["HOME"]) / "Library" / "Caches" / "envy"
    return Path(env["XDG_CACHE_HOME"]) / "envy"


def seed_cached_envy(cache_root: Path, version: str, source: Path | None = None) -> Path:
    """Put an envy binary at <cache_root>/envy/<version>/, as self-deploy would.

    The launchers and reexec look here before downloading, so this is how a test says
    "the user already has this version" without running a download to create it. `source`
    defaults to the shipped binary; re-exec tests pass the functional tester instead,
    because that is the artifact they are re-exec'ing into.
    """
    version_dir = cache_root / "envy" / version
    version_dir.mkdir(parents=True, exist_ok=True)
    binary = version_dir / ("envy.exe" if sys.platform == "win32" else "envy")
    shutil.copy2(source or get_envy_production_executable(), binary)
    if sys.platform != "win32":
        binary.chmod(binary.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    return binary


def get_envy_version() -> str:
    """The version baked into the shipped binary."""
    result = run(
        [str(get_envy_production_executable()), "version"],
        capture_output=True,
        text=True,
        timeout=10,
    )
    for line in result.stderr.splitlines():
        if line.startswith("envy version "):
            return line.split()[2]
    raise RuntimeError("Could not parse envy version from: " + result.stderr)
