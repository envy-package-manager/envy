"""Functional tests for engine build phase.

Tests build phase with nil, string, and function forms. Verifies ctx API
(run, package, copy, move, extract) and build phase integration.
"""

import hashlib
import io
import os
import tarfile
from pathlib import Path
import unittest

from . import test_config
from .env import EnvyTestCase

# Test archive contents
TEST_ARCHIVE_FILES = {
    "root/file1.txt": "Root file content\n",
    "root/file2.txt": "Another root file\n",
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


class TestBuildPhase(EnvyTestCase):
    """Tests for build phase (compilation and processing workflows)."""

    def setUp(self):
        super().setUp()
        self.specs_dir = self.make_temp_dir("specs_dir")
        self.trace_flag = ["--trace"] if os.environ.get("ENVY_TEST_TRACE") else []

        # Create test archive and get its hash
        self.archive_path = self.specs_dir / "test.tar.gz"
        self.archive_hash = create_test_archive(self.archive_path)

    def write_spec(self, name: str, content: str) -> str:
        """Write spec to temp dir with placeholder substitution, return path."""
        spec_content = content.format(
            ARCHIVE_PATH=self.archive_path.as_posix(),
            ARCHIVE_HASH=self.archive_hash,
            SPECS_DIR=self.specs_dir.as_posix(),
        )
        path = self.specs_dir / f"{name}.lua"
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
            self.specs_dir, [(identity, self.specs_dir / f"{name}.lua")]
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

    # =========================================================================
    # Basic build phase forms
    # =========================================================================

    def test_build_nil_skips_phase(self):
        """Spec with no build field should skip build phase."""
        # Build = nil: no BUILD field, skip build phase and proceed to install
        spec = """IDENTITY = "local.build_nil@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = {{strip = 1}}
"""
        self.write_spec("nil_skip_build", spec)
        self.run_spec("nil_skip_build", "local.build_nil@v1")

        pkg_path = self.get_pkg_path("local.build_nil@v1")
        self.assertIsNotNone(pkg_path)
        self.assertTrue((pkg_path / "file1.txt").exists())

    def test_build_string_executes_shell(self):
        """Spec with build = function executing shell script."""
        # Build creates artifacts via shell script
        spec = """IDENTITY = "local.build_string@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = {{strip = 1}}

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  if envy.PLATFORM == "windows" then
    envy.run([[
      Write-Host "Building in shell script mode"
      New-Item -ItemType Directory -Path build_output -Force | Out-Null
      Set-Content -Path build_output/artifact.txt -Value "build_artifact"
      Get-ChildItem
      if (-not (Test-Path build_output/artifact.txt)) {{ Write-Error "artifact missing"; exit 1 }}
      Write-Output "Build string shell success"
    ]], {{ shell = ENVY_SHELL.POWERSHELL }})
  else
    envy.run([[
      echo "Building in shell script mode"
      mkdir -p build_output
      echo "build_artifact" > build_output/artifact.txt
      ls -la
    ]])
  end
end
"""
        self.write_spec("string_shell", spec)
        self.run_spec("string_shell", "local.build_string@v1")

        pkg_path = self.get_pkg_path("local.build_string@v1")
        self.assertIsNotNone(pkg_path)
        self.assertTrue((pkg_path / "build_output").exists())
        self.assertTrue((pkg_path / "build_output" / "artifact.txt").exists())

        content = (pkg_path / "build_output" / "artifact.txt").read_text()
        self.assertEqual(content.strip(), "build_artifact")

    def test_build_function_with_ctx_run(self):
        """Spec with build = function(ctx) uses envy.run() with capture."""
        # Build function using envy.run() with capture to verify output
        spec = """IDENTITY = "local.build_function@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = {{strip = 1}}

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  print("Building with envy.run()")

  local result
  if envy.PLATFORM == "windows" then
    result = envy.run([[
mkdir build_output 2> nul
echo function_artifact > build_output\\result.txt
echo Build complete
    ]],{{ shell = ENVY_SHELL.CMD, capture = true }})
  else
    result = envy.run([[
      mkdir -p build_output
      echo "function_artifact" > build_output/result.txt
      echo "Build complete"
    ]], {{ capture = true }})
  end

  if not result.stdout:match("Build complete") then
    error("Expected 'Build complete' in stdout")
  end

  print("Build finished successfully")
end
"""
        self.write_spec("function_ctx_run", spec)
        self.run_spec("function_ctx_run", "local.build_function@v1")

        pkg_path = self.get_pkg_path("local.build_function@v1")
        self.assertIsNotNone(pkg_path)
        self.assertTrue((pkg_path / "build_output" / "result.txt").exists())

        content = (pkg_path / "build_output" / "result.txt").read_text()
        self.assertEqual(content.strip(), "function_artifact")

    # =========================================================================
    # Dependency access tests
    # =========================================================================

    def test_build_with_package_dependency(self):
        """Build phase can access dependencies via envy.package()."""
        # First write the dependency spec
        dep_spec = """IDENTITY = "local.build_dependency@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = {{strip = 1}}

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  if envy.PLATFORM == "windows" then
    envy.run([[Write-Output "dependency: begin"; Remove-Item -Force dependency.txt -ErrorAction SilentlyContinue; Set-Content -Path dependency.txt -Value "dependency_data"; New-Item -ItemType Directory -Path bin -Force | Out-Null; Set-Content -Path bin/app -Value "binary"; if (-not (Test-Path bin/app)) {{ Write-Error "missing bin/app"; exit 1 }}; Write-Output "dependency: success"; exit 0 ]], {{ shell = ENVY_SHELL.POWERSHELL }})
  else
    envy.run([[echo 'dependency_data' > dependency.txt
      mkdir -p bin
      echo 'binary' > bin/app]])
  end
end
"""
        self.write_spec("dependency", dep_spec)

        # Consumer spec that uses envy.package() to access dependency
        consumer_spec = """IDENTITY = "local.build_with_package@v1"

DEPENDENCIES = {{
  {{ spec = "local.build_dependency@v1", source = "{SPECS_DIR}/dependency.lua" }}
}}

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = {{strip = 1}}

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  print("Accessing dependency via envy.package()")

  local dep_path = envy.package("local.build_dependency@v1")
  print("Dependency path: " .. dep_path)

  local result
  if envy.PLATFORM == "windows" then
    result = envy.run([[
      $depFile = ']] .. dep_path .. [[/dependency.txt'
      if (-not (Test-Path $depFile)) {{ Start-Sleep -Milliseconds 100 }}
      if (-not (Test-Path $depFile)) {{ Write-Error "Dependency artifact missing"; exit 61 }}
      Get-Content $depFile | Set-Content -Path from_dependency.txt
      Write-Output "Used dependency data"
      if (-not (Test-Path from_dependency.txt)) {{ Write-Error "Output artifact missing"; exit 62 }}
      exit 0
    ]], {{ shell = ENVY_SHELL.POWERSHELL, capture = true }})
  else
    result = envy.run([[
      cat "]] .. dep_path .. [[/dependency.txt" > from_dependency.txt
      echo "Used dependency data"
    ]], {{ capture = true }})
  end

  if not result.stdout:match("Used dependency data") then
    error("Failed to use dependency")
  end
end
"""
        self.write_spec("with_package", consumer_spec)
        self.run_spec("with_package", "local.build_with_package@v1")

        pkg_path = self.get_pkg_path("local.build_with_package@v1")
        self.assertIsNotNone(pkg_path)
        self.assertTrue((pkg_path / "from_dependency.txt").exists())

        content = (pkg_path / "from_dependency.txt").read_text()
        self.assertEqual(content.strip(), "dependency_data")

    # =========================================================================
    # File operation tests (copy, move, extract)
    # =========================================================================

    def test_build_with_copy_operations(self):
        """Build phase can copy files and directories with envy.copy()."""
        # Test envy.copy() for single file and directory copy
        spec = """IDENTITY = "local.build_with_copy@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = {{strip = 1}}

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  print("Testing envy.copy()")

  if envy.PLATFORM == "windows" then
    envy.run([[
      Set-Content -Path source.txt -Value "source_file"
      New-Item -ItemType Directory -Path source_dir -Force | Out-Null
      Set-Content -Path source_dir/file1.txt -Value "nested1"
      Set-Content -Path source_dir/file2.txt -Value "nested2"
      Write-Output "creation_done"
    ]], {{ shell = ENVY_SHELL.POWERSHELL }})
  else
    envy.run([[
      echo "source_file" > source.txt
      mkdir -p source_dir
      echo "nested1" > source_dir/file1.txt
      echo "nested2" > source_dir/file2.txt
    ]])
  end

  envy.copy("source.txt", "dest_file.txt")
  envy.copy("source_dir", "dest_dir")
  -- Missing parents are created for a directory destination, not just a file one
  envy.copy("source_dir", "new/nested/dest_dir")

  if envy.PLATFORM == "windows" then
    envy.run([[
      if (-not (Test-Path dest_file.txt)) {{ Write-Output "missing dest_file.txt"; exit 1 }}
      if (-not (Test-Path dest_dir/file1.txt)) {{ Write-Output "missing file1.txt"; exit 1 }}
      if (-not (Test-Path dest_dir/file2.txt)) {{ Write-Output "missing file2.txt"; exit 1 }}
      Write-Output "Copy operations successful"
    ]], {{ shell = ENVY_SHELL.POWERSHELL }})
  else
    envy.run([[
      test -f dest_file.txt || exit 1
      test -f dest_dir/file1.txt || exit 1
      test -f dest_dir/file2.txt || exit 1
      echo "Copy operations successful"
    ]])
  end
end
"""
        self.write_spec("with_copy", spec)
        self.run_spec("with_copy", "local.build_with_copy@v1")

        pkg_path = self.get_pkg_path("local.build_with_copy@v1")
        self.assertIsNotNone(pkg_path)

        self.assertTrue((pkg_path / "dest_file.txt").exists())
        content = (pkg_path / "dest_file.txt").read_text()
        self.assertEqual(content.strip(), "source_file")

        self.assertTrue((pkg_path / "dest_dir").is_dir())
        self.assertTrue((pkg_path / "dest_dir" / "file1.txt").exists())
        self.assertTrue((pkg_path / "dest_dir" / "file2.txt").exists())

        nested = pkg_path / "new" / "nested" / "dest_dir"
        self.assertTrue(nested.is_dir())
        self.assertTrue((nested / "file1.txt").exists())

    def test_build_with_move_operations(self):
        """Build phase can move files and directories with envy.move()."""
        # Test envy.move() for efficient rename operations
        spec = """IDENTITY = "local.build_with_move@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = {{strip = 1}}

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  print("Testing envy.move()")

  if envy.PLATFORM == "windows" then
    envy.run([[
      Set-Content -Path source_move.txt -Value "moveable_file"
      New-Item -ItemType Directory -Path move_dir -Force | Out-Null
      Set-Content -Path move_dir/content.txt -Value "dir_content"
    ]], {{ shell = ENVY_SHELL.POWERSHELL }})
  else
    envy.run([[
      echo "moveable_file" > source_move.txt
      mkdir -p move_dir
      echo "dir_content" > move_dir/content.txt
    ]])
  end

  envy.move("source_move.txt", "moved_file.txt")
  envy.move("move_dir", "moved_dir")

  if envy.PLATFORM == "windows" then
    envy.run([[
      if (Test-Path source_move.txt) {{ exit 1 }}
      if (-not (Test-Path moved_file.txt)) {{ exit 1 }}
      if (Test-Path move_dir) {{ exit 1 }}
      if (-not (Test-Path moved_dir/content.txt)) {{ exit 1 }}
      Write-Output "Move operations successful"
    ]], {{ shell = ENVY_SHELL.POWERSHELL }})
  else
    envy.run([[
      test ! -f source_move.txt || exit 1
      test -f moved_file.txt || exit 1
      test ! -d move_dir || exit 1
      test -f moved_dir/content.txt || exit 1
      echo "Move operations successful"
    ]])
  end
end
"""
        self.write_spec("with_move", spec)
        self.run_spec("with_move", "local.build_with_move@v1")

        pkg_path = self.get_pkg_path("local.build_with_move@v1")
        self.assertIsNotNone(pkg_path)

        self.assertFalse((pkg_path / "source_move.txt").exists())
        self.assertFalse((pkg_path / "move_dir").exists())

        self.assertTrue((pkg_path / "moved_file.txt").exists())
        self.assertTrue((pkg_path / "moved_dir").is_dir())
        self.assertTrue((pkg_path / "moved_dir" / "content.txt").exists())

    def test_build_with_extract(self):
        """Build phase can extract archives with envy.extract()."""
        # Test envy.extract() to extract archive in build phase
        spec = """IDENTITY = "local.build_with_extract@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  if envy.PLATFORM == "windows" then
    envy.run([[New-Item -ItemType Directory -Path manual_build -Force | Out-Null]], {{ shell = ENVY_SHELL.POWERSHELL }})
  else
    envy.run("mkdir -p manual_build")
  end
end

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  print("Testing envy.extract()")

  local files_extracted = envy.extract(fetch_dir .. "/test.tar.gz", stage_dir)
  print("Extracted " .. files_extracted .. " files")

  if envy.PLATFORM == "windows" then
    envy.run([[
      if (-not (Test-Path root -PathType Container)) {{ exit 1 }}
      if (-not (Test-Path root/file1.txt)) {{ exit 1 }}
      Write-Output "Extract successful"
    ]], {{ shell = ENVY_SHELL.POWERSHELL }})
  else
    envy.run([[
      test -d root || exit 1
      test -f root/file1.txt || exit 1
      echo "Extract successful"
    ]])
  end
end
"""
        self.write_spec("with_extract", spec)
        self.run_spec("with_extract", "local.build_with_extract@v1")

        pkg_path = self.get_pkg_path("local.build_with_extract@v1")
        self.assertIsNotNone(pkg_path)
        self.assertTrue((pkg_path / "root").exists())
        self.assertTrue((pkg_path / "root" / "file1.txt").exists())

    # =========================================================================
    # Multi-operation tests
    # =========================================================================

    def test_build_multiple_operations(self):
        """Build phase can chain multiple operations."""
        # Multi-step build: create -> copy -> modify -> move -> verify
        spec = """IDENTITY = "local.build_multiple_operations@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = {{strip = 1}}

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  print("Testing multiple operations")

  if envy.PLATFORM == "windows" then
    envy.run([[
      New-Item -ItemType Directory -Path step1 -Force | Out-Null
      Set-Content -Path step1/data.txt -Value "step1_output"
    ]], {{ shell = ENVY_SHELL.POWERSHELL }})
  else
    envy.run([[
      mkdir -p step1
      echo "step1_output" > step1/data.txt
    ]])
  end

  envy.copy("step1", "step2")

  if envy.PLATFORM == "windows" then
    envy.run([[
      Add-Content -Path step2/data.txt -Value "step2_additional"
      Set-Content -Path step2/new.txt -Value "step2_new"
    ]], {{ shell = ENVY_SHELL.POWERSHELL }})
  else
    envy.run([[
      echo "step2_additional" >> step2/data.txt
      echo "step2_new" > step2/new.txt
    ]])
  end

  envy.move("step2", "final")

  if envy.PLATFORM == "windows" then
    envy.run([[
      if (-not (Test-Path final -PathType Container)) {{ Write-Output "missing final"; exit 1 }}
      if (Test-Path step2) {{ Write-Output "step2 still exists"; exit 1 }}
      if (-not (Select-String -Path final/data.txt -Pattern "step1_output" -Quiet)) {{ Write-Output "missing step1_output"; exit 1 }}
      if (-not (Select-String -Path final/data.txt -Pattern "step2_additional" -Quiet)) {{ Write-Output "missing step2_additional"; exit 1 }}
      if (-not (Test-Path final/new.txt)) {{ Write-Output "missing new.txt"; exit 1 }}
      Write-Output "All operations completed"
    ]], {{ shell = ENVY_SHELL.POWERSHELL }})
  else
    envy.run([[
      test -d final || exit 1
      test ! -d step2 || exit 1
      grep -q step1_output final/data.txt || exit 1
      grep -q step2_additional final/data.txt || exit 1
      test -f final/new.txt || exit 1
      echo "All operations completed"
    ]])
  end

  print("Multiple operations successful")
end
"""
        self.write_spec("multi_op", spec)
        self.run_spec("multi_op", "local.build_multiple_operations@v1")

        pkg_path = self.get_pkg_path("local.build_multiple_operations@v1")
        self.assertIsNotNone(pkg_path)

        self.assertFalse((pkg_path / "step2").exists())

        self.assertTrue((pkg_path / "final").is_dir())
        self.assertTrue((pkg_path / "final" / "data.txt").exists())
        self.assertTrue((pkg_path / "final" / "new.txt").exists())

        content = (pkg_path / "final" / "data.txt").read_text()
        self.assertIn("step1_output", content)
        self.assertIn("step2_additional", content)

    # =========================================================================
    # Environment and cwd tests
    # =========================================================================

    def test_build_with_custom_env(self):
        """Build phase can set custom environment variables."""
        # Test envy.run() with custom env parameter
        spec = """IDENTITY = "local.build_with_env@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = {{strip = 1}}

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  print("Testing custom environment variables")

  local result
  if envy.PLATFORM == "windows" then
    result = envy.run([[
      Write-Output "BUILD_MODE=$env:BUILD_MODE"
      Write-Output "CUSTOM_VAR=$env:CUSTOM_VAR"
      if ($env:BUILD_MODE -ne "release") {{ exit 1 }}
      if ($env:CUSTOM_VAR -ne "test_value") {{ exit 1 }}
    ]], {{
      env = {{ BUILD_MODE = "release", CUSTOM_VAR = "test_value" }},
      shell = ENVY_SHELL.POWERSHELL,
      capture = true
    }})
  else
    result = envy.run([[
      echo "BUILD_MODE=$BUILD_MODE"
      echo "CUSTOM_VAR=$CUSTOM_VAR"
      test "$BUILD_MODE" = "release" || exit 1
      test "$CUSTOM_VAR" = "test_value" || exit 1
    ]], {{
      env = {{ BUILD_MODE = "release", CUSTOM_VAR = "test_value" }},
      capture = true
    }})
  end

  if not result.stdout:match("BUILD_MODE=release") then
    error("BUILD_MODE not set correctly")
  end

  if not result.stdout:match("CUSTOM_VAR=test_value") then
    error("CUSTOM_VAR not set correctly")
  end

  print("Environment variables work correctly")
end
"""
        self.write_spec("with_env", spec)
        self.run_spec("with_env", "local.build_with_env@v1")

    def test_build_with_custom_cwd(self):
        """Build phase can run commands in custom working directory."""
        # Test envy.run() with cwd parameter
        spec = """IDENTITY = "local.build_with_cwd@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = {{strip = 1}}

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  print("Testing custom working directory")

  if envy.PLATFORM == "windows" then
    envy.run([[New-Item -ItemType Directory -Path subdir -Force | Out-Null]], {{ shell = ENVY_SHELL.POWERSHELL }})
  else
    envy.run("mkdir -p subdir")
  end

  if envy.PLATFORM == "windows" then
    envy.run([[
      (Get-Location).Path | Out-File -FilePath current_dir.txt
      Set-Content -Path marker.txt -Value "In subdirectory"
    ]], {{cwd = "subdir", shell = ENVY_SHELL.POWERSHELL}})
  else
    envy.run([[
      pwd > current_dir.txt
      echo "In subdirectory" > marker.txt
    ]], {{cwd = "subdir"}})
  end

  if envy.PLATFORM == "windows" then
    envy.run([[
      if (-not (Test-Path subdir/marker.txt)) {{ Write-Output "missing subdir/marker.txt"; exit 1 }}
      $content = Get-Content subdir/current_dir.txt -Raw
      if ($content -notmatch "(?i)subdir") {{ Write-Output "current_dir does not contain subdir: $content"; exit 1 }}
      Write-Output "CWD subdir verified"
    ]], {{ shell = ENVY_SHELL.POWERSHELL }})
  else
    envy.run([[
      test -f subdir/marker.txt || exit 1
      grep -q subdir subdir/current_dir.txt || exit 1
    ]])
  end

  if envy.PLATFORM == "windows" then
    envy.run([[New-Item -ItemType Directory -Path nested/deep/dir -Force | Out-Null]], {{ shell = ENVY_SHELL.POWERSHELL }})
  else
    envy.run("mkdir -p nested/deep/dir")
  end

  if envy.PLATFORM == "windows" then
    envy.run([[Set-Content -Path deep_marker.txt -Value "deep"]], {{cwd = "nested/deep/dir", shell = ENVY_SHELL.POWERSHELL}})
  else
    envy.run([[echo "deep" > deep_marker.txt]], {{cwd = "nested/deep/dir"}})
  end

  if envy.PLATFORM == "windows" then
    envy.run([[
      if (-not (Test-Path nested/deep/dir/deep_marker.txt)) {{ Write-Output "missing deep_marker.txt"; exit 1 }}
      Write-Output "CWD operations successful"
    ]], {{ shell = ENVY_SHELL.POWERSHELL }})
  else
    envy.run([[
      test -f nested/deep/dir/deep_marker.txt || exit 1
      echo "CWD operations successful"
    ]])
  end

  print("Custom working directory works correctly")
end
"""
        self.write_spec("with_cwd", spec)
        self.run_spec("with_cwd", "local.build_with_cwd@v1")

        pkg_path = self.get_pkg_path("local.build_with_cwd@v1")
        self.assertIsNotNone(pkg_path)
        self.assertTrue((pkg_path / "subdir" / "marker.txt").exists())
        self.assertTrue(
            (pkg_path / "nested" / "deep" / "dir" / "deep_marker.txt").exists()
        )

    # =========================================================================
    # Error handling tests
    # =========================================================================

    def test_build_error_nonzero_exit(self):
        """Build phase fails on non-zero exit code."""
        # Build that exits with non-zero code
        spec = """IDENTITY = "local.build_error_nonzero_exit@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = {{strip = 1}}

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  print("Testing error handling")

  if envy.PLATFORM == "windows" then
    envy.run("exit 42", {{ shell = ENVY_SHELL.POWERSHELL }})
  else
    envy.run("exit 42")
  end

  error("Should not reach here")
end
"""
        self.write_spec("error_nonzero", spec)
        self.run_spec(
            "error_nonzero", "local.build_error_nonzero_exit@v1", should_succeed=False
        )

    def test_build_string_error(self):
        """Build phase fails when shell script returns non-zero."""
        # Build with intentional shell script failure
        spec = """IDENTITY = "local.build_string_error@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = {{strip = 1}}

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  if envy.PLATFORM == "windows" then
    local result = envy.run([[Write-Output "Starting build"; Write-Error "Intentional failure"; exit 7 ]], {{ shell = ENVY_SHELL.POWERSHELL }})
    error("Intentional failure after ctx.run")
  else
    envy.run([[
      set -e
      echo "Starting build"
      false
      echo "This should not execute"
    ]], {{ check = true }})
  end
end
"""
        self.write_spec("string_error", spec)
        self.run_spec(
            "string_error", "local.build_string_error@v1", should_succeed=False
        )

    # =========================================================================
    # Directory access tests
    # =========================================================================

    def test_build_access_directories(self):
        """Build phase has access to fetch_dir, stage_dir."""
        # Verify directory parameters are accessible and contain expected data
        spec = """IDENTITY = "local.build_access_dirs@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = {{strip = 1}}

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  print("Testing directory access")
  print("fetch_dir: " .. fetch_dir)
  print("stage_dir: " .. stage_dir)

  if envy.PLATFORM == "windows" then
    envy.run([[
      if (-not (Test-Path -LiteralPath ']] .. fetch_dir .. [[' -PathType Container)) {{ exit 42 }}
      if (-not (Test-Path -LiteralPath ']] .. stage_dir .. [[' -PathType Container)) {{ exit 43 }}
      Write-Output "All directories exist"
    ]], {{ shell = ENVY_SHELL.POWERSHELL }})
  else
    envy.run([[
      test -d "]] .. fetch_dir .. [[" || exit 1
      test -d "]] .. stage_dir .. [[" || exit 1
      echo "All directories exist"
    ]])
  end

  if envy.PLATFORM == "windows" then
    envy.run([[
      if (-not (Test-Path -LiteralPath ']] .. fetch_dir .. [[/test.tar.gz')) {{ exit 45 }}
      Write-Output "Archive found in fetch_dir"
    ]], {{ shell = ENVY_SHELL.POWERSHELL }})
  else
    envy.run([[
      test -f "]] .. fetch_dir .. [[/test.tar.gz" || exit 1
      echo "Archive found in fetch_dir"
    ]])
  end

  if envy.PLATFORM == "windows" then
    envy.run([[Set-Content -Path build_marker.txt -Value "Built successfully"]], {{ shell = ENVY_SHELL.POWERSHELL }})
  else
    envy.run([[echo "Built successfully" > build_marker.txt]])
  end

  print("Directory access successful")
end
"""
        self.write_spec("access_dirs", spec)
        self.run_spec("access_dirs", "local.build_access_dirs@v1")

        pkg_path = self.get_pkg_path("local.build_access_dirs@v1")
        self.assertIsNotNone(pkg_path)

    def test_build_nested_directory_structure(self):
        """Build phase can create and manipulate nested directories."""
        # Complex nested directory creation and copying
        spec = """IDENTITY = "local.build_nested_dirs@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = {{strip = 1}}

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  print("Creating nested directory structure")

  if envy.PLATFORM == "windows" then
    envy.run([[
      New-Item -ItemType Directory -Path output/bin -Force | Out-Null
      New-Item -ItemType Directory -Path output/lib/x86_64 -Force | Out-Null
      New-Item -ItemType Directory -Path output/include/subproject -Force | Out-Null
      New-Item -ItemType Directory -Path output/share/doc -Force | Out-Null
      Set-Content -Path output/bin/app -Value "binary"
      Set-Content -Path output/lib/x86_64/libapp.so -Value "library"
      Set-Content -Path output/include/app.h -Value "header"
      Set-Content -Path output/include/subproject/sub.h -Value "nested_header"
      Set-Content -Path output/share/doc/README.md -Value "documentation"
    ]], {{ shell = ENVY_SHELL.POWERSHELL }})
  else
    envy.run([[
      mkdir -p output/bin
      mkdir -p output/lib/x86_64
      mkdir -p output/include/subproject
      mkdir -p output/share/doc
      echo "binary" > output/bin/app
      echo "library" > output/lib/x86_64/libapp.so
      echo "header" > output/include/app.h
      echo "nested_header" > output/include/subproject/sub.h
      echo "documentation" > output/share/doc/README.md
    ]])
  end

  envy.copy("output", "copied_output")

  if envy.PLATFORM == "windows" then
    envy.run([[
      if (-not (Test-Path copied_output/bin/app)) {{ Write-Output "missing bin/app"; exit 1 }}
      if (-not (Test-Path copied_output/lib/x86_64/libapp.so)) {{ Write-Output "missing libapp.so"; exit 1 }}
      if (-not (Test-Path copied_output/include/app.h)) {{ Write-Output "missing app.h"; exit 1 }}
      if (-not (Test-Path copied_output/include/subproject/sub.h)) {{ Write-Output "missing sub.h"; exit 1 }}
      if (-not (Test-Path copied_output/share/doc/README.md)) {{ Write-Output "missing README.md"; exit 1 }}
      Write-Output "Nested directory operations successful"
    ]], {{ shell = ENVY_SHELL.POWERSHELL }})
  else
    envy.run([[
      test -f copied_output/bin/app || exit 1
      test -f copied_output/lib/x86_64/libapp.so || exit 1
      test -f copied_output/include/app.h || exit 1
      test -f copied_output/include/subproject/sub.h || exit 1
      test -f copied_output/share/doc/README.md || exit 1
      echo "Nested directory operations successful"
    ]])
  end

  print("Nested directory handling works correctly")
end
"""
        self.write_spec("nested_dirs", spec)
        self.run_spec("nested_dirs", "local.build_nested_dirs@v1")

        pkg_path = self.get_pkg_path("local.build_nested_dirs@v1")
        self.assertIsNotNone(pkg_path)

        self.assertTrue((pkg_path / "copied_output" / "bin" / "app").exists())
        self.assertTrue(
            (pkg_path / "copied_output" / "lib" / "x86_64" / "libapp.so").exists()
        )
        self.assertTrue((pkg_path / "copied_output" / "include" / "app.h").exists())
        self.assertTrue(
            (pkg_path / "copied_output" / "include" / "subproject" / "sub.h").exists()
        )
        self.assertTrue(
            (pkg_path / "copied_output" / "share" / "doc" / "README.md").exists()
        )

    # =========================================================================
    # Output capture tests
    # =========================================================================

    def test_build_output_capture(self):
        """Build phase captures stdout correctly."""
        # Test envy.run() capture=true captures output correctly
        spec = """IDENTITY = "local.build_output_capture@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = {{strip = 1}}

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  print("Testing output capture")

  local result
  if envy.PLATFORM == "windows" then
    result = envy.run(
        [[if (-not $PSVersionTable) {{ Write-Output "psversion-init" }}; Write-Output "line1"; if (-not ("line1")) {{ Write-Output "line1" }}; Write-Output "line2"; Write-Output "line3"; exit 0]],
        {{ shell = ENVY_SHELL.POWERSHELL, capture = true }})
  else
    result = envy.run([[
      echo "line1"
      echo "line2"
      echo "line3"
    ]], {{ capture = true }})
  end

  if not result.stdout:match("line1") then error("Missing line1 in stdout") end
  if not result.stdout:match("line2") then error("Missing line2 in stdout") end
  if not result.stdout:match("line3") then error("Missing line3 in stdout") end

  if envy.PLATFORM == "windows" then
      result = envy.run(
          [[Write-Output "Special: !@#$%^&*()"; Write-Output "Unicode: hello"; Write-Output "Quotes: 'single' \\"double\\""; exit 0]],
          {{ shell = ENVY_SHELL.POWERSHELL, capture = true }})
  else
    result = envy.run([[
      echo "Special: !@#$%^&*()"
      echo "Unicode: hello"
      echo "Quotes: 'single' \\"double\\""
    ]], {{ capture = true }})
  end

  if not result.stdout:match("Special:") then
    error("Missing special characters in output")
  end

  print("Output capture works correctly")
end
"""
        self.write_spec("output_capture", spec)
        self.run_spec("output_capture", "local.build_output_capture@v1")

    # =========================================================================
    # Function return string tests
    # =========================================================================

    def test_build_function_returns_string(self):
        """Build function can return a string that gets executed."""
        # Build function does setup then returns a script to execute
        spec = """IDENTITY = "local.build_function_returns_string@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = {{strip = 1}}

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  print("BUILD function executing, preparing to return script")

  if envy.PLATFORM == "windows" then
    envy.run("mkdir setup_dir 2> nul", {{ shell = ENVY_SHELL.CMD }})
  else
    envy.run("mkdir -p setup_dir")
  end

  if envy.PLATFORM == "windows" then
    return [[
      New-Item -ItemType Directory -Force -Path output_from_returned_script | Out-Null
      Set-Content -Path output_from_returned_script\\marker.txt -Value "returned_script_artifact" -NoNewline
    ]]
  else
    return [[
      mkdir -p output_from_returned_script
      echo "returned_script_artifact" > output_from_returned_script/marker.txt
    ]]
  end
end
"""
        self.write_spec("func_returns_string", spec)
        self.run_spec("func_returns_string", "local.build_function_returns_string@v1")

        pkg_path = self.get_pkg_path("local.build_function_returns_string@v1")
        self.assertIsNotNone(pkg_path)
        self.assertTrue((pkg_path / "setup_dir").exists())

        self.assertTrue((pkg_path / "output_from_returned_script").exists())
        self.assertTrue(
            (pkg_path / "output_from_returned_script" / "marker.txt").exists()
        )

        content = (pkg_path / "output_from_returned_script" / "marker.txt").read_text()
        self.assertEqual(content.strip(), "returned_script_artifact")

    # =========================================================================
    # Cache path tests
    # =========================================================================

    def test_cache_path_includes_platform_arch(self):
        """Verify cache variant directory includes platform-arch prefix."""
        # Run a spec and verify cache directory naming
        spec = """IDENTITY = "local.build_cache_test@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = {{strip = 1}}

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  if envy.PLATFORM == "windows" then
    envy.run([[Set-Content -Path result.txt -Value "test"]], {{ shell = ENVY_SHELL.POWERSHELL }})
  else
    envy.run([[echo "test" > result.txt]])
  end
end
"""
        self.write_spec("cache_test", spec)
        self.run_spec("cache_test", "local.build_cache_test@v1")

        identity_dir = self.cache_root / "packages" / "local.build_cache_test@v1"
        self.assertTrue(identity_dir.exists())

        variant_dirs = [d for d in identity_dir.iterdir() if d.is_dir()]
        self.assertEqual(
            len(variant_dirs), 1, "Should have exactly one variant directory"
        )

        variant_name = variant_dirs[0].name
        self.assertNotIn("/", variant_name)
        self.assertIn("-blake3-", variant_name)

    # =========================================================================
    # install_dir parameter tests
    # =========================================================================

    def test_build_receives_install_dir(self):
        """BUILD function receives install_dir as first parameter."""
        spec = """IDENTITY = "local.build_install_dir@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = {{strip = 1}}

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  -- Verify install_dir is a non-empty string
  if type(install_dir) ~= "string" or install_dir == "" then
    error("install_dir should be a non-empty string, got: " .. tostring(install_dir))
  end

  -- Write install_dir to a marker file in stage_dir for verification
  if envy.PLATFORM == "windows" then
    envy.run('Set-Content -Path build_install_dir.txt -Value "' .. install_dir .. '"', {{ shell = ENVY_SHELL.POWERSHELL }})
  else
    envy.run('echo "' .. install_dir .. '" > build_install_dir.txt')
  end
end

INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  -- Read what BUILD wrote
  local build_marker = stage_dir .. "build_install_dir.txt"
  if not envy.exists(build_marker) then
    error("BUILD should have written build_install_dir.txt")
  end

  -- Copy the marker to install_dir so we can verify from Python
  envy.copy(build_marker, install_dir .. "build_install_dir.txt")

  -- Also write INSTALL's view of install_dir
  if envy.PLATFORM == "windows" then
    envy.run('Set-Content -Path "' .. install_dir .. 'install_install_dir.txt" -Value "' .. install_dir .. '"', {{ shell = ENVY_SHELL.POWERSHELL }})
  else
    envy.run('echo "' .. install_dir .. '" > "' .. install_dir .. 'install_install_dir.txt"')
  end
end
"""
        self.write_spec("build_install_dir", spec)
        self.run_spec("build_install_dir", "local.build_install_dir@v1")

        pkg_path = self.get_pkg_path("local.build_install_dir@v1")
        self.assertIsNotNone(pkg_path, "Package should be installed")

        # Verify BUILD received install_dir
        build_marker = pkg_path / "build_install_dir.txt"
        self.assertTrue(
            build_marker.exists(), "BUILD should have written install_dir marker"
        )
        build_install_dir = build_marker.read_text().strip()
        self.assertTrue(
            len(build_install_dir) > 0, "BUILD install_dir should be non-empty"
        )

        # Verify INSTALL received install_dir
        install_marker = pkg_path / "install_install_dir.txt"
        self.assertTrue(
            install_marker.exists(), "INSTALL should have written install_dir marker"
        )
        install_install_dir = install_marker.read_text().strip()

        # Both should point to the same directory
        self.assertEqual(
            build_install_dir,
            install_install_dir,
            "BUILD and INSTALL should receive the same install_dir",
        )

        # install_dir is now pkg/ directly (no rename step)
        normalized = build_install_dir.rstrip("/").rstrip("\\").replace("\\", "/")
        self.assertTrue(
            normalized.endswith("/pkg"),
            f"install_dir should end with /pkg, got: {build_install_dir}",
        )

    def test_build_install_dir_usable_for_prefix(self):
        """BUILD can use install_dir for --prefix style configuration."""
        spec = """IDENTITY = "local.build_prefix@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = {{strip = 1}}

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  -- Simulate a configure script that writes prefix to a file
  local prefix_file = stage_dir .. "configured_prefix.txt"
  if envy.PLATFORM == "windows" then
    envy.run('Set-Content -Path configured_prefix.txt -Value "' .. install_dir .. '"', {{ shell = ENVY_SHELL.POWERSHELL }})
  else
    envy.run('echo "' .. install_dir .. '" > configured_prefix.txt')
  end

  -- Create a bin directory structure as if we ran make
  if envy.PLATFORM == "windows" then
    envy.run([[
      New-Item -ItemType Directory -Path staged_bin -Force | Out-Null
      Set-Content -Path staged_bin/mytool.txt -Value "tool_binary"
    ]], {{ shell = ENVY_SHELL.POWERSHELL }})
  else
    envy.run([[
      mkdir -p staged_bin
      echo "tool_binary" > staged_bin/mytool.txt
    ]])
  end
end

INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  -- Simulate make install: copy from stage to install_dir/bin
  if envy.PLATFORM == "windows" then
    envy.run([[
      New-Item -ItemType Directory -Path "]] .. install_dir .. [[bin" -Force | Out-Null
      Copy-Item -Path staged_bin/mytool.txt -Destination "]] .. install_dir .. [[bin/mytool.txt"
    ]], {{ shell = ENVY_SHELL.POWERSHELL }})
  else
    envy.run("mkdir -p " .. install_dir .. "bin && cp staged_bin/mytool.txt " .. install_dir .. "bin/")
  end

  -- Copy the configured prefix for verification
  envy.copy(stage_dir .. "configured_prefix.txt", install_dir .. "configured_prefix.txt")
end

PRODUCTS = {{ mytool = "bin/mytool.txt" }}
"""
        self.write_spec("build_prefix", spec)
        self.run_spec("build_prefix", "local.build_prefix@v1")

        pkg_path = self.get_pkg_path("local.build_prefix@v1")
        self.assertIsNotNone(pkg_path, "Package should be installed")

        # Verify the tool was installed
        tool_path = pkg_path / "bin" / "mytool.txt"
        self.assertTrue(tool_path.exists(), "Tool should be installed to bin/")

        # Verify BUILD's configured prefix points to install dir (which becomes pkg after completion)
        prefix_file = pkg_path / "configured_prefix.txt"
        self.assertTrue(prefix_file.exists(), "Prefix file should exist")
        configured_prefix = prefix_file.read_text().strip().rstrip("/").rstrip("\\")

        # install_dir is now pkg/ directly (no rename step)
        normalized_prefix = configured_prefix.replace("\\", "/")
        self.assertTrue(
            normalized_prefix.endswith("/pkg"),
            f"Configured prefix should end with /pkg, got: {configured_prefix}",
        )

        # install_dir and pkg_path should be the same directory
        normalized_pkg = str(pkg_path).rstrip("/").rstrip("\\").replace("\\", "/")
        self.assertEqual(
            normalized_prefix,
            normalized_pkg,
            f"install_dir and pkg_path should match: {normalized_prefix} vs {normalized_pkg}",
        )

    # =========================================================================
    # Fail-fast tests
    # =========================================================================

    def test_build_failfast_stops_on_first_error(self):
        """Multi-line BUILD string stops on first error (fail-fast behavior)."""
        # Shell script where 'false' fails, subsequent echo should NOT run
        spec = """IDENTITY = "local.build_failfast@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = {{strip = 1}}

BUILD = [[
echo "line1"
false
echo "line2_should_not_run" > failfast_marker.txt
]]
"""
        self.write_spec("failfast", spec)
        self.run_spec("failfast", "local.build_failfast@v1", should_succeed=False)

        identity_dir = self.cache_root / "packages" / "local.build_failfast@v1"
        marker_found = False
        if identity_dir.exists():
            for root, dirs, files in os.walk(identity_dir):
                if "failfast_marker.txt" in files:
                    marker_found = True
                    break

        self.assertFalse(
            marker_found,
            "failfast_marker.txt should NOT exist - fail-fast should have stopped execution",
        )


if __name__ == "__main__":
    unittest.main()
