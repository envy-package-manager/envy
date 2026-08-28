"""Tests for the cache-root tiers: @envy cache-local / cache-mode / state-dir.

Covers the directive grammar (relative literal, no expansion of any kind), the mode
tiers, and the zero-byte override markers `envy cache --local/--shared` writes.
"""

import hashlib
import io
import os
import platform
import shutil
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path

from . import test_config
from .env import EnvyTestCase
from .test_config import make_manifest

IS_WINDOWS = platform.system() == "Windows"

# One directive for every platform now: cache-local is relative to the manifest, and a
# relative project path is the same string everywhere. The cache-posix/cache-win split
# existed only because those directives held absolute paths.
CACHE_DIRECTIVE = "cache-local"

# Test archive contents
TEST_ARCHIVE_FILES = {
    "root/file1.txt": "Test file content\n",
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


class TestCacheDirective(EnvyTestCase):
    """Tests for the cache-root tiers: @envy cache-local / cache-mode / state-dir.

Covers the directive grammar (relative literal, no expansion of any kind), the mode
tiers, and the zero-byte override markers `envy cache --local/--shared` writes.
"""

    def setUp(self):
        self.test_dir = self.make_temp_dir("test_dir")
        self.envy = test_config.get_envy_executable()
        self.project_root = Path(__file__).parent.parent
        # pid distinguishes between test runs, unique_suffix distinguishes parallel threads
        self.unique_suffix = f"{os.getpid()}-{self.test_dir.name.split('-')[-1]}"

        # Create test archive and spec
        self.archive_path = self.test_dir / "test.tar.gz"
        self.archive_hash = create_test_archive(self.archive_path)

        # Write inline spec to temp directory
        self.spec_content = f"""-- Test cache-managed package
IDENTITY = "local.cache_test_pkg@v1"

FETCH = {{
  source = "{self.archive_path.as_posix()}",
  sha256 = "{self.archive_hash}"
}}

STAGE = {{strip = 1}}

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  envy.run([[echo 'built' > built.txt]])
end
"""
        self.spec_path = self.test_dir / "cache_test_pkg.lua"
        self.spec_path.write_text(self.spec_content, encoding="utf-8")

    def tearDown(self):
        shutil.rmtree(self.test_dir, ignore_errors=True)

    @staticmethod
    def lua_path(path: Path) -> str:
        """Convert path to Lua-safe string (forward slashes work on all platforms)."""
        return path.as_posix()

    def create_manifest(self, content: str) -> Path:
        """Create manifest file with given content."""
        manifest_path = self.test_dir / "envy.lua"
        manifest_path.write_text(make_manifest(content), encoding="utf-8")
        return manifest_path

    def run_sync(
        self,
        manifest: Path,
        cache_root: str | None = None,
        env_override: dict | None = None,
        cwd: Path | None = None,
    ):
        """Run 'envy sync' command and return result."""
        cmd = [str(self.envy)]
        if cache_root:
            cmd.extend(["--cache-root", cache_root])
        cmd.extend(["install", "--manifest", str(manifest)])

        env = env_override if env_override else test_config.get_test_env()

        result = test_config.run(
            cmd,
            cwd=cwd or self.project_root,
            capture_output=True,
            text=True,
            env=env,
        )
        return result

    def test_cli_cache_root_overrides_manifest(self):
        """CLI --cache-root takes precedence over manifest cache directive."""
        cli_cache = self.test_dir / "cli-cache"
        manifest_cache = self.test_dir / "relcache"

        try:
            manifest = self.create_manifest(
                f"""-- @envy {CACHE_DIRECTIVE} "relcache"
PACKAGES = {{
    {{ spec = "local.cache_test_pkg@v1", source = "{self.lua_path(self.spec_path)}" }},
}}
"""
            )

            # Remove any pre-existing caches
            if manifest_cache.exists():
                shutil.rmtree(manifest_cache)

            # Clear ENVY_CACHE_ROOT to isolate CLI vs manifest behavior
            env = test_config.get_test_env()
            env.pop("ENVY_CACHE_ROOT", None)

            result = self.run_sync(
                manifest, cache_root=str(cli_cache), env_override=env
            )

            self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
            # CLI cache should be used
            self.assertTrue(
                cli_cache.exists(),
                f"CLI cache should exist at {cli_cache}",
            )
            # Manifest cache should NOT be created
            self.assertFalse(
                manifest_cache.exists(),
                f"Manifest cache should NOT exist at {manifest_cache}",
            )
        finally:
            if manifest_cache.exists():
                shutil.rmtree(manifest_cache, ignore_errors=True)

    def test_env_cache_root_overrides_manifest(self):
        """ENVY_CACHE_ROOT env takes precedence over manifest cache directive."""
        env_cache = self.test_dir / "env-cache"
        manifest_cache = self.test_dir / "relcache"

        try:
            manifest = self.create_manifest(
                f"""-- @envy {CACHE_DIRECTIVE} "relcache"
PACKAGES = {{
    {{ spec = "local.cache_test_pkg@v1", source = "{self.lua_path(self.spec_path)}" }},
}}
"""
            )

            # Remove any pre-existing caches
            if manifest_cache.exists():
                shutil.rmtree(manifest_cache)

            # Set ENVY_CACHE_ROOT to override manifest directive
            env = test_config.get_test_env()
            env["ENVY_CACHE_ROOT"] = str(env_cache)

            result = self.run_sync(manifest, env_override=env)

            self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
            # Env cache should be used
            self.assertTrue(
                env_cache.exists(),
                f"Env cache should exist at {env_cache}",
            )
            # Manifest cache should NOT be created
            self.assertFalse(
                manifest_cache.exists(),
                f"Manifest cache should NOT exist at {manifest_cache}",
            )
        finally:
            if manifest_cache.exists():
                shutil.rmtree(manifest_cache, ignore_errors=True)

    def test_relative_cache_directive_anchors_to_manifest(self):
        """A relative cache directive resolves against the manifest dir, not the cwd.

        Anchoring to the cwd builds a separate cache tree per working directory, silently
        refetching every package on each invocation from a subdirectory.
        """
        rel_name = "relcache"
        expected_cache = self.test_dir / rel_name
        subdirs = [self.test_dir / "sub", self.test_dir / "sub" / "nested"]
        subdirs[1].mkdir(parents=True)

        manifest = self.create_manifest(
            f"""-- @envy {CACHE_DIRECTIVE} "{rel_name}"
PACKAGES = {{
    {{ spec = "local.cache_test_pkg@v1", source = "{self.lua_path(self.spec_path)}" }},
}}
"""
        )

        # Clear ENVY_CACHE_ROOT to ensure manifest directive is used
        env = test_config.get_test_env()
        env.pop("ENVY_CACHE_ROOT", None)

        for cwd in subdirs:
            result = self.run_sync(manifest, env_override=env, cwd=cwd)
            self.assertEqual(result.returncode, 0, f"cwd={cwd} stderr: {result.stderr}")

        pkg_path = expected_cache / "packages" / "local.cache_test_pkg@v1"
        self.assertTrue(
            pkg_path.exists(),
            f"Package should exist at {pkg_path}",
        )
        # One tree, not one per cwd
        for cwd in subdirs:
            self.assertFalse(
                (cwd / rel_name).exists(),
                f"Cache should not be anchored to the cwd at {cwd / rel_name}",
            )


    # ---- grammar: relative literal, no expansion of any kind ------------------------

    def _expect_rejected(self, value: str, directive: str = CACHE_DIRECTIVE) -> str:
        """Assert a directive value is rejected, and return stderr for further checks."""
        manifest = self.create_manifest(
            f'''-- @envy {directive} "{value}"
PACKAGES = {{}}
'''
        )
        env = test_config.get_test_env()
        env.pop("ENVY_CACHE_ROOT", None)
        result = self.run_sync(manifest, env_override=env)
        self.assertNotEqual(0, result.returncode, f"'{value}' was accepted")
        return result.stderr

    def test_cache_local_rejects_expansion_forms(self):
        """Every form wordexp() used to accept is now an error, not an expansion.

        The launchers cannot implement shell word expansion, and the four readers disagreed
        about all of these -- that disagreement is the bug this replaced.
        """
        for value in ("~", "~/cache", "$HOME/cache", "${HOME}/cache", "%LOCALAPPDATA%/x"):
            with self.subTest(value=value):
                stderr = self._expect_rejected(value)
                self.assertIn("cache-local", stderr)

    def test_cache_local_rejects_absolute_and_escaping_paths(self):
        """cache-local names a project subdirectory; anything else is ENVY_CACHE_ROOT's job.

        '..' matters most: it defeats "delete the build root and every trace is gone".
        """
        for value in ("/opt/cache", "..", "../sibling", "out/../..", "."):
            with self.subTest(value=value):
                self._expect_rejected(value)

    def test_state_dir_shares_the_same_grammar(self):
        for value in ("~/state", "$HOME/state", "/var/state", ".."):
            with self.subTest(value=value):
                stderr = self._expect_rejected(value, directive="state-dir")
                self.assertIn("state-dir", stderr)

    def test_removed_directives_name_their_replacement(self):
        """cache-posix/cache-win throw rather than being silently ignored.

        Unknown keys fall off parse_envy_meta's chain without a word, so a bare rename would
        have dropped the directive and handed the project the shared cache in silence.
        """
        for old in ("cache-posix", "cache-win"):
            with self.subTest(directive=old):
                stderr = self._expect_rejected("out/.envy", directive=old)
                self.assertIn("cache-local", stderr)
                self.assertIn("ENVY_CACHE_ROOT", stderr)

    def test_relative_env_cache_root_is_rejected(self):
        """A relative override named two different trees in one invocation.

        The binary anchored it to its own cwd while both launchers took it verbatim.
        """
        manifest = self.create_manifest("PACKAGES = {}\n")
        env = test_config.get_test_env()
        env["ENVY_CACHE_ROOT"] = "relative-cache"
        result = self.run_sync(manifest, env_override=env)
        self.assertNotEqual(0, result.returncode)
        self.assertIn("absolute", result.stderr)

    def test_nesting_state_dir_and_cache_local_is_rejected(self):
        """Markers under the cache root would vanish with a cache wipe.

        Equal is allowed -- that is a project asking for co-located teardown on purpose.
        """
        manifest = self.create_manifest(
            '''-- @envy cache-local "out/.envy"
-- @envy state-dir "out/.envy/state"
PACKAGES = {}
'''
        )
        env = test_config.get_test_env()
        env.pop("ENVY_CACHE_ROOT", None)
        result = self.run_sync(manifest, env_override=env)
        self.assertNotEqual(0, result.returncode)
        self.assertIn("nest", result.stderr)

    # ---- mode tiers -----------------------------------------------------------------

    def _cache_root(self, manifest_dir: Path, env: dict | None = None) -> str:
        """`envy cache --root`: the resolved root alone, with no usage scan."""
        e = env if env is not None else test_config.get_test_env()
        e.pop("ENVY_CACHE_ROOT", None)
        result = test_config.run(
            [str(self.envy), "cache", "--root"],
            cwd=manifest_dir,
            capture_output=True,
            text=True,
            env=e,
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        return result.stdout.strip()

    def _sandbox_env(self) -> dict:
        """Redirect the platform-default cache into the temp tree.

        Without this a shared-mode assertion would touch the developer's real cache.
        """
        env = test_config.get_test_env()
        env.pop("ENVY_CACHE_ROOT", None)
        home = self.test_dir / "home"
        home.mkdir(exist_ok=True)
        env["HOME"] = str(home)
        env["USERPROFILE"] = str(home)
        env["XDG_CACHE_HOME"] = str(home / "cache")
        env["LOCALAPPDATA"] = str(home / "AppData" / "Local")
        return env

    def test_no_directives_resolves_shared(self):
        """Today's behavior for every existing manifest: the user-wide cache."""
        self.create_manifest("PACKAGES = {}\n")
        env = self._sandbox_env()
        root = Path(self._cache_root(self.test_dir, env))
        self.assertFalse(
            str(root).startswith(str(self.test_dir.resolve()) + os.sep),
            f"expected a shared root, got {root}",
        )

    def test_cache_local_implies_local_mode(self):
        """Naming a tree is asking for it; a second directive to activate it would be a trap."""
        self.create_manifest(
            '''-- @envy cache-local "out/.envy"
PACKAGES = {}
'''
        )
        root = Path(self._cache_root(self.test_dir, self._sandbox_env()))
        self.assertEqual((self.test_dir / "out" / ".envy").resolve(), root.resolve())

    def test_cache_mode_shared_overrides_the_implication(self):
        """Declares where --local would put the tree while still defaulting to shared."""
        self.create_manifest(
            '''-- @envy cache-local "out/.envy"
-- @envy cache-mode "shared"
PACKAGES = {}
'''
        )
        env = self._sandbox_env()
        root = Path(self._cache_root(self.test_dir, env))
        self.assertNotEqual((self.test_dir / "out" / ".envy").resolve(), root.resolve())

    def test_cache_mode_local_without_cache_local_uses_the_default_tree(self):
        self.create_manifest(
            '''-- @envy cache-mode "local"
PACKAGES = {}
'''
        )
        root = Path(self._cache_root(self.test_dir, self._sandbox_env()))
        self.assertEqual((self.test_dir / ".envy" / "cache").resolve(), root.resolve())

    def test_invalid_cache_mode_value_is_rejected(self):
        stderr = self._expect_rejected("sometimes", directive="cache-mode")
        self.assertIn("cache-mode", stderr)

    # ---- override markers -----------------------------------------------------------

    def _run_cache(self, args: list[str], env: dict) -> subprocess.CompletedProcess:
        return test_config.run(
            [str(self.envy), "cache", *args],
            cwd=self.test_dir,
            capture_output=True,
            text=True,
            env=env,
        )

    def test_markers_round_trip_and_only_exist_when_they_differ(self):
        """A marker records a divergence from the project's default, never a restatement.

        `--local` on a project that already defaults local removes both markers instead of
        writing a redundant one, so the steady state for most projects is no markers at all.
        """
        self.create_manifest(
            '''-- @envy cache-local "out/.envy"
PACKAGES = {}
'''
        )
        env = self._sandbox_env()
        local_marker = self.test_dir / ".envy-cache-local"
        shared_marker = self.test_dir / ".envy-cache-shared"

        # Project default is local, so nothing is recorded yet.
        self.assertFalse(local_marker.exists())
        self.assertFalse(shared_marker.exists())
        local_root = Path(self._cache_root(self.test_dir, dict(env)))

        # Opting into shared diverges from the default, so it is recorded.
        self.assertEqual(0, self._run_cache(["--shared"], dict(env)).returncode)
        self.assertTrue(shared_marker.exists())
        self.assertFalse(local_marker.exists())
        self.assertEqual(b"", shared_marker.read_bytes(), "marker must be zero-byte")
        shared_root = Path(self._cache_root(self.test_dir, dict(env)))
        self.assertNotEqual(local_root, shared_root)

        # Back to local, which matches the default, so both markers go away.
        self.assertEqual(0, self._run_cache(["--local"], dict(env)).returncode)
        self.assertFalse(shared_marker.exists())
        self.assertFalse(local_marker.exists())
        self.assertEqual(local_root, Path(self._cache_root(self.test_dir, dict(env))))

    def test_marker_opts_a_shared_default_project_into_local(self):
        """The other direction: a public project defaults shared, a user wants hermetic."""
        self.create_manifest("PACKAGES = {}\n")
        env = self._sandbox_env()

        self.assertEqual(0, self._run_cache(["--local"], dict(env)).returncode)
        self.assertTrue((self.test_dir / ".envy-cache-local").exists())
        self.assertEqual(
            (self.test_dir / ".envy" / "cache").resolve(),
            Path(self._cache_root(self.test_dir, dict(env))).resolve(),
        )

    def test_setting_a_mode_succeeds_on_a_fresh_clone(self):
        """util_write_file cannot create its parent, so a relocated state dir needs one made.

        Without it `--local` threw on any project whose state dir did not already exist.
        """
        self.create_manifest(
            '''-- @envy state-dir "out/state"
PACKAGES = {}
'''
        )
        env = self._sandbox_env()
        result = self._run_cache(["--local"], dict(env))
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertTrue((self.test_dir / "out" / "state" / ".envy-cache-local").exists())

    def test_both_markers_is_an_error(self):
        """A state envy never writes, so it means someone hand-edited; do not guess."""
        self.create_manifest("PACKAGES = {}\n")
        (self.test_dir / ".envy-cache-local").write_bytes(b"")
        (self.test_dir / ".envy-cache-shared").write_bytes(b"")

        env = self._sandbox_env()
        result = self._run_cache(["--root"], env)
        self.assertNotEqual(0, result.returncode)
        self.assertIn("both", result.stderr)

    def test_state_dir_relocation_moves_where_markers_live(self):
        self.create_manifest(
            '''-- @envy state-dir "out/state"
PACKAGES = {}
'''
        )
        env = self._sandbox_env()
        self.assertEqual(0, self._run_cache(["--local"], dict(env)).returncode)
        self.assertTrue((self.test_dir / "out" / "state" / ".envy-cache-local").exists())
        self.assertFalse((self.test_dir / ".envy-cache-local").exists())

    def test_report_names_the_tier_that_decided(self):
        """Every bug in this area was two readers silently disagreeing."""
        self.create_manifest(
            '''-- @envy cache-local "out/.envy"
PACKAGES = {}
'''
        )
        result = self._run_cache([], self._sandbox_env())
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertIn("@envy cache-local", result.stdout)

    def test_root_prints_only_the_root(self):
        """--root is the binary half of the launcher parity test, so it must stay parseable."""
        self.create_manifest(
            '''-- @envy cache-local "out/.envy"
PACKAGES = {}
'''
        )
        result = self._run_cache(["--root"], self._sandbox_env())
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(1, len(result.stdout.strip().splitlines()))
        self.assertEqual(
            (self.test_dir / "out" / ".envy").resolve(),
            Path(result.stdout.strip()).resolve(),
        )


    # ---- first-run notice ------------------------------------------------------------

    def _install(self, env: dict) -> subprocess.CompletedProcess:
        return test_config.run(
            [str(self.envy), "install"],
            cwd=self.test_dir,
            capture_output=True,
            text=True,
            env=env,
        )

    def test_notice_names_the_shared_root_and_offers_local(self):
        """Told, not asked: a prompt would hang CI and any non-TTY stdin.

        It fires before packages land, which is the only moment where knowing is useful.
        """
        self.create_manifest("PACKAGES = {}\n")
        result = self._install(self._sandbox_env())
        self.assertIn("caching packages in", result.stderr)
        self.assertIn("other envy projects", result.stderr)
        self.assertIn("cache --local", result.stderr)

    def test_notice_reverses_for_a_local_project(self):
        self.create_manifest(
            '''-- @envy cache-local "out/.envy"
PACKAGES = {}
'''
        )
        result = self._install(self._sandbox_env())
        self.assertIn("caching packages in", result.stderr)
        self.assertIn("deleting it removes them", result.stderr)
        self.assertIn("cache --shared", result.stderr)

    def test_notice_stops_once_the_tree_holds_packages(self):
        """Keyed on packages/, not on the root.

        main()'s pre-dispatch self-deploy creates <root>/envy/<version>/ before any command
        runs, so a root-existence trigger was already false by the time anything could report
        it -- the notice never fired at all.
        """
        self.create_manifest(
            '''-- @envy cache-local "out/.envy"
PACKAGES = {}
'''
        )
        env = self._sandbox_env()
        self.assertIn("caching packages in", self._install(dict(env)).stderr)

        # Stand in for a populated cache: the first real entry is what creates packages/.
        (self.test_dir / "out" / ".envy" / "packages").mkdir(parents=True, exist_ok=True)
        self.assertNotIn("caching packages in", self._install(dict(env)).stderr)

    def test_notice_returns_after_a_teardown(self):
        """`rm -rf` the tree and the notice is useful again, with no state to reset."""
        self.create_manifest(
            '''-- @envy cache-local "out/.envy"
PACKAGES = {}
'''
        )
        env = self._sandbox_env()
        self._install(dict(env))
        (self.test_dir / "out" / ".envy" / "packages").mkdir(parents=True, exist_ok=True)
        self.assertNotIn("caching packages in", self._install(dict(env)).stderr)

        shutil.rmtree(self.test_dir / "out")
        self.assertIn("caching packages in", self._install(dict(env)).stderr)

    def test_notice_never_pollutes_stdout(self):
        """`envy cache --root` is the binary half of the launcher parity test."""
        self.create_manifest(
            '''-- @envy cache-local "out/.envy"
PACKAGES = {}
'''
        )
        result = self._run_cache(["--root"], self._sandbox_env())
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        self.assertEqual(1, len(result.stdout.strip().splitlines()))
        self.assertNotIn("caching packages", result.stdout)


if __name__ == "__main__":
    unittest.main()
