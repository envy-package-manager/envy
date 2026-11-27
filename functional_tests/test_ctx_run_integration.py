"""Functional tests for envy.run() integration scenarios.

Tests integration with other envy functions and phase-specific usage.
"""

import unittest

from .test_ctx_run_base import CtxRunTestBase


class TestCtxRunIntegration(CtxRunTestBase):
    """Tests for envy.run() integration with other operations."""

    def test_with_extract(self):
        """envy.run() works with envy.extract_all()."""
        # Run after extract_all
        spec = """-- Test envy.run() mixed with envy.extract_all()
IDENTITY = "local.ctx_run_with_extract@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  -- Extract first
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})

  if envy.PLATFORM == "windows" then
      envy.run([[
        Get-ChildItem -Name | Set-Content -Path extracted_files.txt
        if (Test-Path file1.txt) {{ Set-Content -Path verify_extract.txt -Value "Extraction verified" }} else {{ exit 52 }}
        Add-Content -Path file1.txt -Value "Modified by ctx.run"
      ]], {{ shell = ENVY_SHELL.POWERSHELL }})
  else
    envy.run([[
      ls > extracted_files.txt
      test -f file1.txt && echo "Extraction verified" > verify_extract.txt
    ]])

    envy.run([[
      echo "Modified by ctx.run" >> file1.txt
    ]])
  end
end
"""
        self.write_spec("ctx_run_with_extract.lua", spec)
        self.run_spec("local.ctx_run_with_extract@v1", "ctx_run_with_extract.lua")
        pkg_path = self.get_pkg_path("local.ctx_run_with_extract@v1")
        assert pkg_path
        self.assertTrue((pkg_path / "verify_extract.txt").exists())

    def test_with_rename(self):
        """envy.run() works with file rename operations."""
        # Move/rename files via shell
        spec = """-- Test envy.run() mixed with envy.rename()
IDENTITY = "local.ctx_run_with_rename@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})

  if envy.PLATFORM == "windows" then
    envy.run([[
      Set-Content -Path original.txt -Value "original name"
    ]], {{ shell = ENVY_SHELL.POWERSHELL }})

    envy.run([[
      Move-Item -Path original.txt -Destination renamed.txt -Force
    ]], {{ shell = ENVY_SHELL.POWERSHELL }})

    envy.run([[
      if (Test-Path renamed.txt) {{ Set-Content -Path rename_check.txt -Value "Rename verified" }}
      if (-not (Test-Path original.txt)) {{ Add-Content -Path rename_check.txt -Value "Original gone" }}
      if (-not (Test-Path rename_check.txt)) {{ Write-Error "rename_check missing"; exit 1 }}
      exit 0
    ]], {{ shell = ENVY_SHELL.POWERSHELL }})
  else
    envy.run([[
      echo "original name" > original.txt
    ]])

    envy.run([[
      mv original.txt renamed.txt
    ]])

    envy.run([[
      test -f renamed.txt && echo "Rename verified" > rename_check.txt
      test ! -f original.txt && echo "Original gone" >> rename_check.txt
    ]])
  end
end
"""
        self.write_spec("ctx_run_with_rename.lua", spec)
        self.run_spec("local.ctx_run_with_rename@v1", "ctx_run_with_rename.lua")
        pkg_path = self.get_pkg_path("local.ctx_run_with_rename@v1")
        assert pkg_path
        self.assertTrue((pkg_path / "rename_check.txt").exists())

    def test_with_template(self):
        """envy.run() passes escaped braces to a shell-side template.

        The 8 braces are envy's `{{{{name}}}}` escape, doubled again for str.format.
        """
        spec = """-- Test envy.run() with a shell-side template
IDENTITY = "local.ctx_run_with_template@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})

  if envy.PLATFORM == "windows" then
    envy.run([[
      Set-Content -Path greeting.tmpl -Value "Hello {{{{{{{{name}}}}}}}}, you are {{{{{{{{age}}}}}}}} years old"
    ]], {{ shell = ENVY_SHELL.POWERSHELL }})

    envy.run([[
      $content = Get-Content greeting.tmpl
      $content = $content -replace "{{{{{{{{name}}}}}}}}","Alice" -replace "{{{{{{{{age}}}}}}}}","30"
      Set-Content -Path greeting.txt -Value $content
    ]], {{ shell = ENVY_SHELL.POWERSHELL }})

    envy.run([[
      if ((Select-String -Path greeting.txt -Pattern "Alice")) {{ Set-Content -Path template_check.txt -Value "Alice" }}
      if ((Select-String -Path greeting.txt -Pattern "30")) {{ Add-Content -Path template_check.txt -Value "30" }}
    ]], {{ shell = ENVY_SHELL.POWERSHELL }})
  else
    envy.run([[
      echo "Hello {{{{{{{{name}}}}}}}}, you are {{{{{{{{age}}}}}}}} years old" > greeting.tmpl
    ]])

    envy.run([[
      name="Alice"
      age="30"
      sed "s/{{{{{{{{name}}}}}}}}/$name/g; s/{{{{{{{{age}}}}}}}}/$age/g" greeting.tmpl > greeting.txt
    ]])

    envy.run([[
      grep "Alice" greeting.txt > template_check.txt
      grep "30" greeting.txt >> template_check.txt
    ]])
  end
end
"""
        self.write_spec("ctx_run_with_template.lua", spec)
        self.run_spec("local.ctx_run_with_template@v1", "ctx_run_with_template.lua")
        pkg_path = self.get_pkg_path("local.ctx_run_with_template@v1")
        assert pkg_path
        self.assertTrue((pkg_path / "template_check.txt").exists())
        self.assertEqual(  # the shell received `{{name}}`
            "Hello {name}, you are {age} years old".format(name="{{name}}", age="{{age}}"),
            (pkg_path / "greeting.tmpl").read_text(encoding="utf-8").strip(),
        )

    def test_multiple_calls(self):
        """Multiple envy.run() calls work in sequence."""
        # Multiple separate run calls
        spec = """-- Test multiple envy.run() calls in sequence
IDENTITY = "local.ctx_run_multiple_calls@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})

  if envy.PLATFORM == "windows" then
    envy.run([[
      Set-Content -Path call1.txt -Value "Call 1"
    ]], {{ shell = ENVY_SHELL.POWERSHELL }})
    envy.run([[
      Set-Content -Path call2.txt -Value "Call 2"
    ]], {{ shell = ENVY_SHELL.POWERSHELL }})
    envy.run([[
      Set-Content -Path call3.txt -Value "Call 3"
    ]], {{ shell = ENVY_SHELL.POWERSHELL }})
    envy.run([[
      Get-Content call1.txt, call2.txt, call3.txt | Set-Content -Path all_calls.txt
    ]], {{ shell = ENVY_SHELL.POWERSHELL }})
  else
    envy.run([[
      echo "Call 1" > call1.txt
    ]])
    envy.run([[
      echo "Call 2" > call2.txt
    ]])
    envy.run([[
      echo "Call 3" > call3.txt
    ]])
    envy.run([[
      cat call1.txt call2.txt call3.txt > all_calls.txt
    ]])
  end
end
"""
        self.write_spec("ctx_run_multiple_calls.lua", spec)
        self.run_spec("local.ctx_run_multiple_calls@v1", "ctx_run_multiple_calls.lua")
        pkg_path = self.get_pkg_path("local.ctx_run_multiple_calls@v1")
        assert pkg_path
        self.assertTrue((pkg_path / "all_calls.txt").exists())

    def test_interleaved(self):
        """envy.run() interleaves with other operations."""
        # Interleaved run calls with other ops
        spec = """-- Test envy.run() interleaved with other operations
IDENTITY = "local.ctx_run_interleaved@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  -- Extract
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})

  if envy.PLATFORM == "windows" then
    envy.run([[
      Set-Content -Path steps.txt -Value "Step 1"
    ]], {{ shell = ENVY_SHELL.POWERSHELL }})

    envy.run([[
      if (Test-Path test.txt) {{
        Move-Item test.txt test_renamed.txt -Force
      }}
    ]], {{ shell = ENVY_SHELL.POWERSHELL }})

    envy.run([[
      Add-Content -Path steps.txt -Value "Step 2"
      Get-ChildItem | Select-Object -ExpandProperty Name | Set-Content -Path file_list.txt
    ]], {{ shell = ENVY_SHELL.POWERSHELL }})

    envy.run([[
      Set-Content -Path version.tmpl -Value "Version: {{{{{{{{version}}}}}}}}"
      $content = Get-Content version.tmpl
      $content = $content -replace "{{{{{{{{version}}}}}}}}", "1.0"
      Set-Content -Path version.txt -Value $content
    ]], {{ shell = ENVY_SHELL.POWERSHELL }})

    envy.run([[
      Add-Content -Path steps.txt -Value "Step 3"
      $count = (Get-Content steps.txt | Measure-Object -Line).Lines
      Set-Content -Path step_count.txt -Value $count
    ]], {{ shell = ENVY_SHELL.POWERSHELL }})
  else
    envy.run([[
      echo "Step 1" > steps.txt
    ]])

    envy.run([[
      if [ -f test.txt ]; then
        mv test.txt test_renamed.txt
      fi
    ]])

    envy.run([[
      echo "Step 2" >> steps.txt
      ls > file_list.txt
    ]])

    envy.run([[
      echo "Version: {{{{{{{{version}}}}}}}}" > version.tmpl
      version="1.0"
      sed "s/{{{{{{{{version}}}}}}}}/$version/g" version.tmpl > version.txt
    ]])

    envy.run([[
      echo "Step 3" >> steps.txt
      wc -l steps.txt > step_count.txt
    ]])
  end
end
"""
        self.write_spec("ctx_run_interleaved.lua", spec)
        self.run_spec("local.ctx_run_interleaved@v1", "ctx_run_interleaved.lua")
        pkg_path = self.get_pkg_path("local.ctx_run_interleaved@v1")
        assert pkg_path
        self.assertTrue((pkg_path / "step_count.txt").exists())


