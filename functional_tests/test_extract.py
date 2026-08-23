from __future__ import annotations

import bz2
import gzip
import io
import lzma
import os
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path

from . import test_config

# Test file structure: root/{file1.txt, file2.txt, subdir1/{file3.txt, nested/file4.txt}, subdir2/file5.txt}
TEST_FILES = {
    "root/file1.txt": "Root file content\n",
    "root/file2.txt": "Another root file\n",
    "root/subdir1/file3.txt": "Subdirectory file\n",
    "root/subdir1/nested/file4.txt": "Nested file content\n",
    "root/subdir2/file5.txt": "Second subdir file\n",
}


def create_tar_archive(output_path: Path, compression: str | None = None) -> None:
    """Create a tar archive with test files."""
    mode = "w"
    if compression == "gz":
        mode = "w:gz"
    elif compression == "bz2":
        mode = "w:bz2"
    elif compression == "xz":
        mode = "w:xz"

    with tarfile.open(output_path, mode) as tar:
        for name, content in TEST_FILES.items():
            data = content.encode("utf-8")
            info = tarfile.TarInfo(name=name)
            info.size = len(data)
            tar.addfile(info, io.BytesIO(data))


def create_tar_zst_archive(output_path: Path) -> None:
    """Create a zstd-compressed tar archive."""
    try:
        import zstandard as zstd
    except ImportError:
        # Fall back to creating via subprocess if zstandard not available
        tar_path = output_path.with_suffix("")
        create_tar_archive(tar_path)
        test_config.run(
            ["zstd", "-f", str(tar_path), "-o", str(output_path)], check=True
        )
        tar_path.unlink()
        return

    # Create tar in memory
    tar_buffer = io.BytesIO()
    with tarfile.open(fileobj=tar_buffer, mode="w") as tar:
        for name, content in TEST_FILES.items():
            data = content.encode("utf-8")
            info = tarfile.TarInfo(name=name)
            info.size = len(data)
            tar.addfile(info, io.BytesIO(data))

    # Compress with zstd
    cctx = zstd.ZstdCompressor()
    compressed = cctx.compress(tar_buffer.getvalue())
    output_path.write_bytes(compressed)


def create_zip_archive(output_path: Path) -> None:
    """Create a zip archive with test files (no root/ prefix)."""
    with zipfile.ZipFile(output_path, "w", zipfile.ZIP_DEFLATED) as zf:
        for name, content in TEST_FILES.items():
            # Strip 'root/' prefix for zip format
            zip_name = name.replace("root/", "", 1)
            zf.writestr(zip_name, content)


BARE_CONTENT = b"Bare compression test\n"


def create_bare_gz(output_path: Path) -> None:
    with gzip.open(output_path, "wb") as f:
        f.write(BARE_CONTENT)


def create_bare_bz2(output_path: Path) -> None:
    with bz2.open(output_path, "wb") as f:
        f.write(BARE_CONTENT)


def create_bare_xz(output_path: Path) -> None:
    with lzma.open(output_path, "wb") as f:
        f.write(BARE_CONTENT)


