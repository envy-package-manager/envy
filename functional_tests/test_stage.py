"""Functional tests for engine stage phase.

Tests default extraction, declarative stage options (strip), and imperative
stage functions (ctx.extract, ctx.extract_all).
"""

import hashlib
import io
import os
import subprocess
import tarfile
from pathlib import Path
import unittest

from . import test_config
from .env import EnvyTestCase

# Test archive contents - standard nested directory structure
TEST_ARCHIVE_FILES = {
    "root/file1.txt": "Root file content\n",
    "root/file2.txt": "Another root file\n",
    "root/subdir1/file3.txt": "Subdir file content\n",
    "root/subdir1/subdir2/file4.txt": "Deep nested file\n",
    "root/subdir1/subdir2/file5.txt": "Another deep file\n",
}


def create_test_archive(output_path: Path) -> str:
    """Create test.tar.gz archive and return its SHA256 hash."""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz") as tar:
        for name, content in TEST_ARCHIVE_FILES.items():
            data = content.encode("utf-8")
            info = tarfile.TarInfo(name=name)
            info.size = len(data)
            tar.addfile(info, io.BytesIO(data))
    archive_data = buf.getvalue()
    output_path.write_bytes(archive_data)
    return hashlib.sha256(archive_data).hexdigest()


class TestStagePhase(EnvyTestCase):
    """Tests for stage phase (archive extraction and preparation)."""

    def setUp(self):
        super().setUp()
        self.test_dir = self.make_temp_dir("test_dir")
        self.trace_flag = ["--trace"] if os.environ.get("ENVY_TEST_TRACE") else []

        # Create test archive and get its hash
        self.archive_path = self.test_dir / "test.tar.gz"
        self.archive_hash = create_test_archive(self.archive_path)

    def write_spec(self, name: str, content: str) -> str:
        """Write spec to temp dir with placeholder substitution, return path."""
        spec_content = content.format(
            ARCHIVE_PATH=self.archive_path.as_posix(),
            ARCHIVE_HASH=self.archive_hash,
        )
        path = self.test_dir / f"{name}.lua"
        path.write_text(spec_content, encoding="utf-8")
        return str(path)

    def get_pkg_path(self, identity):
        """Find package directory for given identity in cache."""
        pkgs_dir = self.cache_root / "packages" / identity
        if not pkgs_dir.exists():
            return None
        for subdir in pkgs_dir.iterdir():
            if subdir.is_dir():
                pkg_dir = subdir / "pkg"
                if pkg_dir.exists():
                    return pkg_dir
                return subdir
        return None

    def run_spec(self, name: str, identity: str, should_succeed: bool = True):
        """Install the named spec via a generated manifest; return result."""
        manifest = test_config.write_spec_manifest(
            self.test_dir, [(identity, self.test_dir / f"{name}.lua")]
        )
        result = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                *self.trace_flag,
                "install",
                "--manifest",
                str(manifest),
            ],
            capture_output=True,
            text=True,
        )

        if should_succeed:
            self.assertEqual(
                result.returncode,
                0,
                f"Spec {name} failed: {result.stderr}",
            )
        else:
            self.assertNotEqual(
                result.returncode,
                0,
                f"Spec {name} should have failed but succeeded",
            )

        return result

    def test_default_stage_extracts_to_install_dir(self):
        """Spec with no stage field auto-extracts archives."""
        # Default stage: no STAGE field, auto-extract archives to install_dir
        spec = """IDENTITY = "local.stage_default@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}
"""
        self.write_spec("default_extract", spec)
        self.run_spec("default_extract", "local.stage_default@v1")

        pkg_path = self.get_pkg_path("local.stage_default@v1")
        assert pkg_path

        # Default extraction keeps root/ directory
        self.assertTrue((pkg_path / "root").exists())
        self.assertTrue((pkg_path / "root" / "file1.txt").exists())
        self.assertTrue((pkg_path / "root" / "file2.txt").exists())
        self.assertTrue((pkg_path / "root" / "subdir1" / "file3.txt").exists())

    def test_declarative_strip_removes_top_level(self):
        """Spec with stage = {strip=1} removes top-level directory."""
        # Declarative stage with strip=1 to remove root/ prefix
        spec = """IDENTITY = "local.stage_declarative_strip@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = {{
  strip = 1
}}
"""
        self.write_spec("declarative_strip", spec)
        self.run_spec("declarative_strip", "local.stage_declarative_strip@v1")

        pkg_path = self.get_pkg_path("local.stage_declarative_strip@v1")
        assert pkg_path

        # With strip=1, root/ should be removed
        self.assertFalse((pkg_path / "root").exists())
        self.assertTrue((pkg_path / "file1.txt").exists())
        self.assertTrue((pkg_path / "file2.txt").exists())
        self.assertTrue((pkg_path / "subdir1" / "file3.txt").exists())

    def test_imperative_extract_all(self):
        """Spec with stage function using envy.extract_all works."""
        # Imperative stage using envy.extract_all with strip option
        spec = """IDENTITY = "local.stage_imperative@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end
"""
        self.write_spec("imperative_extract_all", spec)
        self.run_spec("imperative_extract_all", "local.stage_imperative@v1")

        pkg_path = self.get_pkg_path("local.stage_imperative@v1")
        assert pkg_path

        self.assertFalse((pkg_path / "root").exists())
        self.assertTrue((pkg_path / "file1.txt").exists())
        self.assertTrue((pkg_path / "subdir1" / "file3.txt").exists())

    def test_imperative_extract_single(self):
        """Spec with stage function using envy.extract for single file."""
        # Imperative stage extracting single archive with options
        spec = """IDENTITY = "local.stage_extract_single@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  local files = envy.extract(fetch_dir .. "/test.tar.gz", stage_dir, {{strip = 1}})
end
"""
        self.write_spec("imperative_extract_single", spec)
        self.run_spec("imperative_extract_single", "local.stage_extract_single@v1")

        pkg_path = self.get_pkg_path("local.stage_extract_single@v1")
        assert pkg_path

        self.assertFalse((pkg_path / "root").exists())
        self.assertTrue((pkg_path / "file1.txt").exists())

    def test_shell_script_basic(self):
        """Spec with shell script stage extracts and creates marker file."""
        # Shell script stage that extracts archive and creates marker
        spec = """IDENTITY = "local.stage_shell_basic@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

if envy.PLATFORM == "windows" then
  STAGE = [[
    tar -xzf ../fetch/test.tar.gz --strip-components=1
    "stage script executed" | Out-File -Encoding UTF8 STAGE_MARKER.txt
    Get-ChildItem -Force | Format-List > DIR_LIST.txt
    if (-not (Test-Path STAGE_MARKER.txt)) {{ exit 1 }}
    exit 0
  ]]
else
  STAGE = [[
    tar -xzf ../fetch/test.tar.gz --strip-components=1
    echo "stage script executed" > STAGE_MARKER.txt
    ls -la
  ]]
end
"""
        self.write_spec("shell_basic", spec)
        self.run_spec("shell_basic", "local.stage_shell_basic@v1")

        pkg_path = self.get_pkg_path("local.stage_shell_basic@v1")
        assert pkg_path

        self.assertTrue((pkg_path / "STAGE_MARKER.txt").exists())

        marker_content = (pkg_path / "STAGE_MARKER.txt").read_text()
        self.assertIn("stage script executed", marker_content)

        self.assertFalse((pkg_path / "root").exists())
        self.assertTrue((pkg_path / "file1.txt").exists())
        self.assertTrue((pkg_path / "file2.txt").exists())

    def test_shell_script_failure(self):
        """Spec with failing shell script should fail stage phase."""
        # Shell script that intentionally fails with non-zero exit
        spec = """IDENTITY = "local.stage_shell_failure@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  if envy.PLATFORM == "windows" then
    envy.run([[
      Write-Output "About to fail"
      exit 9
    ]], {{ shell = ENVY_SHELL.POWERSHELL, check = true }})
  else
    envy.run([[
      set -euo pipefail
      echo "About to fail"
      false
    ]], {{ check = true }})
  end
end
"""
        self.write_spec("shell_failure", spec)
        result = self.run_spec(
            "shell_failure", "local.stage_shell_failure@v1", should_succeed=False
        )

        error_output = result.stdout + result.stderr
        self.assertTrue(
            "Stage shell script failed" in error_output
            or "exit code 1" in error_output
            or "failed" in error_output.lower(),
            f"Expected stage failure message in output:\n{error_output}",
        )

    def test_shell_script_complex_operations(self):
        """Spec with complex shell operations creates custom directory structure."""
        # Complex shell script that reorganizes files into custom structure
        spec = """IDENTITY = "local.stage_shell_complex@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

if envy.PLATFORM == "windows" then
  STAGE = [[
    tar -xzf ../fetch/test.tar.gz --strip-components=1
    New-Item -ItemType Directory -Force -Path custom/bin | Out-Null
    New-Item -ItemType Directory -Force -Path custom/lib | Out-Null
    New-Item -ItemType Directory -Force -Path custom/share | Out-Null
    Move-Item file1.txt custom/bin/;
    Move-Item file2.txt custom/lib/;
    $fileCount = (Get-ChildItem -Recurse -File | Measure-Object).Count
    @(
      'Stage phase executed successfully'
      'Files reorganized into custom structure'
      ("Working directory: $(Get-Location)")
      ("File count: $fileCount")
    ) | Out-File -Encoding UTF8 custom/share/metadata.txt
    if (-not (Test-Path custom/bin/file1.txt)) {{ exit 1 }}
    if (-not (Test-Path custom/lib/file2.txt)) {{ exit 1 }}
    if (-not (Test-Path custom/share/metadata.txt)) {{ exit 1 }}
    Write-Output "Stage complete: custom structure created"
    exit 0
  ]]
else
  STAGE = [[
    tar -xzf ../fetch/test.tar.gz --strip-components=1
    mkdir -p custom/bin custom/lib custom/share
    mv file1.txt custom/bin/
    mv file2.txt custom/lib/
    cat > custom/share/metadata.txt << EOF
Stage phase executed successfully
Files reorganized into custom structure
Working directory: $(pwd)
File count: $(find . -type f | wc -l)
EOF
    test -f custom/bin/file1.txt || exit 1
    test -f custom/lib/file2.txt || exit 1
    test -f custom/share/metadata.txt || exit 1
    echo "Stage complete: custom structure created"
  ]]
end
"""
        self.write_spec("shell_complex", spec)
        self.run_spec("shell_complex", "local.stage_shell_complex@v1")

        pkg_path = self.get_pkg_path("local.stage_shell_complex@v1")
        assert pkg_path

        self.assertTrue((pkg_path / "custom").exists())
        self.assertTrue((pkg_path / "custom" / "bin").exists())
        self.assertTrue((pkg_path / "custom" / "lib").exists())
        self.assertTrue((pkg_path / "custom" / "share").exists())

        self.assertTrue((pkg_path / "custom" / "bin" / "file1.txt").exists())
        self.assertTrue((pkg_path / "custom" / "lib" / "file2.txt").exists())

        metadata_path = pkg_path / "custom" / "share" / "metadata.txt"
        self.assertTrue(metadata_path.exists())

        metadata_content = metadata_path.read_text()
        self.assertIn("Stage phase executed successfully", metadata_content)
        self.assertIn("Files reorganized", metadata_content)

    def test_shell_script_environment_access(self):
        """Spec with shell script can access environment variables."""
        # Shell script that writes environment variable info to file
        spec = """IDENTITY = "local.stage_shell_env@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

if envy.PLATFORM == "windows" then
  STAGE = [[
    tar -xzf ../fetch/test.tar.gz --strip-components=1
    $pathStatus = if ($env:Path) {{ 'yes' }} else {{ '' }}
    $homeStatus = if ($env:UserProfile) {{ 'yes' }} else {{ '' }}
    $userStatus = if ($env:USERNAME) {{ 'yes' }} else {{ '' }}
    @(
      "PATH is available: $pathStatus"
      "HOME is available: $homeStatus"
      "USER is available: $userStatus"
      ("Shell: powershell")
    ) | Out-File -Encoding UTF8 env_info.txt
    if (-not (Test-Path env_info.txt)) {{ exit 1 }}
    if (-not (Select-String -Path env_info.txt -Pattern "PATH is available: yes" -Quiet)) {{ exit 1 }}
    Write-Output "Environment check complete"
    exit 0
  ]]
else
  STAGE = [[
    tar -xzf ../fetch/test.tar.gz --strip-components=1
    cat > env_info.txt << EOF
PATH is available: ${{PATH:+yes}}
HOME is available: ${{HOME:+yes}}
USER is available: ${{USER:+yes}}
Shell: $(basename $SHELL)
EOF
    test -f env_info.txt || exit 1
    grep -q "PATH is available: yes" env_info.txt || exit 1
    echo "Environment check complete"
  ]]
end
"""
        self.write_spec("shell_env", spec)
        self.run_spec("shell_env", "local.stage_shell_env@v1")

        pkg_path = self.get_pkg_path("local.stage_shell_env@v1")
        assert pkg_path

        env_info_path = pkg_path / "env_info.txt"
        self.assertTrue(env_info_path.exists())

        env_content = env_info_path.read_text()
        self.assertIn("PATH is available: yes", env_content)


if __name__ == "__main__":
    unittest.main()
