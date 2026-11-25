#!/usr/bin/env python3
"""Build script for envy project."""

import argparse
import os
import platform
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile
from pathlib import Path

CMAKE_VERSION = "4.1.2"
NINJA_VERSION = "1.13.2"

CMAKE_BASE = f"https://github.com/Kitware/CMake/releases/download/v{CMAKE_VERSION}/cmake-{CMAKE_VERSION}-"
CMAKE_SUFFIXES = {
    ("Darwin", "arm64"): "macos-universal.tar.gz",
    ("Darwin", "x86_64"): "macos-universal.tar.gz",
    ("Linux", "x86_64"): "linux-x86_64.tar.gz",
    ("Linux", "aarch64"): "linux-aarch64.tar.gz",
    ("Windows", "AMD64"): "windows-x86_64.zip",
    ("Windows", "ARM64"): "windows-arm64.zip",
}

NINJA_BASE = (
    f"https://github.com/ninja-build/ninja/releases/download/v{NINJA_VERSION}/ninja-"
)
NINJA_SUFFIXES = {
    ("Darwin", "arm64"): "mac.zip",
    ("Darwin", "x86_64"): "mac.zip",
    ("Linux", "x86_64"): "linux.zip",
    ("Linux", "aarch64"): "linux-aarch64.zip",
    ("Windows", "AMD64"): "win.zip",
    ("Windows", "ARM64"): "winarm64.zip",
}


def remove_quarantine(path: Path) -> None:
    """Remove macOS quarantine attribute from file or directory."""
    if platform.system() != "Darwin":
        return
    try:
        subprocess.run(
            ["xattr", "-dr", "com.apple.quarantine", str(path)], capture_output=True
        )
    except FileNotFoundError:
        pass


def download_cmake(cache_dir: Path) -> Path:
    """Download and extract cmake, return path to cmake binary."""
    cmake_dir = cache_dir / "cmake"
    system, machine = platform.system(), platform.machine()

    if system == "Windows":
        cmake_bin = cmake_dir / "bin" / "cmake.exe"
    elif system == "Darwin":
        cmake_bin = cmake_dir / "CMake.app" / "Contents" / "bin" / "cmake"
    else:
        cmake_bin = cmake_dir / "bin" / "cmake"

    if cmake_bin.exists():
        return cmake_bin

    suffix = CMAKE_SUFFIXES.get((system, machine))
    url = f"{CMAKE_BASE}{suffix}" if suffix else None
    if not url:
        print(f"No cmake download available for {system}/{machine}, using system cmake")
        return Path(shutil.which("cmake") or "cmake")

    print(f"Downloading cmake {CMAKE_VERSION}...")
    cmake_dir.mkdir(parents=True, exist_ok=True)

    with tempfile.NamedTemporaryFile(
        delete=False, suffix=".tar.gz" if url.endswith(".tar.gz") else ".zip"
    ) as tmp:
        tmp_path = Path(tmp.name)

    try:
        urllib.request.urlretrieve(url, tmp_path)

        if url.endswith(".tar.gz"):
            with tarfile.open(tmp_path, "r:gz") as tar:
                members = tar.getmembers()
                prefix = members[0].name.split("/")[0] if members else ""
                for member in members:
                    member.name = (
                        "/".join(member.name.split("/")[1:])
                        if "/" in member.name
                        else member.name
                    )
                    if member.name:
                        tar.extract(member, cmake_dir)
        else:
            with zipfile.ZipFile(tmp_path, "r") as zf:
                names = zf.namelist()
                prefix = names[0].split("/")[0] if names else ""
                for name in names:
                    new_name = "/".join(name.split("/")[1:]) if "/" in name else name
                    if new_name and not name.endswith("/"):
                        target = cmake_dir / new_name
                        target.parent.mkdir(parents=True, exist_ok=True)
                        with zf.open(name) as src, open(target, "wb") as dst:
                            dst.write(src.read())
    finally:
        tmp_path.unlink(missing_ok=True)

    remove_quarantine(cmake_dir)
    if not cmake_bin.exists():
        print(f"ERROR: cmake binary not found at {cmake_bin}")
        sys.exit(1)

    return cmake_bin


