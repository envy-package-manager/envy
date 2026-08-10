"""Functional tests for conflicting declarations of one bundle identity.

pkg_key is identity + options, never the source, so every declaration of a bundle
identity collapses onto one package. A declaration that names a different payload
would silently lose, so the engine rejects it (engine::validate_bundle_redeclaration);
declarations that agree must keep working, and two custom fetch closures — which
cannot be compared — warn instead of failing.
"""

import hashlib
import shutil
import subprocess
import tarfile
import tempfile
import threading
import unittest
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from . import test_config
from .test_config import make_manifest


class _QuietHandler(SimpleHTTPRequestHandler):
    def log_message(self, fmt, *args):  # noqa: A003 - silence per-request logging
        pass


BUNDLE_LUA = """BUNDLE = "test.tc@v1"
SPECS = {
  ["test.tool@v1"] = "specs/tool.lua",
}
"""

TOOL_SPEC = """IDENTITY = "test.tool@v1"
DEPENDENCIES = {}
USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options) return false end,
    INSTALL = function(pkg_dir, options) end,
  },
}
"""

# Declares the bundle itself (so the bundle source lives in this spec) and pulls the
# tool spec out of it. `SRC` is substituted per test.
DECLARING_SPEC = """IDENTITY = "test.{name}@v1"

DEPENDENCIES = {{
  {{ bundle = "test.tc@v1", source = {src} }},
  {{ spec = "test.tool@v1", bundle = "test.tc@v1", setup = {{ "main" }} }},
}}

FETCH = function(dir, options) end
"""

# A custom-fetch declaration of the same bundle identity, written out by hand so two
# specs can declare byte-identical closures that still cannot be compared.
CUSTOM_FETCH_SRC = """{ fetch = function(tmp_dir)
      local b = io.open(tmp_dir .. "/envy-bundle.lua", "w")
      b:write('BUNDLE = "test.tc@v1"\\n')
      b:write('SPECS = { ["test.tool@v1"] = "tool.lua" }\\n')
      b:close()
      local t = io.open(tmp_dir .. "/tool.lua", "w")
      t:write('IDENTITY = "test.tool@v1"\\n')
      t:write('DEPENDENCIES = {}\\n')
      t:write('USER_MANAGED = true\\n')
      t:write('SETUP = { main = { CHECK = function() return false end, ')
      t:write('INSTALL = function() end } }\\n')
      t:close()
      envy.commit_fetch({"envy-bundle.lua", "tool.lua"})
    end }"""


