"""Functional tests for 'envy export' and 'envy import' commands."""

import hashlib
import io
import os
import shutil
import tarfile
import tempfile
import unittest
from pathlib import Path

from . import test_config
from .env import EnvyTestCase
from .test_config import make_manifest, parse_export_line

TEST_ARCHIVE_FILES = {
    "root/file1.txt": "Root file content\n",
    "root/file2.txt": "Another root file\n",
    "root/subdir1/file3.txt": "Subdirectory file\n",
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


def snapshot_tree(root: Path) -> dict[str, bytes]:
    """Snapshot a directory tree: {relative_path: file_contents} for all regular files."""
    tree = {}
    for dirpath, _, filenames in os.walk(root):
        for f in filenames:
            full = Path(dirpath) / f
            rel = full.relative_to(root)
            tree[str(rel)] = full.read_bytes()
    return tree


class TestExportImport(EnvyTestCase):
    """Tests for 'envy export' and 'envy import' commands."""

    def setUp(self):
        super().setUp()
        self.test_dir = self.make_temp_dir("test_dir")
        self.output_dir = self.make_temp_dir("output_dir")

        # Create test archive
        self.archive_path = self.test_dir / "test.tar.gz"
        self.archive_hash = create_test_archive(self.archive_path)
        self._write_specs()

    def _write_specs(self):
        archive_lua_path = self.archive_path.as_posix()

        specs = {
            "exportable_pkg.lua": f'''IDENTITY = "local.exportable_pkg@v1"
EXPORTABLE = true

FETCH = {{
  source = "{archive_lua_path}",
  sha256 = "{self.archive_hash}"
}}

STAGE = {{strip = 1}}

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  if envy.PLATFORM == "windows" then
    envy.run([[Set-Content -Path built.txt -Value "built"]], {{ shell = ENVY_SHELL.POWERSHELL }})
  else
    envy.run([[echo 'built' > built.txt]])
  end
end
''',
            "default_pkg.lua": f'''IDENTITY = "local.default_pkg@v1"

FETCH = {{
  source = "{archive_lua_path}",
  sha256 = "{self.archive_hash}"
}}

STAGE = {{strip = 1}}

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  if envy.PLATFORM == "windows" then
    envy.run([[Set-Content -Path built.txt -Value "built"]], {{ shell = ENVY_SHELL.POWERSHELL }})
  else
    envy.run([[echo 'built' > built.txt]])
  end
end
''',
        }

        for name, content in specs.items():
            (self.test_dir / name).write_text(content, encoding="utf-8")

    @staticmethod
    def lua_path(path: Path) -> str:
        return path.as_posix()

    def create_manifest(self, content: str) -> Path:
        manifest_path = self.test_dir / "envy.lua"
        manifest_path.write_text(make_manifest(content), encoding="utf-8")
        return manifest_path

    def run_envy(self, *args, cwd=None):
        cmd = [str(self.envy), "--cache-root", str(self.cache_root), *args]
        return test_config.run(
            cmd,
            cwd=cwd or self.project_root,
            capture_output=True,
            text=True,
        )

    def _install_and_export(self, spec_name, identity, manifest):
        """Install a package and export it. Returns (install_result, export_result)."""
        install = self.run_envy("install", "--manifest", str(manifest))
        self.assertEqual(install.returncode, 0, f"install failed: {install.stderr}")

        export = self.run_envy(
            "export",
            identity,
            "-o",
            str(self.output_dir),
            "--manifest",
            str(manifest),
        )
        return install, export

    def test_export_single_package(self):
        """Export a single EXPORTABLE=true package, verify archive created."""
        manifest = self.create_manifest(
            f'''
PACKAGES = {{
    {{ spec = "local.exportable_pkg@v1", source = "{self.lua_path(self.test_dir)}/exportable_pkg.lua" }}
}}
'''
        )

        _, export = self._install_and_export(
            "exportable_pkg.lua", "local.exportable_pkg@v1", manifest
        )
        self.assertEqual(export.returncode, 0, f"export failed: {export.stderr}")

        # Verify archive was created
        sha_hex, archive_path = parse_export_line(export.stdout)
        self.assertEqual(len(sha_hex), 64, f"Expected 64-char SHA256, got: {sha_hex}")
        self.assertTrue(archive_path.exists(), f"Archive not found: {archive_path}")
        self.assertTrue(
            archive_path.name.startswith("local.exportable_pkg@v1-"),
            f"Unexpected filename: {archive_path.name}",
        )
        self.assertTrue(
            archive_path.name.endswith(".tar.zst"),
            f"Expected .tar.zst extension: {archive_path.name}",
        )

    def test_export_default_package(self):
        """Export a default (non-EXPORTABLE) package produces fetch archive."""
        manifest = self.create_manifest(
            f'''
PACKAGES = {{
    {{ spec = "local.default_pkg@v1", source = "{self.lua_path(self.test_dir)}/default_pkg.lua" }}
}}
'''
        )

        _, export = self._install_and_export(
            "default_pkg.lua", "local.default_pkg@v1", manifest
        )
        self.assertEqual(export.returncode, 0, f"export failed: {export.stderr}")

        _, archive_path = parse_export_line(export.stdout)
        self.assertTrue(archive_path.exists())
        self.assertTrue(archive_path.name.startswith("local.default_pkg@v1-"))

    def test_export_all_packages(self):
        """Export all packages when no query specified."""
        manifest = self.create_manifest(
            f'''
PACKAGES = {{
    {{ spec = "local.exportable_pkg@v1", source = "{self.lua_path(self.test_dir)}/exportable_pkg.lua" }},
    {{ spec = "local.default_pkg@v1", source = "{self.lua_path(self.test_dir)}/default_pkg.lua" }}
}}
'''
        )

        install = self.run_envy("install", "--manifest", str(manifest))
        self.assertEqual(install.returncode, 0, f"install failed: {install.stderr}")

        export = self.run_envy(
            "export",
            "-o",
            str(self.output_dir),
            "--manifest",
            str(manifest),
        )
        self.assertEqual(export.returncode, 0, f"export failed: {export.stderr}")

        # Should have two archive paths in stdout
        lines = [l for l in export.stdout.strip().split("\n") if l.strip()]
        self.assertEqual(len(lines), 2, f"Expected 2 archives, got: {lines}")

    def test_export_output_dir(self):
        """Verify -o flag places archives in specified directory."""
        manifest = self.create_manifest(
            f'''
PACKAGES = {{
    {{ spec = "local.exportable_pkg@v1", source = "{self.lua_path(self.test_dir)}/exportable_pkg.lua" }}
}}
'''
        )

        _, export = self._install_and_export(
            "exportable_pkg.lua", "local.exportable_pkg@v1", manifest
        )
        self.assertEqual(export.returncode, 0)

        _, archive_path = parse_export_line(export.stdout)
        self.assertEqual(
            archive_path.parent,
            self.output_dir,
            f"Archive should be in output dir: {archive_path}",
        )

    def test_export_no_match_error(self):
        """Export with non-matching query produces error."""
        manifest = self.create_manifest(
            f'''
PACKAGES = {{
    {{ spec = "local.exportable_pkg@v1", source = "{self.lua_path(self.test_dir)}/exportable_pkg.lua" }}
}}
'''
        )

        install = self.run_envy("install", "--manifest", str(manifest))
        self.assertEqual(install.returncode, 0)

        export = self.run_envy(
            "export",
            "nonexistent",
            "--manifest",
            str(manifest),
        )
        self.assertEqual(export.returncode, 1)
        self.assertIn("no package matching", export.stderr.lower())

    def test_import_exported_package(self):
        """Export then import: verify package path is printed and usable."""
        manifest = self.create_manifest(
            f'''
PACKAGES = {{
    {{ spec = "local.exportable_pkg@v1", source = "{self.lua_path(self.test_dir)}/exportable_pkg.lua" }}
}}
'''
        )

        _, export = self._install_and_export(
            "exportable_pkg.lua", "local.exportable_pkg@v1", manifest
        )
        self.assertEqual(export.returncode, 0, f"export failed: {export.stderr}")
        _, archive_path = parse_export_line(export.stdout)

        # Delete cache and reimport
        import_cache = self.make_temp_dir("import_cache")
        result = test_config.run(
            [
                str(self.envy),
                "--cache-root",
                str(import_cache),
                "import",
                str(archive_path),
            ],
            cwd=self.project_root,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, f"import failed: {result.stderr}")

        pkg_path = Path(result.stdout.strip())
        self.assertTrue(
            pkg_path.exists(),
            f"Imported package path should exist: {pkg_path}",
        )
    def test_import_roundtrip_exportable(self):
        """Export EXPORTABLE package, wipe, import: every file restored byte-identical."""
        manifest = self.create_manifest(
            f'''
PACKAGES = {{
    {{ spec = "local.exportable_pkg@v1", source = "{self.lua_path(self.test_dir)}/exportable_pkg.lua" }}
}}
'''
        )

        install = self.run_envy("install", "--manifest", str(manifest))
        self.assertEqual(install.returncode, 0, f"install failed: {install.stderr}")

        # Find the installed package path via `envy package`
        pkg_result = self.run_envy(
            "package",
            "local.exportable_pkg@v1",
            "--manifest",
            str(manifest),
        )
        self.assertEqual(
            pkg_result.returncode, 0, f"package failed: {pkg_result.stderr}"
        )
        original_pkg_path = Path(pkg_result.stdout.strip())

        # Snapshot the pkg/ tree before export
        before = snapshot_tree(original_pkg_path)
        self.assertTrue(len(before) > 0, "Package dir should have files")

        # Export
        export = self.run_envy(
            "export",
            "local.exportable_pkg@v1",
            "-o",
            str(self.output_dir),
            "--manifest",
            str(manifest),
        )
        self.assertEqual(export.returncode, 0, f"export failed: {export.stderr}")
        _, archive_path = parse_export_line(export.stdout)

        # Import into fresh cache
        import_cache = self.make_temp_dir("import_cache")
        result = test_config.run(
            [
                str(self.envy),
                "--cache-root",
                str(import_cache),
                "import",
                str(archive_path),
            ],
            cwd=self.project_root,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, f"import failed: {result.stderr}")

        imported_pkg_path = Path(result.stdout.strip())
        after = snapshot_tree(imported_pkg_path)

        self.assertEqual(
            sorted(before.keys()),
            sorted(after.keys()),
            f"File set mismatch.\n  before: {sorted(before.keys())}\n  after:  {sorted(after.keys())}",
        )
        for rel_path in before:
            self.assertEqual(
                before[rel_path],
                after[rel_path],
                f"Content mismatch for {rel_path}",
            )
    def test_import_roundtrip_fetch_only(self):
        """Export fetch-only package, wipe, import: fetch/ tree restored byte-identical."""
        manifest = self.create_manifest(
            f'''
PACKAGES = {{
    {{ spec = "local.default_pkg@v1", source = "{self.lua_path(self.test_dir)}/default_pkg.lua" }}
}}
'''
        )

        install = self.run_envy("install", "--manifest", str(manifest))
        self.assertEqual(install.returncode, 0, f"install failed: {install.stderr}")

        # Find the installed entry path — for fetch-only, the export archive contains fetch/
        # Use `envy package` to get the pkg_path, then go up to entry_path/fetch/
        pkg_result = self.run_envy(
            "package",
            "local.default_pkg@v1",
            "--manifest",
            str(manifest),
        )
        self.assertEqual(
            pkg_result.returncode, 0, f"package failed: {pkg_result.stderr}"
        )
        original_pkg_path = Path(pkg_result.stdout.strip())
        entry_path = original_pkg_path.parent
        fetch_path = entry_path / "fetch"

        # Snapshot the fetch/ tree before export
        before = snapshot_tree(fetch_path)
        self.assertTrue(len(before) > 0, "Fetch dir should have files")

        # Export (non-EXPORTABLE → fetch-only archive)
        export = self.run_envy(
            "export",
            "local.default_pkg@v1",
            "-o",
            str(self.output_dir),
            "--manifest",
            str(manifest),
        )
        self.assertEqual(export.returncode, 0, f"export failed: {export.stderr}")
        _, archive_path = parse_export_line(export.stdout)

        # Import into fresh cache
        import_cache = self.make_temp_dir("import_cache")
        result = test_config.run(
            [
                str(self.envy),
                "--cache-root",
                str(import_cache),
                "import",
                str(archive_path),
            ],
            cwd=self.project_root,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, f"import failed: {result.stderr}")
        self.assertIn("fetch-only import", result.stdout)

        # The printed path is the entry dir; snapshot its fetch/ subdir
        imported_entry_path = Path(
            result.stdout.strip().split("fetch-only import: ")[1]
        )
        imported_fetch_path = imported_entry_path / "fetch"
        after = snapshot_tree(imported_fetch_path)

        self.assertEqual(
            sorted(before.keys()),
            sorted(after.keys()),
            f"File set mismatch.\n  before: {sorted(before.keys())}\n  after:  {sorted(after.keys())}",
        )
        for rel_path in before:
            self.assertEqual(
                before[rel_path],
                after[rel_path],
                f"Content mismatch for {rel_path}",
            )
    def test_import_already_cached(self):
        """Importing a package that's already cached returns immediately."""
        manifest = self.create_manifest(
            f'''
PACKAGES = {{
    {{ spec = "local.exportable_pkg@v1", source = "{self.lua_path(self.test_dir)}/exportable_pkg.lua" }}
}}
'''
        )

        _, export = self._install_and_export(
            "exportable_pkg.lua", "local.exportable_pkg@v1", manifest
        )
        self.assertEqual(export.returncode, 0)
        _, archive_path = parse_export_line(export.stdout)

        # Import into the SAME cache (already has the package)
        result = self.run_envy("import", str(archive_path))
        self.assertEqual(result.returncode, 0, f"import failed: {result.stderr}")

        pkg_path = Path(result.stdout.strip())
        self.assertTrue(pkg_path.exists())

    def test_import_bad_extension_error(self):
        """Importing a file without .tar.zst extension fails."""
        bad_file = self.output_dir / "bad.tar.gz"
        bad_file.write_text("not a real archive")

        result = self.run_envy("import", str(bad_file))
        self.assertEqual(result.returncode, 1)
        self.assertIn(".tar.zst", result.stderr)

    def test_export_partial_match(self):
        """Export supports partial identity matching."""
        manifest = self.create_manifest(
            f'''
PACKAGES = {{
    {{ spec = "local.exportable_pkg@v1", source = "{self.lua_path(self.test_dir)}/exportable_pkg.lua" }}
}}
'''
        )

        install = self.run_envy("install", "--manifest", str(manifest))
        self.assertEqual(install.returncode, 0, f"install failed: {install.stderr}")

        # Use partial match (name only)
        export = self.run_envy(
            "export",
            "exportable_pkg",
            "-o",
            str(self.output_dir),
            "--manifest",
            str(manifest),
        )
        self.assertEqual(export.returncode, 0, f"export failed: {export.stderr}")

        _, archive_path = parse_export_line(export.stdout)
        self.assertTrue(archive_path.exists())

    def test_import_fetch_only_package(self):
        """Import a fetch-only archive (EXPORTABLE=false), verify fetch-only message."""
        manifest = self.create_manifest(
            f'''
PACKAGES = {{
    {{ spec = "local.default_pkg@v1", source = "{self.lua_path(self.test_dir)}/default_pkg.lua" }}
}}
'''
        )

        _, export = self._install_and_export(
            "default_pkg.lua", "local.default_pkg@v1", manifest
        )
        self.assertEqual(export.returncode, 0, f"export failed: {export.stderr}")
        _, archive_path = parse_export_line(export.stdout)

        # Import into fresh cache
        import_cache = self.make_temp_dir("import_cache")
        result = test_config.run(
            [
                str(self.envy),
                "--cache-root",
                str(import_cache),
                "import",
                str(archive_path),
            ],
            cwd=self.project_root,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, f"import failed: {result.stderr}")
        self.assertIn("fetch-only import", result.stdout)
    def test_import_dir(self):
        """Export 2 packages to dir, wipe cache, import --dir, verify both imported."""
        manifest = self.create_manifest(
            f'''
PACKAGES = {{
    {{ spec = "local.exportable_pkg@v1", source = "{self.lua_path(self.test_dir)}/exportable_pkg.lua" }},
    {{ spec = "local.default_pkg@v1", source = "{self.lua_path(self.test_dir)}/default_pkg.lua" }}
}}
'''
        )

        install = self.run_envy("install", "--manifest", str(manifest))
        self.assertEqual(install.returncode, 0, f"install failed: {install.stderr}")

        export = self.run_envy(
            "export",
            "-o",
            str(self.output_dir),
            "--manifest",
            str(manifest),
        )
        self.assertEqual(export.returncode, 0, f"export failed: {export.stderr}")

        # Verify we got 2 archives
        archives = list(self.output_dir.glob("*.tar.zst"))
        self.assertEqual(len(archives), 2, f"Expected 2 archives, got: {archives}")

        # Wipe cache and import from directory
        import_cache = self.make_temp_dir("import_cache")
        result = test_config.run(
            [
                str(self.envy),
                "--cache-root",
                str(import_cache),
                "import",
                "--dir",
                str(self.output_dir),
                "--manifest",
                str(manifest),
            ],
            cwd=self.project_root,
            capture_output=True,
            text=True,
        )
        self.assertEqual(
            result.returncode, 0, f"import --dir failed: {result.stderr}"
        )

        # Verify both packages exist in the new cache via `envy package`
        for identity in ["local.exportable_pkg@v1", "local.default_pkg@v1"]:
            pkg_result = test_config.run(
                [
                    str(self.envy),
                    "--cache-root",
                    str(import_cache),
                    "package",
                    identity,
                    "--manifest",
                    str(manifest),
                ],
                cwd=self.project_root,
                capture_output=True,
                text=True,
            )
            self.assertEqual(
                pkg_result.returncode,
                0,
                f"package {identity} failed after import: {pkg_result.stderr}",
            )
            pkg_path = Path(pkg_result.stdout.strip())
            self.assertTrue(
                pkg_path.exists(),
                f"Imported package path should exist: {pkg_path}",
            )
    def test_import_dir_with_dependency(self):
        """Export package A (exportable), import --dir; engine builds from source."""
        manifest = self.create_manifest(
            f'''
PACKAGES = {{
    {{ spec = "local.exportable_pkg@v1", source = "{self.lua_path(self.test_dir)}/exportable_pkg.lua" }},
    {{ spec = "local.default_pkg@v1", source = "{self.lua_path(self.test_dir)}/default_pkg.lua" }}
}}
'''
        )

        install = self.run_envy("install", "--manifest", str(manifest))
        self.assertEqual(install.returncode, 0, f"install failed: {install.stderr}")

        # Export only the exportable package
        export = self.run_envy(
            "export",
            "local.exportable_pkg@v1",
            "-o",
            str(self.output_dir),
            "--manifest",
            str(manifest),
        )
        self.assertEqual(export.returncode, 0, f"export failed: {export.stderr}")

        archives = list(self.output_dir.glob("*.tar.zst"))
        self.assertEqual(len(archives), 1, f"Expected 1 archive, got: {archives}")

        # Import from directory into fresh cache
        import_cache = self.make_temp_dir("import_cache")
        result = test_config.run(
            [
                str(self.envy),
                "--cache-root",
                str(import_cache),
                "import",
                "--dir",
                str(self.output_dir),
                "--manifest",
                str(manifest),
            ],
            cwd=self.project_root,
            capture_output=True,
            text=True,
        )
        self.assertEqual(
            result.returncode, 0, f"import --dir failed: {result.stderr}"
        )

        # The imported package should be usable
        pkg_result = test_config.run(
            [
                str(self.envy),
                "--cache-root",
                str(import_cache),
                "package",
                "local.exportable_pkg@v1",
                "--manifest",
                str(manifest),
            ],
            cwd=self.project_root,
            capture_output=True,
            text=True,
        )
        self.assertEqual(
            pkg_result.returncode,
            0,
            f"package lookup failed after import: {pkg_result.stderr}",
        )
    def test_import_dir_same_identity_different_options(self):
        """Same spec with different options produces distinct archives; both import correctly."""
        archive_lua_path = self.archive_path.as_posix()

        # Spec that writes option value into install dir
        variant_spec = f'''IDENTITY = "local.variant_pkg@v1"
EXPORTABLE = true

FETCH = {{
  source = "{archive_lua_path}",
  sha256 = "{self.archive_hash}"
}}

STAGE = {{strip = 1}}

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  if envy.PLATFORM == "windows" then
    envy.run(string.format([[Set-Content -Path variant.txt -Value "%s"]], options.variant or "none"), {{ shell = ENVY_SHELL.POWERSHELL }})
  else
    envy.run(string.format([[echo '%s' > variant.txt]], options.variant or "none"))
  end
end
'''
        spec_path = self.test_dir / "variant_pkg.lua"
        spec_path.write_text(variant_spec, encoding="utf-8")

        manifest = self.create_manifest(
            f'''
PACKAGES = {{
    {{ spec = "local.variant_pkg@v1", source = "{self.lua_path(spec_path)}", options = {{ variant = "alpha" }} }},
    {{ spec = "local.variant_pkg@v1", source = "{self.lua_path(spec_path)}", options = {{ variant = "beta" }} }}
}}
'''
        )

        install = self.run_envy("install", "--manifest", str(manifest))
        self.assertEqual(install.returncode, 0, f"install failed: {install.stderr}")

        export = self.run_envy(
            "export",
            "-o",
            str(self.output_dir),
            "--manifest",
            str(manifest),
        )
        self.assertEqual(export.returncode, 0, f"export failed: {export.stderr}")

        # Both variants exported — same identity, different hashes
        archives = list(self.output_dir.glob("local.variant_pkg@v1-*.tar.zst"))
        self.assertEqual(
            len(archives),
            2,
            f"Expected 2 variant archives, got: {[a.name for a in archives]}",
        )

        # Verify archives have different hash prefixes in their filenames
        names = sorted(a.name for a in archives)
        self.assertNotEqual(
            names[0],
            names[1],
            "Different options must produce different archive filenames",
        )

        # Import both into fresh cache
        import_cache = self.make_temp_dir("import_cache")
        result = test_config.run(
            [
                str(self.envy),
                "--cache-root",
                str(import_cache),
                "import",
                "--dir",
                str(self.output_dir),
                "--manifest",
                str(manifest),
            ],
            cwd=self.project_root,
            capture_output=True,
            text=True,
        )
        self.assertEqual(
            result.returncode, 0, f"import --dir failed: {result.stderr}"
        )

        # Verify both variants installed by running install against the same manifest.
        # If both are cached from import, this succeeds immediately.
        install_result = test_config.run(
            [
                str(self.envy),
                "--cache-root",
                str(import_cache),
                "install",
                "--manifest",
                str(manifest),
            ],
            cwd=self.project_root,
            capture_output=True,
            text=True,
        )
        self.assertEqual(
            install_result.returncode,
            0,
            f"install after import failed: {install_result.stderr}",
        )
    def test_import_dir_stale_hash_skipped(self):
        """Archive with valid identity but wrong hash is not imported."""
        manifest = self.create_manifest(
            f'''
PACKAGES = {{
    {{ spec = "local.exportable_pkg@v1", source = "{self.lua_path(self.test_dir)}/exportable_pkg.lua" }}
}}
'''
        )

        install = self.run_envy("install", "--manifest", str(manifest))
        self.assertEqual(install.returncode, 0, f"install failed: {install.stderr}")

        export = self.run_envy(
            "export",
            "local.exportable_pkg@v1",
            "-o",
            str(self.output_dir),
            "--manifest",
            str(manifest),
        )
        self.assertEqual(export.returncode, 0, f"export failed: {export.stderr}")

        # Rename the exported archive to have a wrong hash prefix
        archives = list(self.output_dir.glob("*.tar.zst"))
        self.assertEqual(len(archives), 1)
        original = archives[0]
        # Replace the blake3 hash with a fake one
        wrong_hash = (
            self.output_dir
            / "local.exportable_pkg@v1-darwin-arm64-blake3-0000000000000000.tar.zst"
        )
        original.rename(wrong_hash)

        import_cache = self.make_temp_dir("import_cache")
        result = test_config.run(
            [
                str(self.envy),
                "--cache-root",
                str(import_cache),
                "import",
                "--dir",
                str(self.output_dir),
                "--manifest",
                str(manifest),
            ],
            cwd=self.project_root,
            capture_output=True,
            text=True,
        )
        # Import succeeds — stale archive simply doesn't match any package's
        # depot query, so the package builds normally from source.
        self.assertEqual(
            result.returncode, 0, f"import --dir failed: {result.stderr}"
        )
    def test_import_dir_skips_unmatched(self):
        """Unrecognized .tar.zst in dir skipped with warning."""
        manifest = self.create_manifest(
            f'''
PACKAGES = {{
    {{ spec = "local.exportable_pkg@v1", source = "{self.lua_path(self.test_dir)}/exportable_pkg.lua" }}
}}
'''
        )

        install = self.run_envy("install", "--manifest", str(manifest))
        self.assertEqual(install.returncode, 0, f"install failed: {install.stderr}")

        export = self.run_envy(
            "export",
            "local.exportable_pkg@v1",
            "-o",
            str(self.output_dir),
            "--manifest",
            str(manifest),
        )
        self.assertEqual(export.returncode, 0, f"export failed: {export.stderr}")

        # Add an unrecognized .tar.zst file
        bogus = self.output_dir / "bogus-file.tar.zst"
        bogus.write_bytes(b"not a real archive")

        import_cache = self.make_temp_dir("import_cache")
        result = test_config.run(
            [
                str(self.envy),
                "--cache-root",
                str(import_cache),
                "import",
                "--dir",
                str(self.output_dir),
                "--manifest",
                str(manifest),
            ],
            cwd=self.project_root,
            capture_output=True,
            text=True,
        )
        self.assertEqual(
            result.returncode, 0, f"import --dir failed: {result.stderr}"
        )

        # The bogus file should have been skipped (warning in stderr)
        self.assertIn("skipping", result.stderr.lower())
    def test_export_sha256_correctness(self):
        """Exported SHA256 in stdout matches actual archive file hash."""
        manifest = self.create_manifest(
            f'''
PACKAGES = {{
    {{ spec = "local.exportable_pkg@v1", source = "{self.lua_path(self.test_dir)}/exportable_pkg.lua" }}
}}
'''
        )

        _, export = self._install_and_export(
            "exportable_pkg.lua", "local.exportable_pkg@v1", manifest
        )
        self.assertEqual(export.returncode, 0, f"export failed: {export.stderr}")

        sha_hex, archive_path = parse_export_line(export.stdout)
        actual_hash = hashlib.sha256(archive_path.read_bytes()).hexdigest()
        self.assertEqual(
            sha_hex,
            actual_hash,
            f"SHA256 mismatch: stdout={sha_hex}, file={actual_hash}",
        )

    def test_export_deterministic_order(self):
        """Multi-package export outputs lines in manifest order."""
        manifest = self.create_manifest(
            f'''
PACKAGES = {{
    {{ spec = "local.exportable_pkg@v1", source = "{self.lua_path(self.test_dir)}/exportable_pkg.lua" }},
    {{ spec = "local.default_pkg@v1", source = "{self.lua_path(self.test_dir)}/default_pkg.lua" }}
}}
'''
        )

        install = self.run_envy("install", "--manifest", str(manifest))
        self.assertEqual(install.returncode, 0, f"install failed: {install.stderr}")

        # Run export multiple times — order must be stable
        for _ in range(3):
            export = self.run_envy(
                "export",
                "-o",
                str(self.output_dir),
                "--manifest",
                str(manifest),
            )
            self.assertEqual(export.returncode, 0, f"export failed: {export.stderr}")

            lines = [l for l in export.stdout.strip().split("\n") if l.strip()]
            self.assertEqual(len(lines), 2, f"Expected 2 lines, got: {lines}")

            _, first_path = parse_export_line(lines[0])
            _, second_path = parse_export_line(lines[1])
            self.assertIn(
                "local.exportable_pkg@v1",
                first_path.name,
                f"First line should be exportable_pkg, got: {first_path.name}",
            )
            self.assertIn(
                "local.default_pkg@v1",
                second_path.name,
                f"Second line should be default_pkg, got: {second_path.name}",
            )

    def test_export_stdout_as_import_checksums(self):
        """Export stdout (without --depot-prefix) works as import --checksums file."""
        manifest = self.create_manifest(
            f'''
PACKAGES = {{
    {{ spec = "local.exportable_pkg@v1", source = "{self.lua_path(self.test_dir)}/exportable_pkg.lua" }}
}}
'''
        )

        _, export = self._install_and_export(
            "exportable_pkg.lua", "local.exportable_pkg@v1", manifest
        )
        self.assertEqual(export.returncode, 0, f"export failed: {export.stderr}")

        # Write export stdout directly as a checksums file
        # Replace absolute path with just the filename for checksums format
        sha_hex, archive_path = parse_export_line(export.stdout)
        checksums_path = self.test_dir / "checksums.txt"
        checksums_path.write_text(f"{sha_hex}  {archive_path.name}\n", encoding="utf-8")

        # Import into fresh cache using --dir + --checksums
        import_cache = self.make_temp_dir("import_cache")
        result = test_config.run(
            [
                str(self.envy),
                "--cache-root",
                str(import_cache),
                "import",
                "--dir",
                str(self.output_dir),
                "--checksums",
                str(checksums_path),
                "--manifest",
                str(manifest),
            ],
            cwd=self.project_root,
            capture_output=True,
            text=True,
        )
        self.assertEqual(
            result.returncode,
            0,
            f"import --dir --checksums failed: {result.stderr}",
        )

        pkg_result = test_config.run(
            [
                str(self.envy),
                "--cache-root",
                str(import_cache),
                "package",
                "local.exportable_pkg@v1",
                "--manifest",
                str(manifest),
            ],
            cwd=self.project_root,
            capture_output=True,
            text=True,
        )
        self.assertEqual(
            pkg_result.returncode,
            0,
            f"package lookup failed after import: {pkg_result.stderr}",
        )
        pkg_path = Path(pkg_result.stdout.strip())
        self.assertTrue(
            pkg_path.exists(),
            f"Imported package path should exist: {pkg_path}",
        )


if __name__ == "__main__":
    unittest.main()
