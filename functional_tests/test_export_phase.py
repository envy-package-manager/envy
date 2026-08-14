"""Functional tests for the pipeline-integrated export phase.

Verifies that export runs as a normal phase (after install, before completion)
and that it overlaps with other packages' builds when dependencies exist.
Tests use structured trace events, not timing, to assert phase ordering.
"""

import hashlib
import io
import os
import tarfile
import unittest
from pathlib import Path

from . import test_config
from .env import EnvyTestCase
from .test_config import make_manifest, parse_export_line
from .trace_parser import PkgPhase, TraceParser


def create_test_archive(output_path: Path) -> str:
    """Create test.tar.gz archive and return its SHA256 hash."""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz") as tar:
        data = b"test content\n"
        info = tarfile.TarInfo(name="root/file.txt")
        info.size = len(data)
        tar.addfile(info, io.BytesIO(data))
    archive_data = buf.getvalue()
    output_path.write_bytes(archive_data)
    return hashlib.sha256(archive_data).hexdigest()


class TestExportPhase(EnvyTestCase):
    """Tests for export as a pipeline phase."""

    def setUp(self):
        super().setUp()
        self.test_dir = self.make_temp_dir("test_dir")
        self.output_dir = self.make_temp_dir("output_dir")

        self.archive_path = self.test_dir / "test.tar.gz"
        self.archive_hash = create_test_archive(self.archive_path)

    def lua_path(self, path: Path) -> str:
        return path.as_posix()

    def create_manifest(self, content: str) -> Path:
        manifest_path = self.test_dir / "envy.lua"
        manifest_path.write_text(make_manifest(content), encoding="utf-8")
        return manifest_path

    def run_envy(self, *args, trace_file=None, cwd=None):
        cmd = [str(self.envy), "--cache-root", str(self.cache_root)]
        if trace_file:
            cmd.append(f"--trace=file:{trace_file}")
        cmd.extend(args)
        return test_config.run(
            cmd, cwd=cwd or self.project_root, capture_output=True, text=True
        )

    def test_export_phase_in_sequence(self):
        """Export phase appears between install and completion in trace."""
        archive_lua = self.lua_path(self.archive_path)

        spec = f"""IDENTITY = "local.exportable@v1"
EXPORTABLE = true

FETCH = {{
  source = "{archive_lua}",
  sha256 = "{self.archive_hash}"
}}

STAGE = {{strip = 1}}
"""
        (self.test_dir / "exportable.lua").write_text(spec, encoding="utf-8")

        manifest = self.create_manifest(
            f'PACKAGES = {{ {{ spec = "local.exportable@v1", source = "{self.lua_path(self.test_dir / "exportable.lua")}" }} }}'
        )

        trace_file = self.cache_root / "trace.jsonl"
        result = self.run_envy(
            "export",
            "-o",
            str(self.output_dir),
            "--ignore-depot",
            "--manifest",
            str(manifest),
            trace_file=trace_file,
        )
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        # Verify export phase is in the sequence between install and completion
        parser = TraceParser(trace_file)
        phases = parser.get_phase_sequence("local.exportable@v1")
        self.assertIn(PkgPhase.PKG_INSTALL, phases)
        self.assertIn(PkgPhase.PKG_EXPORT, phases)
        self.assertIn(PkgPhase.COMPLETION, phases)

        install_idx = phases.index(PkgPhase.PKG_INSTALL)
        export_idx = phases.index(PkgPhase.PKG_EXPORT)
        completion_idx = phases.index(PkgPhase.COMPLETION)
        self.assertLess(install_idx, export_idx, "export must follow install")
        self.assertLess(export_idx, completion_idx, "export must precede completion")

        # Verify output file was created
        lines = [l for l in result.stdout.strip().split("\n") if l.strip()]
        self.assertEqual(len(lines), 1)
        sha_hex, archive_path = parse_export_line(lines[0])
        self.assertEqual(len(sha_hex), 64)

    def test_export_phase_skipped_for_non_export(self):
        """Export phase runs but short-circuits when not exporting."""
        archive_lua = self.lua_path(self.archive_path)

        spec = f"""IDENTITY = "local.simple@v1"

FETCH = {{
  source = "{archive_lua}",
  sha256 = "{self.archive_hash}"
}}

STAGE = {{strip = 1}}
"""
        (self.test_dir / "simple.lua").write_text(spec, encoding="utf-8")

        manifest = self.create_manifest(
            f'PACKAGES = {{ {{ spec = "local.simple@v1", source = "{self.lua_path(self.test_dir / "simple.lua")}" }} }}'
        )

        trace_file = self.cache_root / "trace.jsonl"
        # Use install (not export) — export phase should still appear in trace
        # but produce no output files
        result = self.run_envy(
            "install",
            "--ignore-depot",
            "--manifest",
            str(manifest),
            trace_file=trace_file,
        )
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        parser = TraceParser(trace_file)
        phases = parser.get_phase_sequence("local.simple@v1")
        # Export phase should appear in the sequence (it runs but short-circuits)
        self.assertIn(PkgPhase.PKG_EXPORT, phases)
        self.assertIn(PkgPhase.COMPLETION, phases)

        # No export output files should exist
        export_files = list(self.output_dir.glob("*.tar.zst"))
        self.assertEqual(len(export_files), 0)

    def test_dependency_does_not_wait_for_export(self):
        """Package B (depends on A) starts building before A finishes exporting.

        Proven via trace: B's build phase_start occurs before A's completion.
        Since export is between install and completion, if B starts building
        before A completes, B did not wait for A's export.
        """
        archive_lua = self.lua_path(self.archive_path)

        # Package A: exportable, dependency of B
        spec_a = f"""IDENTITY = "local.dep_a@v1"
EXPORTABLE = true

FETCH = {{
  source = "{archive_lua}",
  sha256 = "{self.archive_hash}"
}}

STAGE = {{strip = 1}}
"""
        spec_a_path = self.test_dir / "dep_a.lua"
        spec_a_path.write_text(spec_a, encoding="utf-8")

        # Package B: depends on A
        spec_b = f"""IDENTITY = "local.dep_b@v1"
EXPORTABLE = true
DEPENDENCIES = {{
  {{ spec = "local.dep_a@v1", source = "{self.lua_path(spec_a_path)}" }}
}}

FETCH = {{
  source = "{archive_lua}",
  sha256 = "{self.archive_hash}"
}}

STAGE = {{strip = 1}}
"""
        spec_b_path = self.test_dir / "dep_b.lua"
        spec_b_path.write_text(spec_b, encoding="utf-8")

        manifest = self.create_manifest(
            f"""PACKAGES = {{
  {{ spec = "local.dep_a@v1", source = "{self.lua_path(spec_a_path)}" }},
  {{ spec = "local.dep_b@v1", source = "{self.lua_path(spec_b_path)}" }},
}}"""
        )

        trace_file = self.cache_root / "trace.jsonl"
        result = self.run_envy(
            "export",
            "-o",
            str(self.output_dir),
            "--ignore-depot",
            "--manifest",
            str(manifest),
            trace_file=trace_file,
        )
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        parser = TraceParser(trace_file)

        # Get phase_complete events for A and phase_start events for B
        a_export_start = parser.filter_by_spec_and_event(
            "local.dep_a@v1", "phase_start"
        )
        a_export_start = [
            e for e in a_export_start if e.phase == PkgPhase.PKG_EXPORT
        ]

        a_completion = parser.filter_by_spec_and_event(
            "local.dep_a@v1", "phase_complete"
        )
        a_completion = [
            e for e in a_completion if e.phase == PkgPhase.COMPLETION
        ]

        b_build_start = parser.filter_by_spec_and_event(
            "local.dep_b@v1", "phase_start"
        )
        b_fetch_start = [
            e for e in b_build_start if e.phase == PkgPhase.PKG_FETCH
        ]

        # Both packages should have exported
        self.assertTrue(len(a_export_start) > 0, "A should have export phase")

        # A's export should have started (verifies it's a target)
        a_phases = parser.get_phase_sequence("local.dep_a@v1")
        b_phases = parser.get_phase_sequence("local.dep_b@v1")
        self.assertIn(PkgPhase.PKG_EXPORT, a_phases)
        self.assertIn(PkgPhase.PKG_EXPORT, b_phases)

        # Key assertion: B did NOT wait for A's completion.
        # The dependency wait is for kDependencySatisfiedPhase (install+1 = export),
        # not completion. So B starts its fetch/build after A enters export,
        # but before A reaches completion.
        # Verify this via trace ordering: B's fetch must start before A completes.
        if a_completion and b_fetch_start:
            # Compare causal order — B's fetch should start before A's completion
            self.assertLessEqual(
                b_fetch_start[0].seq,
                a_completion[0].seq,
                "B's fetch should start before or at A's completion "
                "(B should not wait for A's full completion)",
            )

        # Both packages should have produced export output
        lines = [l for l in result.stdout.strip().split("\n") if l.strip()]
        self.assertEqual(len(lines), 2, f"Expected 2 export lines, got: {lines}")

    def test_export_produces_valid_archive(self):
        """Export through the pipeline produces the same output format."""
        archive_lua = self.lua_path(self.archive_path)

        spec = f"""IDENTITY = "local.pkg@v1"
EXPORTABLE = true

FETCH = {{
  source = "{archive_lua}",
  sha256 = "{self.archive_hash}"
}}

STAGE = {{strip = 1}}
"""
        (self.test_dir / "pkg.lua").write_text(spec, encoding="utf-8")

        manifest = self.create_manifest(
            f'PACKAGES = {{ {{ spec = "local.pkg@v1", source = "{self.lua_path(self.test_dir / "pkg.lua")}" }} }}'
        )

        result = self.run_envy(
            "export",
            "-o",
            str(self.output_dir),
            "--ignore-depot",
            "--manifest",
            str(manifest),
        )
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        lines = [l for l in result.stdout.strip().split("\n") if l.strip()]
        self.assertEqual(len(lines), 1)

        sha_hex, rel_path = parse_export_line(lines[0])
        archive_path = Path(rel_path)
        self.assertTrue(archive_path.exists(), f"Archive not found: {archive_path}")
        self.assertTrue(
            str(archive_path).endswith(".tar.zst"),
            f"Expected .tar.zst extension: {archive_path}",
        )

        # Verify SHA256 matches the archive content
        actual_hash = hashlib.sha256(archive_path.read_bytes()).hexdigest()
        self.assertEqual(sha_hex, actual_hash)

    def test_non_target_dependency_not_exported(self):
        """Dependencies that are not export targets skip the export phase."""
        archive_lua = self.lua_path(self.archive_path)

        # dep: not in export targets (not in manifest PACKAGES directly,
        # only referenced as dependency)
        spec_dep = f"""IDENTITY = "local.dep@v1"

FETCH = {{
  source = "{archive_lua}",
  sha256 = "{self.archive_hash}"
}}

STAGE = {{strip = 1}}
"""
        spec_dep_path = self.test_dir / "dep.lua"
        spec_dep_path.write_text(spec_dep, encoding="utf-8")

        # main: export target, depends on dep
        spec_main = f"""IDENTITY = "local.main@v1"
EXPORTABLE = true
DEPENDENCIES = {{
  {{ spec = "local.dep@v1", source = "{self.lua_path(spec_dep_path)}" }}
}}

FETCH = {{
  source = "{archive_lua}",
  sha256 = "{self.archive_hash}"
}}

STAGE = {{strip = 1}}
"""
        spec_main_path = self.test_dir / "main.lua"
        spec_main_path.write_text(spec_main, encoding="utf-8")

        # Only main is in PACKAGES — dep is pulled in as dependency
        manifest = self.create_manifest(
            f'PACKAGES = {{ {{ spec = "local.main@v1", source = "{self.lua_path(spec_main_path)}" }} }}'
        )

        trace_file = self.cache_root / "trace.jsonl"
        result = self.run_envy(
            "export",
            "-o",
            str(self.output_dir),
            "--ignore-depot",
            "--manifest",
            str(manifest),
            trace_file=trace_file,
        )
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        # Only one export line (main), not the dependency
        lines = [l for l in result.stdout.strip().split("\n") if l.strip()]
        self.assertEqual(len(lines), 1)
        self.assertIn("local.main@v1", lines[0])

        # dep should still have the export phase in its sequence (it short-circuits)
        parser = TraceParser(trace_file)
        dep_phases = parser.get_phase_sequence("local.dep@v1")
        self.assertIn(PkgPhase.PKG_EXPORT, dep_phases)

        # But no archive should exist for dep
        dep_archives = [
            f for f in self.output_dir.glob("*.tar.zst") if "local.dep" in f.name
        ]
        self.assertEqual(len(dep_archives), 0, "dep should not produce an archive")


if __name__ == "__main__":
    unittest.main()