def download_ninja(cache_dir: Path) -> Path:
    """Download and extract ninja, return path to ninja binary."""
    ninja_dir = cache_dir / "ninja"
    system, machine = platform.system(), platform.machine()

    ninja_bin = ninja_dir / ("ninja.exe" if system == "Windows" else "ninja")

    if ninja_bin.exists():
        return ninja_bin

    suffix = NINJA_SUFFIXES.get((system, machine))
    url = f"{NINJA_BASE}{suffix}" if suffix else None
    if not url:
        print(f"No ninja download available for {system}/{machine}, using system ninja")
        return Path(shutil.which("ninja") or "ninja")

    print(f"Downloading ninja {NINJA_VERSION}...")
    ninja_dir.mkdir(parents=True, exist_ok=True)

    with tempfile.NamedTemporaryFile(delete=False, suffix=".zip") as tmp:
        tmp_path = Path(tmp.name)

    try:
        urllib.request.urlretrieve(url, tmp_path)
        with zipfile.ZipFile(tmp_path, "r") as zf:
            zf.extractall(ninja_dir)
    finally:
        tmp_path.unlink(missing_ok=True)

    remove_quarantine(ninja_dir)
    ninja_bin.chmod(ninja_bin.stat().st_mode | 0o111)

    if not ninja_bin.exists():
        print(f"ERROR: ninja binary not found at {ninja_bin}")
        sys.exit(1)

    return ninja_bin


def main():
    parser = argparse.ArgumentParser(description="Build envy project")
    parser.add_argument(
        "--sanitizer",
        choices=["none", "asan", "ubsan", "tsan"],
        default="none",
        help="Sanitizer to use (default: none)",
    )
    args, extra_cmake_args = parser.parse_known_args()

    # Check for cl.exe on Windows
    if sys.platform == "win32":
        if shutil.which("cl") is None:
            print(
                "cl.exe not found on PATH. Please launch from a VS Developer Command Prompt or run vcvars64.bat first."
            )
            return 1

    script_dir = Path(__file__).parent.resolve()
    root_dir = script_dir.parent

    cache_dir = root_dir / "out" / "cache"
    build_dir = root_dir / "out" / "build"
    cache_file = build_dir / "CMakeCache.txt"
    state_file = build_dir / ".envy-build-state"

    # Ensure cache directory exists
    cache_dir.mkdir(parents=True, exist_ok=True)

    # Download cmake and ninja
    cmake_bin = os.environ.get("CMAKE") or str(download_cmake(cache_dir))
    ninja_bin = os.environ.get("NINJA") or str(download_ninja(cache_dir))

    # Build current state string for comparison
    extra_str = ",".join(extra_cmake_args) if extra_cmake_args else ""
    current_state = f"sanitizer={args.sanitizer},extra={extra_str}"

    # Check if we need to configure
    need_configure = True
    if cache_file.exists() and state_file.exists():
        cached_state = state_file.read_text().strip()
        if cached_state == current_state:
            need_configure = False

    if need_configure:
        cmake_args = [
            cmake_bin,
            "--preset",
            "release-lto-on",
            f"-DCMAKE_MAKE_PROGRAM={ninja_bin}",
            f"-DENVY_SANITIZER={args.sanitizer.upper()}",
            "--log-level=STATUS",
            *extra_cmake_args,
        ]
        result = subprocess.run(cmake_args, cwd=root_dir)
        if result.returncode != 0:
            return 1

        build_dir.mkdir(parents=True, exist_ok=True)
        state_file.write_text(current_state + "\n")

    result = subprocess.run(
        [cmake_bin, "--build", "--preset", "release-lto-on"], cwd=root_dir
    )
    if result.returncode != 0:
        print("\nBuild failed. See messages above for details.")
        return 1

    print("Build completed successfully.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