class EnvyExtractTests(unittest.TestCase):
    def setUp(self) -> None:
        self._envy_binary = test_config.get_envy_executable()
        self._tmpdir = tempfile.mkdtemp(prefix="envy-extract-test-")
        self._archives_dir = Path(self._tmpdir) / "archives"
        self._archives_dir.mkdir()

        # Create test archives
        create_tar_archive(self._archives_dir / "test.tar")
        create_tar_archive(self._archives_dir / "test.tar.gz", compression="gz")
        create_tar_archive(self._archives_dir / "test.tar.bz2", compression="bz2")
        create_tar_archive(self._archives_dir / "test.tar.xz", compression="xz")
        create_zip_archive(self._archives_dir / "test.zip")

        # Bare single-stream compressed fixtures
        create_bare_gz(self._archives_dir / "hello.txt.gz")
        create_bare_bz2(self._archives_dir / "hello.txt.bz2")
        create_bare_xz(self._archives_dir / "hello.txt.xz")

    def tearDown(self) -> None:
        import shutil

        shutil.rmtree(self._tmpdir, ignore_errors=True)

    def _run_envy(
        self,
        *args: str,
        cwd: str | None = None,
        env: dict[str, str] | None = None,
    ) -> subprocess.CompletedProcess[str]:
        self.assertTrue(
            self._envy_binary.exists(), f"Expected envy binary at {self._envy_binary}"
        )
        return test_config.run(
            [str(self._envy_binary), *args],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=env if env is not None else os.environ.copy(),
            cwd=cwd,
        )

    def test_extract_reports_progress_and_finishes_on_a_full_bar(self) -> None:
        """Unpacking is a wait, so it draws: a lone archive has no pre-scanned total, so
        the row spins on running counts and lands on a full bar when the archive is out."""
        env = os.environ.copy()
        env["TERM"] = "dumb"
        env["ENVY_TEST_FALLBACK_THROTTLE_MS"] = "0"

        with tempfile.TemporaryDirectory() as tmpdir:
            result = self._run_envy(
                "extract", str(self._archives_dir / "test.tar"), tmpdir, env=env
            )
            self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")

            rows = [ln.strip() for ln in result.stderr.splitlines() if "[extract]" in ln]
            self.assertTrue(rows, f"extract drew no progress row: {result.stderr}")
            self.assertTrue(
                rows[-1].endswith(": 100.0%"),
                f"extract did not finish on a full bar: {rows}",
            )
            self.assertIn(
                "files", rows[-1], f"progress row carries no file count: {rows[-1]}"
            )

    def _verify_extracted_structure(self, extract_dir: Path) -> None:
        """Verify the expected directory structure and file contents after extraction."""
        # For tar archives, the structure includes 'root/' prefix
        # For zip archive created with relative paths, files are at the top level

        # Check if we have the root directory (tar format)
        root_dir = extract_dir / "root"
        if root_dir.exists():
            base = root_dir
        else:
            # Zip format - files at top level
            base = extract_dir

        # Verify all expected files exist
        file1 = base / "file1.txt"
        file2 = base / "file2.txt"
        file3 = base / "subdir1" / "file3.txt"
        file4 = base / "subdir1" / "nested" / "file4.txt"
        file5 = base / "subdir2" / "file5.txt"

        self.assertTrue(file1.exists(), f"Expected {file1} to exist")
        self.assertTrue(file2.exists(), f"Expected {file2} to exist")
        self.assertTrue(file3.exists(), f"Expected {file3} to exist")
        self.assertTrue(file4.exists(), f"Expected {file4} to exist")
        self.assertTrue(file5.exists(), f"Expected {file5} to exist")

        # Verify file contents
        self.assertEqual("Root file content\n", file1.read_text())
        self.assertEqual("Another root file\n", file2.read_text())
        self.assertEqual("Subdirectory file\n", file3.read_text())
        self.assertEqual("Nested file content\n", file4.read_text())
        self.assertEqual("Second subdir file\n", file5.read_text())

        # Verify directories exist
        self.assertTrue((base / "subdir1").is_dir())
        self.assertTrue((base / "subdir2").is_dir())
        self.assertTrue((base / "subdir1" / "nested").is_dir())

    def test_extract_missing_archive_fails(self) -> None:
        """Test that extracting a non-existent archive fails with appropriate error."""
        with tempfile.TemporaryDirectory() as tmpdir:
            result = self._run_envy("extract", "nonexistent.tar.gz", tmpdir)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("does not exist", result.stderr)

    def test_extract_tar(self) -> None:
        """Test extracting a plain .tar archive."""
        archive = self._archives_dir / "test.tar"
        self.assertTrue(archive.exists(), f"Test archive {archive} not found")

        with tempfile.TemporaryDirectory() as tmpdir:
            result = self._run_envy("extract", str(archive), tmpdir)

            self.assertEqual(0, result.returncode, f"Extract failed: {result.stderr}")
            self.assertIn("Extracted", result.stderr)
            self.assertIn("files", result.stderr)

            self._verify_extracted_structure(Path(tmpdir))

    def test_extract_tar_gz(self) -> None:
        """Test extracting a .tar.gz (gzip compressed tar) archive."""
        archive = self._archives_dir / "test.tar.gz"
        self.assertTrue(archive.exists(), f"Test archive {archive} not found")

        with tempfile.TemporaryDirectory() as tmpdir:
            result = self._run_envy("extract", str(archive), tmpdir)

            self.assertEqual(0, result.returncode, f"Extract failed: {result.stderr}")
            self.assertIn("Extracted", result.stderr)

            self._verify_extracted_structure(Path(tmpdir))

    def test_extract_tar_bz2(self) -> None:
        """Test extracting a .tar.bz2 (bzip2 compressed tar) archive."""
        archive = self._archives_dir / "test.tar.bz2"
        self.assertTrue(archive.exists(), f"Test archive {archive} not found")

        with tempfile.TemporaryDirectory() as tmpdir:
            result = self._run_envy("extract", str(archive), tmpdir)

            self.assertEqual(0, result.returncode, f"Extract failed: {result.stderr}")
            self.assertIn("Extracted", result.stderr)

            self._verify_extracted_structure(Path(tmpdir))

    def test_extract_tar_xz(self) -> None:
        """Test extracting a .tar.xz (xz/lzma compressed tar) archive."""
        archive = self._archives_dir / "test.tar.xz"
        self.assertTrue(archive.exists(), f"Test archive {archive} not found")

        with tempfile.TemporaryDirectory() as tmpdir:
            result = self._run_envy("extract", str(archive), tmpdir)

            self.assertEqual(0, result.returncode, f"Extract failed: {result.stderr}")
            self.assertIn("Extracted", result.stderr)

            self._verify_extracted_structure(Path(tmpdir))

    @unittest.skip("zstd archive creation requires zstandard module or zstd binary")
    def test_extract_tar_zst(self) -> None:
        """Test extracting a .tar.zst (zstd compressed tar) archive."""
        archive = self._archives_dir / "test.tar.zst"
        create_tar_zst_archive(archive)
        self.assertTrue(archive.exists(), f"Test archive {archive} not found")

        with tempfile.TemporaryDirectory() as tmpdir:
            result = self._run_envy("extract", str(archive), tmpdir)

            self.assertEqual(0, result.returncode, f"Extract failed: {result.stderr}")
            self.assertIn("Extracted", result.stderr)

            self._verify_extracted_structure(Path(tmpdir))

    def test_extract_zip(self) -> None:
        """Test extracting a .zip archive."""
        archive = self._archives_dir / "test.zip"
        self.assertTrue(archive.exists(), f"Test archive {archive} not found")

        with tempfile.TemporaryDirectory() as tmpdir:
            result = self._run_envy("extract", str(archive), tmpdir)

            self.assertEqual(0, result.returncode, f"Extract failed: {result.stderr}")
            self.assertIn("Extracted", result.stderr)

            self._verify_extracted_structure(Path(tmpdir))

    def test_extract_to_current_directory(self) -> None:
        """Test extracting to current directory when destination is not specified."""
        archive = self._archives_dir / "test.tar.gz"
        self.assertTrue(archive.exists(), f"Test archive {archive} not found")

        with tempfile.TemporaryDirectory() as tmpdir:
            result = self._run_envy("extract", str(archive), cwd=tmpdir)

            self.assertEqual(0, result.returncode, f"Extract failed: {result.stderr}")
            self.assertIn("Extracted", result.stderr)

            self._verify_extracted_structure(Path(tmpdir))

    def test_extract_creates_destination_if_missing(self) -> None:
        """Test that extract creates the destination directory if it doesn't exist."""
        archive = self._archives_dir / "test.tar.gz"
        self.assertTrue(archive.exists(), f"Test archive {archive} not found")

        with tempfile.TemporaryDirectory() as tmpdir:
            dest = Path(tmpdir) / "nested" / "destination"
            self.assertFalse(dest.exists())

            result = self._run_envy("extract", str(archive), str(dest))

            self.assertEqual(0, result.returncode, f"Extract failed: {result.stderr}")
            self.assertTrue(dest.exists())
            self.assertTrue(dest.is_dir())

            self._verify_extracted_structure(dest)

    def test_extract_reports_file_count(self) -> None:
        """Test that extract reports the number of files extracted."""
        archive = self._archives_dir / "test.tar.gz"
        self.assertTrue(archive.exists(), f"Test archive {archive} not found")

        with tempfile.TemporaryDirectory() as tmpdir:
            result = self._run_envy("extract", str(archive), tmpdir)

            self.assertEqual(0, result.returncode)
            self.assertIn("Extracted 5 files", result.stderr)

    def _verify_bare_extraction(self, archive: Path) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            result = self._run_envy("extract", str(archive), tmpdir)
            self.assertEqual(0, result.returncode, f"Extract failed: {result.stderr}")
            self.assertIn("Extracted 1 files", result.stderr)

            out = Path(tmpdir) / "hello.txt"
            self.assertTrue(out.exists(), f"Expected {out} to exist")
            self.assertEqual(BARE_CONTENT, out.read_bytes())

    def test_extract_bare_gz(self) -> None:
        """Bare .gz extracts to stem-named file."""
        self._verify_bare_extraction(self._archives_dir / "hello.txt.gz")

    def test_extract_bare_bz2(self) -> None:
        """Bare .bz2 extracts to stem-named file."""
        self._verify_bare_extraction(self._archives_dir / "hello.txt.bz2")

    def test_extract_bare_xz(self) -> None:
        """Bare .xz extracts to stem-named file."""
        self._verify_bare_extraction(self._archives_dir / "hello.txt.xz")

    def test_extract_bare_to_default_dest(self) -> None:
        """Bare-compressed extracts to cwd when destination is omitted."""
        archive = self._archives_dir / "hello.txt.gz"

        with tempfile.TemporaryDirectory() as tmpdir:
            result = self._run_envy("extract", str(archive), cwd=tmpdir)
            self.assertEqual(0, result.returncode, f"Extract failed: {result.stderr}")
            self.assertEqual(BARE_CONTENT, (Path(tmpdir) / "hello.txt").read_bytes())

    def test_extract_bare_unrecognized_suffix_errors(self) -> None:
        """A non-archive file with no recognized suffix must error, not silently copy."""
        with tempfile.TemporaryDirectory() as tmpdir:
            fake = Path(tmpdir) / "garbage.bin"
            fake.write_bytes(b"not an archive of any kind")

            dest = Path(tmpdir) / "dest"
            dest.mkdir()
            result = self._run_envy("extract", str(fake), str(dest))

            self.assertNotEqual(0, result.returncode)
            # No files should have leaked into dest
            self.assertEqual([], list(dest.iterdir()))

    def test_extract_corrupt_gz_errors(self) -> None:
        """A file with .gz suffix but invalid stream must error cleanly."""
        with tempfile.TemporaryDirectory() as tmpdir:
            bad = Path(tmpdir) / "bad.gz"
            bad.write_bytes(b"this is not gzip data")

            dest = Path(tmpdir) / "dest"
            dest.mkdir()
            result = self._run_envy("extract", str(bad), str(dest))

            self.assertNotEqual(0, result.returncode)

    def test_extract_rejects_parent_traversal_entry(self) -> None:
        """A tar entry with a ../ path must be refused and write nothing outside dest."""
        with tempfile.TemporaryDirectory() as tmpdir:
            archive = self._archives_dir / "evil_traversal.tar"
            with tarfile.open(archive, "w") as tar:
                data = b"pwned\n"
                info = tarfile.TarInfo(name="../escaped.txt")
                info.size = len(data)
                tar.addfile(info, io.BytesIO(data))

            dest = Path(tmpdir) / "dest"
            dest.mkdir()
            result = self._run_envy("extract", str(archive), str(dest))

            self.assertNotEqual(0, result.returncode)
            # Nothing must escape into the parent of dest.
            self.assertFalse((dest.parent / "escaped.txt").exists())
            self.assertEqual([], list(dest.iterdir()))

    @unittest.skipIf(
        sys.platform == "win32",
        "Windows extract of UTF-8 archive entry names needs wide-path handling "
        "(libarchive narrow pathname mangles UTF-8 to '?'); tracked separately",
    )
    def test_extract_non_ascii_filenames_roundtrip(self) -> None:
        """Archive entries with non-ASCII (UTF-8) names must extract intact; the
        path-safety guard rejects traversal, not legitimate Unicode names."""
        with tempfile.TemporaryDirectory() as tmpdir:
            entries = {
                "root/café.txt": "accented\n",
                "root/日本語/ファイル.txt": "japanese\n",
                "root/naïve-Ω.bin": "mixed\n",
            }
            archive = self._archives_dir / "unicode.tar"
            with tarfile.open(archive, "w", encoding="utf-8") as tar:
                for name, content in entries.items():
                    data = content.encode("utf-8")
                    info = tarfile.TarInfo(name=name)
                    info.size = len(data)
                    tar.addfile(info, io.BytesIO(data))

            dest = Path(tmpdir) / "dest"
            dest.mkdir()
            result = self._run_envy("extract", str(archive), str(dest))

            self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
            for name, content in entries.items():
                extracted = dest / name
                self.assertTrue(extracted.exists(), f"missing {name}")
                self.assertEqual(content, extracted.read_text(encoding="utf-8"))

    def test_extract_only_single_file(self) -> None:
        """--only takes exactly one named entry and leaves the rest compressed."""
        archive = self._archives_dir / "test.tar.gz"

        with tempfile.TemporaryDirectory() as tmpdir:
            result = self._run_envy(
                "extract", str(archive), tmpdir, "--only", "root/subdir1/file3.txt"
            )

            self.assertEqual(0, result.returncode, f"Extract failed: {result.stderr}")
            self.assertIn("Extracted 1 files", result.stderr)

            dest = Path(tmpdir)
            self.assertEqual(
                "Subdirectory file\n", (dest / "root/subdir1/file3.txt").read_text()
            )
            self.assertFalse((dest / "root/file1.txt").exists())
            self.assertFalse((dest / "root/subdir2").exists())

    def test_extract_only_directory_subtree(self) -> None:
        """A directory in --only takes everything beneath it."""
        archive = self._archives_dir / "test.tar.gz"

        with tempfile.TemporaryDirectory() as tmpdir:
            result = self._run_envy(
                "extract", str(archive), tmpdir, "--only", "root/subdir1"
            )

            self.assertEqual(0, result.returncode, f"Extract failed: {result.stderr}")
            self.assertIn("Extracted 2 files", result.stderr)

            dest = Path(tmpdir)
            self.assertTrue((dest / "root/subdir1/file3.txt").exists())
            self.assertTrue((dest / "root/subdir1/nested/file4.txt").exists())
            self.assertFalse((dest / "root/file1.txt").exists())
            self.assertFalse((dest / "root/subdir2").exists())

    def test_extract_only_repeated(self) -> None:
        """Repeated --only accumulates entries."""
        archive = self._archives_dir / "test.tar.gz"

        with tempfile.TemporaryDirectory() as tmpdir:
            result = self._run_envy(
                "extract",
                str(archive),
                tmpdir,
                "--only",
                "root/file1.txt",
                "--only",
                "root/subdir2",
            )

            self.assertEqual(0, result.returncode, f"Extract failed: {result.stderr}")
            self.assertIn("Extracted 2 files", result.stderr)

            dest = Path(tmpdir)
            self.assertTrue((dest / "root/file1.txt").exists())
            self.assertTrue((dest / "root/subdir2/file5.txt").exists())
            self.assertFalse((dest / "root/file2.txt").exists())

    def test_extract_only_unmatched_errors(self) -> None:
        """An --only entry that matches nothing must fail loudly, not silently."""
        archive = self._archives_dir / "test.tar.gz"

        with tempfile.TemporaryDirectory() as tmpdir:
            result = self._run_envy(
                "extract",
                str(archive),
                tmpdir,
                "--only",
                "root/file1.txt",
                "--only",
                "root/typo.txt",
            )

            self.assertNotEqual(0, result.returncode)
            self.assertIn("root/typo.txt", result.stderr)

    def test_extract_only_rejects_traversal(self) -> None:
        """An --only entry with '..' must be refused before anything is written."""
        archive = self._archives_dir / "test.tar.gz"

        with tempfile.TemporaryDirectory() as tmpdir:
            dest = Path(tmpdir) / "dest"
            dest.mkdir()
            result = self._run_envy(
                "extract", str(archive), str(dest), "--only", "../escape"
            )

            self.assertNotEqual(0, result.returncode)
            self.assertIn("'only' entry", result.stderr)
            self.assertEqual([], list(dest.iterdir()))

    def test_extract_only_glob_star(self) -> None:
        """A '*' in --only matches within one path component."""
        archive = self._archives_dir / "test.tar.gz"

        with tempfile.TemporaryDirectory() as tmpdir:
            result = self._run_envy(
                "extract", str(archive), tmpdir, "--only", "root/*.txt"
            )

            self.assertEqual(0, result.returncode, f"Extract failed: {result.stderr}")
            self.assertIn("Extracted 2 files", result.stderr)

            dest = Path(tmpdir)
            self.assertTrue((dest / "root/file1.txt").exists())
            self.assertTrue((dest / "root/file2.txt").exists())
            self.assertFalse((dest / "root/subdir1").exists())

    def test_extract_only_glob_globstar(self) -> None:
        """'**' in --only spans path components."""
        archive = self._archives_dir / "test.tar.gz"

        with tempfile.TemporaryDirectory() as tmpdir:
            result = self._run_envy(
                "extract", str(archive), tmpdir, "--only", "root/**/file4.txt"
            )

            self.assertEqual(0, result.returncode, f"Extract failed: {result.stderr}")
            self.assertIn("Extracted 1 files", result.stderr)

            dest = Path(tmpdir)
            self.assertTrue((dest / "root/subdir1/nested/file4.txt").exists())
            self.assertFalse((dest / "root/file1.txt").exists())

    def test_extract_only_glob_class_and_question(self) -> None:
        """Character classes and '?' work, and '?' spans exactly one character."""
        archive = self._archives_dir / "test.tar.gz"

        with tempfile.TemporaryDirectory() as tmpdir:
            result = self._run_envy(
                "extract",
                str(archive),
                tmpdir,
                "--only",
                "root/file[12].txt",
                "--only",
                "root/subdir?/file?.txt",
            )

            self.assertEqual(0, result.returncode, f"Extract failed: {result.stderr}")
            self.assertIn("Extracted 4 files", result.stderr)

            dest = Path(tmpdir)
            self.assertTrue((dest / "root/file1.txt").exists())
            self.assertTrue((dest / "root/file2.txt").exists())
            self.assertTrue((dest / "root/subdir1/file3.txt").exists())
            self.assertTrue((dest / "root/subdir2/file5.txt").exists())
            # '?' does not cross '/', so the nested file is not selected.
            self.assertFalse((dest / "root/subdir1/nested").exists())

    def test_extract_only_glob_unmatched_errors(self) -> None:
        """A pattern matching nothing fails and names the pattern."""
        archive = self._archives_dir / "test.tar.gz"

        with tempfile.TemporaryDirectory() as tmpdir:
            result = self._run_envy(
                "extract", str(archive), tmpdir, "--only", "root/**/*.md"
            )

            self.assertNotEqual(0, result.returncode)
            self.assertIn("root/**/*.md", result.stderr)

    def test_extract_only_glob_malformed_errors(self) -> None:
        """A malformed pattern is rejected before anything is written."""
        archive = self._archives_dir / "test.tar.gz"

        for bad in ("root/[abc", "root/a**b"):
            with tempfile.TemporaryDirectory() as tmpdir:
                dest = Path(tmpdir) / "dest"
                dest.mkdir()
                result = self._run_envy(
                    "extract", str(archive), str(dest), "--only", bad
                )

                self.assertNotEqual(0, result.returncode, f"{bad} should be rejected")
                self.assertIn("'only' entry", result.stderr)
                self.assertEqual([], list(dest.iterdir()))

    def _write_hardlink_archive(self, name: str, link_first: bool = False) -> Path:
        """Write a tar holding root/target.txt plus a hard link root/link.txt to it."""
        archive = self._archives_dir / name
        data = b"payload\n"

        def add_target(tar: tarfile.TarFile) -> None:
            info = tarfile.TarInfo(name="root/target.txt")
            info.size = len(data)
            tar.addfile(info, io.BytesIO(data))

        def add_link(tar: tarfile.TarFile) -> None:
            info = tarfile.TarInfo(name="root/link.txt")
            info.type = tarfile.LNKTYPE
            info.linkname = "root/target.txt"
            tar.addfile(info)

        with tarfile.open(archive, "w") as tar:
            for add in (add_link, add_target) if link_first else (add_target, add_link):
                add(tar)
        return archive

    def test_extract_only_hardlink_without_target_errors(self) -> None:
        """Selecting a hard link but not its target blames 'only' and names the target."""
        archive = self._write_hardlink_archive("hardlink.tar")

        with tempfile.TemporaryDirectory() as tmpdir:
            dest = Path(tmpdir) / "dest"
            dest.mkdir()
            result = self._run_envy(
                "extract", str(archive), str(dest), "--only", "root/link.txt"
            )

            self.assertNotEqual(0, result.returncode)
            self.assertIn("hard link", result.stderr)
            self.assertIn("root/target.txt", result.stderr)
            self.assertIn("'only' does not select", result.stderr)

    def test_extract_only_hardlink_target_later_in_archive_errors(self) -> None:
        """A selected target that trails its link is an ordering fault, not an 'only'
        fault; the two diagnostics must not be conflated."""
        archive = self._write_hardlink_archive("hardlink_reordered.tar", link_first=True)

        with tempfile.TemporaryDirectory() as tmpdir:
            dest = Path(tmpdir) / "dest"
            dest.mkdir()
            result = self._run_envy("extract", str(archive), str(dest), "--only", "root")

            self.assertNotEqual(0, result.returncode)
            self.assertIn("appears later in the archive", result.stderr)
            self.assertNotIn("'only' does not select", result.stderr)

    def test_extract_only_hardlink_target_selected_by_glob(self) -> None:
        """Selection of the target is decided by matching, not by what is on disk."""
        archive = self._write_hardlink_archive("hardlink_glob.tar")

        with tempfile.TemporaryDirectory() as tmpdir:
            dest = Path(tmpdir) / "dest"
            dest.mkdir()
            result = self._run_envy(
                "extract", str(archive), str(dest), "--only", "root/*.txt"
            )

            self.assertEqual(0, result.returncode, f"Extract failed: {result.stderr}")
            self.assertEqual(b"payload\n", (dest / "root/link.txt").read_bytes())

    def test_extract_only_hardlink_preexisting_target_file_still_errors(self) -> None:
        """An unselected target that happens to exist in the destination must still
        fail: existence is not selection."""
        archive = self._write_hardlink_archive("hardlink_preexisting.tar")

        with tempfile.TemporaryDirectory() as tmpdir:
            dest = Path(tmpdir) / "dest"
            (dest / "root").mkdir(parents=True)
            (dest / "root/target.txt").write_bytes(b"unrelated\n")

            result = self._run_envy(
                "extract", str(archive), str(dest), "--only", "root/link.txt"
            )

            self.assertNotEqual(0, result.returncode)
            self.assertIn("'only' does not select", result.stderr)
            # The stale file must not have been hard-linked to.
            self.assertEqual(b"unrelated\n", (dest / "root/target.txt").read_bytes())
            self.assertFalse((dest / "root/link.txt").exists())

    def test_extract_only_hardlink_with_target(self) -> None:
        """A hard link extracts when its target is included too."""
        archive = self._write_hardlink_archive("hardlink_ok.tar")

        with tempfile.TemporaryDirectory() as tmpdir:
            dest = Path(tmpdir) / "dest"
            dest.mkdir()
            result = self._run_envy(
                "extract", str(archive), str(dest), "--only", "root"
            )

            self.assertEqual(0, result.returncode, f"Extract failed: {result.stderr}")
            self.assertEqual(b"payload\n", (dest / "root/target.txt").read_bytes())
            self.assertEqual(b"payload\n", (dest / "root/link.txt").read_bytes())

    def test_extract_rejects_symlink_escape(self) -> None:
        """A symlink entry pointing outside dest plus a write through it must be
        refused (ARCHIVE_EXTRACT_SECURE_SYMLINKS) and must not write outside dest."""
        with tempfile.TemporaryDirectory() as tmpdir:
            outside = Path(tmpdir) / "outside"
            outside.mkdir()
            archive = self._archives_dir / "evil_symlink.tar"
            with tarfile.open(archive, "w") as tar:
                link = tarfile.TarInfo(name="escape")
                link.type = tarfile.SYMTYPE
                link.linkname = str(outside)
                tar.addfile(link)
                data = b"pwned\n"
                payload = tarfile.TarInfo(name="escape/payload.txt")
                payload.size = len(data)
                tar.addfile(payload, io.BytesIO(data))

            dest = Path(tmpdir) / "dest"
            dest.mkdir()
            result = self._run_envy("extract", str(archive), str(dest))

            self.assertNotEqual(0, result.returncode)
            # The write must not have escaped through the symlink.
            self.assertFalse((outside / "payload.txt").exists())


if __name__ == "__main__":
    unittest.main()