class TestBundleRedeclaration(unittest.TestCase):
    def setUp(self):
        self.cache_root = Path(tempfile.mkdtemp(prefix="envy-bundle-redecl-cache-"))
        self.test_dir = Path(tempfile.mkdtemp(prefix="envy-bundle-redecl-"))
        self.envy = test_config.get_envy_executable()
        self.project_root = Path(__file__).parent.parent

        # Two byte-identical bundle archives at different URLs, plus a git repo, so
        # "conflicting" means a different source rather than different content.
        src = self.test_dir / "bundle-src"
        (src / "specs").mkdir(parents=True)
        (src / "envy-bundle.lua").write_text(BUNDLE_LUA, encoding="utf-8")
        (src / "specs" / "tool.lua").write_text(TOOL_SPEC, encoding="utf-8")

        self.serve_dir = self.test_dir / "serve"
        self.serve_dir.mkdir()
        for name in ("bundle-a.tar.gz", "bundle-b.tar.gz"):
            with tarfile.open(self.serve_dir / name, "w:gz") as tf:
                for path in sorted(src.rglob("*")):
                    tf.add(path, arcname=str(path.relative_to(src)))

        handler = partial(_QuietHandler, directory=str(self.serve_dir))
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        port = self.server.server_address[1]
        self.url_a = f"http://127.0.0.1:{port}/bundle-a.tar.gz"
        self.url_b = f"http://127.0.0.1:{port}/bundle-b.tar.gz"
        self.dir_src = (src).as_posix()

    def tearDown(self):
        self.server.shutdown()
        self.thread.join(timeout=5)
        self.server.server_close()
        shutil.rmtree(self.cache_root, ignore_errors=True)
        shutil.rmtree(self.test_dir, ignore_errors=True)

    # -- helpers -------------------------------------------------------------

    def write_spec(self, name: str, src: str) -> Path:
        path = self.test_dir / f"{name}.lua"
        path.write_text(DECLARING_SPEC.format(name=name, src=src), encoding="utf-8")
        return path

    def install(self, packages: str) -> subprocess.CompletedProcess:
        manifest = self.test_dir / "envy.lua"
        manifest.write_text(
            make_manifest(f"PACKAGES = {{\n{packages}\n}}\n"), encoding="utf-8"
        )
        return test_config.run(
            [
                str(self.envy),
                "--cache-root",
                str(self.cache_root),
                "install",
                "--manifest",
                str(manifest),
            ],
            cwd=self.project_root,
            capture_output=True,
            text=True,
        )

    def two_declaring_specs(self, src_one: str, src_two: str) -> str:
        one = self.write_spec("one", src_one)
        two = self.write_spec("two", src_two)
        return (
            f'    {{ spec = "test.one@v1", source = "{one.as_posix()}" }},\n'
            f'    {{ spec = "test.two@v1", source = "{two.as_posix()}" }},'
        )

    # -- conflicts are rejected ----------------------------------------------

    def test_conflicting_remote_urls_rejected(self):
        """Same bundle identity, two URLs: refuse rather than pick one."""
        result = self.install(
            self.two_declaring_specs(f'"{self.url_a}"', f'"{self.url_b}"')
        )

        self.assertNotEqual(0, result.returncode)
        self.assertIn("test.tc@v1", result.stderr)
        self.assertIn("conflicting sources", result.stderr)
        # The message must name both declaring files so the author can fix one.
        self.assertIn("one.lua", result.stderr)
        self.assertIn("two.lua", result.stderr)

    def test_conflicting_sha256_rejected(self):
        """Same URL, different sha256 — still two different payloads."""
        result = self.install(
            self.two_declaring_specs(
                f'"{self.url_a}", sha256 = "' + "a" * 64 + '"',
                f'"{self.url_a}", sha256 = "' + "b" * 64 + '"',
            )
        )

        self.assertNotEqual(0, result.returncode)
        self.assertIn("conflicting sources", result.stderr)

    def test_pinned_versus_unpinned_rejected(self):
        """One declaration pins a sha256, the other does not.

        Both declarations would install cleanly on their own, so without the check the
        winner of the insert silently decides whether the payload is verified at all —
        no error, no warning, different guarantees per run.
        """
        sha = hashlib.sha256(
            (self.serve_dir / "bundle-a.tar.gz").read_bytes()
        ).hexdigest()
        result = self.install(
            self.two_declaring_specs(
                f'"{self.url_a}"', f'"{self.url_a}", sha256 = "{sha}"'
            )
        )

        self.assertNotEqual(0, result.returncode)
        self.assertIn("conflicting sources", result.stderr)
        self.assertNotIn("SHA256 mismatch", result.stderr)

    def test_conflicting_source_kinds_rejected(self):
        """A remote declaration versus a local-directory declaration."""
        result = self.install(
            self.two_declaring_specs(f'"{self.url_a}"', f'"{self.dir_src}"')
        )

        self.assertNotEqual(0, result.returncode)
        self.assertIn("conflicting sources", result.stderr)

    def test_manifest_versus_spec_conflict_rejected(self):
        """The manifest and a spec disagreeing about the bundle is also a conflict."""
        one = self.write_spec("one", f'"{self.url_a}"')
        manifest = self.test_dir / "envy.lua"
        manifest.write_text(
            make_manifest(
                f"""
BUNDLES = {{
    tc = {{ identity = "test.tc@v1", source = "{self.url_b}" }},
}}

PACKAGES = {{
    {{ spec = "test.one@v1", source = "{one.as_posix()}" }},
    {{ spec = "test.tool@v1", bundle = "tc", setup = {{ "main" }} }},
}}
"""
            ),
            encoding="utf-8",
        )

        result = test_config.run(
            [
                str(self.envy),
                "--cache-root",
                str(self.cache_root),
                "install",
                "--manifest",
                str(manifest),
            ],
            cwd=self.project_root,
            capture_output=True,
            text=True,
        )

        self.assertNotEqual(0, result.returncode)
        self.assertIn("conflicting sources", result.stderr)

    # -- conflicts inside one declaring scope --------------------------------
    #
    # Two aliases (or two DEPENDENCIES entries) in one file share a single memoized
    # bundle cfg, so only bundle::ensure_pkg_cfg can see the disagreement — the
    # engine's check compares two cfgs and this scope produces one.

    def test_two_manifest_aliases_conflicting_rejected(self):
        """Two BUNDLES aliases in one manifest naming one identity, two payloads."""
        manifest = self.test_dir / "envy.lua"
        manifest.write_text(
            make_manifest(
                f"""
BUNDLES = {{
    tc_a = {{ identity = "test.tc@v1", source = "{self.url_a}" }},
    tc_b = {{ identity = "test.tc@v1", source = "{self.url_b}" }},
}}

PACKAGES = {{
    {{ spec = "test.tool@v1", bundle = "tc_a", setup = {{ "main" }} }},
    {{ spec = "test.tool@v1", bundle = "tc_b", setup = {{ "main" }} }},
}}
"""
            ),
            encoding="utf-8",
        )

        result = test_config.run(
            [
                str(self.envy),
                "--cache-root",
                str(self.cache_root),
                "install",
                "--manifest",
                str(manifest),
            ],
            cwd=self.project_root,
            capture_output=True,
            text=True,
        )

        self.assertNotEqual(0, result.returncode)
        self.assertIn("declared more than once", result.stderr)
        self.assertIn("test.tc@v1", result.stderr)
        self.assertIn("envy.lua", result.stderr)

    def test_two_manifest_aliases_agreeing_accepted(self):
        """Two aliases for the same bundle are fine when they name one payload."""
        manifest = self.test_dir / "envy.lua"
        manifest.write_text(
            make_manifest(
                f"""
BUNDLES = {{
    tc_a = {{ identity = "test.tc@v1", source = "{self.url_a}" }},
    tc_b = {{ identity = "test.tc@v1", source = "{self.url_a}" }},
}}

PACKAGES = {{
    {{ spec = "test.tool@v1", bundle = "tc_b", setup = {{ "main" }} }},
}}
"""
            ),
            encoding="utf-8",
        )

        result = test_config.run(
            [
                str(self.envy),
                "--cache-root",
                str(self.cache_root),
                "install",
                "--manifest",
                str(manifest),
            ],
            cwd=self.project_root,
            capture_output=True,
            text=True,
        )

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertNotIn("declared more than once", result.stderr)

    def test_two_dependency_entries_conflicting_rejected(self):
        """One spec declaring the same bundle identity twice, two payloads."""
        spec = self.test_dir / "twice.lua"
        spec.write_text(
            f"""IDENTITY = "test.twice@v1"

DEPENDENCIES = {{
  {{ bundle = "test.tc@v1", source = "{self.url_a}" }},
  {{ bundle = "test.tc@v1", source = "{self.url_b}" }},
}}

FETCH = function(dir, options) end
""",
            encoding="utf-8",
        )

        result = self.install(
            f'    {{ spec = "test.twice@v1", source = "{spec.as_posix()}" }},'
        )

        self.assertNotEqual(0, result.returncode)
        self.assertIn("declared more than once", result.stderr)
        self.assertIn("twice.lua", result.stderr)

    def test_two_manifest_aliases_custom_fetch_warns(self):
        """Two fetch closures in one file cannot be compared: warn, keep the first."""
        manifest = self.test_dir / "envy.lua"
        manifest.write_text(
            make_manifest(
                f"""
BUNDLES = {{
    tc_a = {{ identity = "test.tc@v1", source = {CUSTOM_FETCH_SRC} }},
    tc_b = {{ identity = "test.tc@v1", source = {CUSTOM_FETCH_SRC} }},
}}

PACKAGES = {{
    {{ spec = "test.tool@v1", bundle = "tc_b", setup = {{ "main" }} }},
}}
"""
            ),
            encoding="utf-8",
        )

        result = test_config.run(
            [
                str(self.envy),
                "--cache-root",
                str(self.cache_root),
                "install",
                "--manifest",
                str(manifest),
            ],
            cwd=self.project_root,
            capture_output=True,
            text=True,
        )

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn("more than one custom fetch function", result.stderr)

    # -- agreeing declarations keep working ---------------------------------

    def test_identical_declarations_accepted(self):
        """Two specs declaring the same bundle the same way is normal composition."""
        result = self.install(
            self.two_declaring_specs(f'"{self.url_a}"', f'"{self.url_a}"')
        )

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertNotIn("conflicting", result.stderr)
        # Still one bundle package, so still one outcome row.
        self.assertEqual(1, result.stderr.count("[test.tc@v1] fetched"), result.stderr)

    def test_identical_declarations_with_sha_accepted(self):
        """Matching sha256 on both declarations is agreement, not conflict."""
        sha = hashlib.sha256(
            (self.serve_dir / "bundle-a.tar.gz").read_bytes()
        ).hexdigest()
        result = self.install(
            self.two_declaring_specs(
                f'"{self.url_a}", sha256 = "{sha}"', f'"{self.url_a}", sha256 = "{sha}"'
            )
        )

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertNotIn("conflicting", result.stderr)

    def test_one_spec_declaring_and_consuming_accepted(self):
        """Declaring a bundle and pulling a spec from it in one spec is the base case."""
        result = self.install(self.two_declaring_specs(f'"{self.url_a}"', f'"{self.url_a}"'))
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")

    # -- undecidable declarations warn --------------------------------------

    def test_duplicate_custom_fetch_warns_but_succeeds(self):
        """Two fetch closures cannot be compared, so warn and run the winner."""
        result = self.install(
            self.two_declaring_specs(CUSTOM_FETCH_SRC, CUSTOM_FETCH_SRC)
        )

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn("declares a custom fetch function in both", result.stderr)
        self.assertIn("will run", result.stderr)

    def test_single_custom_fetch_does_not_warn(self):
        """One declarer consumed by many references must stay silent."""
        one = self.write_spec("one", CUSTOM_FETCH_SRC)
        result = self.install(
            f'    {{ spec = "test.one@v1", source = "{one.as_posix()}" }},'
        )

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertNotIn("custom fetch function in both", result.stderr)
        self.assertNotIn("conflicting", result.stderr)


if __name__ == "__main__":
    unittest.main()