class TestCtxRunPhaseSpecific(CtxRunTestBase):
    """Tests for envy.run() in specific phase scenarios."""

    def test_stage_build_prep(self):
        """envy.run() in stage for build preparation."""
        # Create build directory structure
        spec = """-- Test envy.run() in stage for build preparation
IDENTITY = "local.ctx_run_stage_build_prep@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})

  if envy.PLATFORM == "windows" then
    envy.run([[
      New-Item -ItemType Directory -Force -Path build | Out-Null
      Push-Location build
      Set-Content -Path config.txt -Value "# Build configuration"
      Add-Content -Path config.txt -Value "CFLAGS=-O2"
      Pop-Location
      if (-not (Test-Path build/config.txt)) {{ exit 1 }}
      exit 0
    ]], {{ shell = ENVY_SHELL.POWERSHELL }})
  else
    envy.run([[
      mkdir -p build
      cd build
      echo "# Build configuration" > config.txt
      echo "CFLAGS=-O2" >> config.txt
    ]])
  end
end
"""
        self.write_spec("ctx_run_stage_build_prep.lua", spec)
        self.run_spec(
            "local.ctx_run_stage_build_prep@v1", "ctx_run_stage_build_prep.lua"
        )
        pkg_path = self.get_pkg_path("local.ctx_run_stage_build_prep@v1")
        assert pkg_path
        self.assertTrue((pkg_path / "build" / "config.txt").exists())

    def test_stage_patch(self):
        """envy.run() in stage for patching."""
        # Apply patches via sed/string replacement
        spec = """-- Test envy.run() in stage for patching
IDENTITY = "local.ctx_run_stage_patch@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})

  if envy.PLATFORM == "windows" then
    envy.run([[
      Set-Content -Path patch_log.txt -Value "Patching file"
      Set-Content -Path temp.txt -Value "old content"
      (Get-Content temp.txt) -replace "old","new" | Set-Content -Path temp.txt
      Add-Content -Path patch_log.txt -Value "Patch applied"
    ]], {{ shell = ENVY_SHELL.POWERSHELL }})
  else
    envy.run([[
      echo "Patching file" > patch_log.txt
      echo "old content" > temp.txt
      sed 's/old/new/g' temp.txt > temp.txt.patched
      mv temp.txt.patched temp.txt
      echo "Patch applied" >> patch_log.txt
    ]])
  end
end
"""
        self.write_spec("ctx_run_stage_patch.lua", spec)
        self.run_spec("local.ctx_run_stage_patch@v1", "ctx_run_stage_patch.lua")
        pkg_path = self.get_pkg_path("local.ctx_run_stage_patch@v1")
        assert pkg_path
        self.assertTrue((pkg_path / "patch_log.txt").exists())

    def test_stage_permissions(self):
        """envy.run() in stage for setting permissions."""
        # Set file permissions
        spec = """-- Test envy.run() in stage for setting permissions
IDENTITY = "local.ctx_run_stage_permissions@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})

  if envy.PLATFORM == "windows" then
    envy.run([[
      (Get-Item file1.txt).Attributes = 'Normal'
      Add-Content -Path permissions.txt -Value ((Get-Item file1.txt).Attributes)
      Set-Content -Path executable.bat -Value "@echo off"
      (Get-Item executable.bat).Attributes = 'Normal'
      Add-Content -Path permissions.txt -Value ((Get-Item executable.bat).Attributes)
    ]], {{ shell = ENVY_SHELL.POWERSHELL }})
  else
    envy.run([[
      chmod +x file1.txt
      ls -l file1.txt > permissions.txt
      touch executable.sh
      chmod 755 executable.sh
      ls -l executable.sh >> permissions.txt
    ]])
  end
end
"""
        self.write_spec("ctx_run_stage_permissions.lua", spec)
        self.run_spec(
            "local.ctx_run_stage_permissions@v1", "ctx_run_stage_permissions.lua"
        )
        pkg_path = self.get_pkg_path("local.ctx_run_stage_permissions@v1")
        assert pkg_path
        self.assertTrue((pkg_path / "permissions.txt").exists())

    def test_stage_verification(self):
        """envy.run() in stage for verification checks."""
        # Verify stage contents
        spec = """-- Test envy.run() in stage for verification checks
IDENTITY = "local.ctx_run_stage_verification@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})

  if envy.PLATFORM == "windows" then
    envy.run([[
      if (-not (Test-Path file1.txt)) {{ throw "Missing file1.txt" }}
      if ((Get-Item file1.txt).Length -eq 0) {{ throw "File is empty" }}
      Set-Content -Path verification.txt -Value "All verification checks passed"
      if (-not (Test-Path verification.txt)) {{ throw "verification.txt missing post write" }}
      exit 0
    ]], {{ shell = ENVY_SHELL.POWERSHELL }})
  else
    envy.run([[
      test -f file1.txt || (echo "Missing file1.txt" && exit 1)
      test -s file1.txt || (echo "File is empty" && exit 1)
      echo "All verification checks passed" > verification.txt
    ]])
  end
end
"""
        self.write_spec("ctx_run_stage_verification.lua", spec)
        self.run_spec(
            "local.ctx_run_stage_verification@v1", "ctx_run_stage_verification.lua"
        )
        pkg_path = self.get_pkg_path("local.ctx_run_stage_verification@v1")
        assert pkg_path
        self.assertTrue((pkg_path / "verification.txt").exists())

    def test_stage_generation(self):
        """envy.run() in stage for code generation."""
        # Generate code files
        spec = """-- Test envy.run() in stage for code generation
IDENTITY = "local.ctx_run_stage_generation@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})

  if envy.PLATFORM == "windows" then
    envy.run([[
$header = @"
#ifndef GENERATED_H
#define GENERATED_H
#define VERSION \\"1.0.0\\"
#endif
"@
Set-Content -Path generated.h -Value $header
Set-Content -Path generated.bat -Value "@echo off`necho Generated script"
# Also produce generated.sh for test parity
Set-Content -Path generated.sh -Value "echo Generated script"
    ]], {{ shell = ENVY_SHELL.POWERSHELL }})
  else
    envy.run([[
cat > generated.h <<EOF
#ifndef GENERATED_H
#define GENERATED_H
#define VERSION "1.0.0"
#endif
EOF

cat > generated.sh <<'SCRIPT'
#!/usr/bin/env bash
echo "Generated script"
SCRIPT
chmod +x generated.sh
    ]])
  end
end
"""
        self.write_spec("ctx_run_stage_generation.lua", spec)
        self.run_spec(
            "local.ctx_run_stage_generation@v1", "ctx_run_stage_generation.lua"
        )
        pkg_path = self.get_pkg_path("local.ctx_run_stage_generation@v1")
        assert pkg_path
        self.assertTrue((pkg_path / "generated.h").exists())
        self.assertTrue((pkg_path / "generated.sh").exists())

    def test_stage_cleanup(self):
        """envy.run() in stage for cleanup operations."""
        # Clean up temp files
        spec = """-- Test envy.run() in stage for cleanup operations
IDENTITY = "local.ctx_run_stage_cleanup@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})

  if envy.PLATFORM == "windows" then
    envy.run([[
      Get-ChildItem -Recurse -Filter *.bak | Remove-Item -Force -ErrorAction SilentlyContinue
      Get-ChildItem -Recurse -Filter *.tmp | Remove-Item -Force -ErrorAction SilentlyContinue
      Get-ChildItem -Recurse -Directory | Where-Object {{ $_.GetFileSystemInfos().Count -eq 0 }} | Remove-Item -Force -ErrorAction SilentlyContinue
      Set-Content -Path cleanup_log.txt -Value "Cleanup complete"
    ]], {{ shell = ENVY_SHELL.POWERSHELL }})
  else
    envy.run([[
      find . -name "*.bak" -delete
      rm -f *.tmp
      find . -type d -empty -delete
      echo "Cleanup complete" > cleanup_log.txt
    ]])
  end
end
"""
        self.write_spec("ctx_run_stage_cleanup.lua", spec)
        self.run_spec("local.ctx_run_stage_cleanup@v1", "ctx_run_stage_cleanup.lua")
        pkg_path = self.get_pkg_path("local.ctx_run_stage_cleanup@v1")
        assert pkg_path
        self.assertTrue((pkg_path / "cleanup_log.txt").exists())

    def test_stage_compilation(self):
        """envy.run() in stage for simple compilation."""
        # Compile source file (mock)
        spec = """-- Test envy.run() in stage for simple compilation
IDENTITY = "local.ctx_run_stage_compilation@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})

  if envy.PLATFORM == "windows" then
    envy.run([[
$source = @'
#include <stdio.h>
int main() {{ printf("Hello\\n"); return 0; }}
'@
Set-Content -Path hello.c -Value $source
Set-Content -Path compile_log.txt -Value "Compiling hello.c..."
Add-Content -Path compile_log.txt -Value "Compilation successful"
    ]], {{ shell = ENVY_SHELL.POWERSHELL }})
  else
    envy.run([[
cat > hello.c <<'EOF'
#include <stdio.h>
int main() {{ printf("Hello\\n"); return 0; }}
EOF
echo "Compiling hello.c..." > compile_log.txt
echo "Compilation successful" >> compile_log.txt
    ]])
  end
end
"""
        self.write_spec("ctx_run_stage_compilation.lua", spec)
        self.run_spec(
            "local.ctx_run_stage_compilation@v1", "ctx_run_stage_compilation.lua"
        )
        pkg_path = self.get_pkg_path("local.ctx_run_stage_compilation@v1")
        assert pkg_path
        self.assertTrue((pkg_path / "compile_log.txt").exists())

    def test_stage_archiving(self):
        """envy.run() in stage for creating archives."""
        # Create archive
        spec = """-- Test envy.run() in stage for creating archives
IDENTITY = "local.ctx_run_stage_archiving@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}"
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})

  -- Create archives
  if envy.PLATFORM == "windows" then
    envy.run([[
      New-Item -ItemType Directory -Force -Path "archive_test/subdir" | Out-Null
      Set-Content -Path archive_test/file1.txt -Value "file1"
      Set-Content -Path archive_test/subdir/file2.txt -Value "file2"
      Compress-Archive -Path archive_test -DestinationPath archive_test.zip -Force
      if (Test-Path archive_test.zip) {{
        Set-Content -Path archive_log.txt -Value "Archive created"
      }} else {{
        exit 1
      }}
      if (-not (Test-Path archive_log.txt)) {{ exit 1 }}
      exit 0
    ]], {{ shell = ENVY_SHELL.POWERSHELL }})
  else
    envy.run([[
      # Create a directory structure
      mkdir -p archive_test/subdir
      echo "file1" > archive_test/file1.txt
      echo "file2" > archive_test/subdir/file2.txt

      # Create tar archive
      tar czf archive_test.tar.gz archive_test/

      # Verify archive was created
      test -f archive_test.tar.gz && echo "Archive created" > archive_log.txt
    ]])
  end
end
"""
        self.write_spec("ctx_run_stage_archiving.lua", spec)
        self.run_spec("local.ctx_run_stage_archiving@v1", "ctx_run_stage_archiving.lua")
        pkg_path = self.get_pkg_path("local.ctx_run_stage_archiving@v1")
        assert pkg_path
        self.assertTrue((pkg_path / "archive_log.txt").exists())


if __name__ == "__main__":
    unittest.main()
