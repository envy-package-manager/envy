"""Tests for '-- @envy cache-posix/cache-win' manifest directive."""

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

# Platform-specific cache directive
IS_WINDOWS = platform.system() == "Windows"
CACHE_DIRECTIVE = "cache-win" if IS_WINDOWS else "cache-posix"

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
    """Tests for '-- @envy cache-posix/cache-win' manifest directive."""

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

    def test_cache_directive_tilde_expansion(self):
        """Cache directive with ~ expands to home directory."""
        # Create a custom cache path using ~
        custom_cache_name = f".envy-cache-test-tilde-{self.unique_suffix}"
        home = Path.home()
        expected_cache = home / custom_cache_name

        try:
            # Note: @envy directive values must be quoted
            manifest = self.create_manifest(
                f"""-- @envy {CACHE_DIRECTIVE} "~/{custom_cache_name}"
PACKAGES = {{
    {{ spec = "local.cache_test_pkg@v1", source = "{self.lua_path(self.spec_path)}" }},
}}
"""
            )

            # Remove any pre-existing cache
            if expected_cache.exists():
                shutil.rmtree(expected_cache)

            # Clear ENVY_CACHE_ROOT to ensure manifest directive is used
            env = test_config.get_test_env()
            env.pop("ENVY_CACHE_ROOT", None)

            result = self.run_sync(manifest, env_override=env)

            self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
            self.assertTrue(
                expected_cache.exists(),
                f"Cache should exist at {expected_cache}. stderr: {result.stderr}",
            )
            # Verify the package was installed in the custom cache
            pkg_path = expected_cache / "packages" / "local.cache_test_pkg@v1"
            self.assertTrue(
                pkg_path.exists(),
                f"Package should exist at {pkg_path}",
            )
        finally:
            # Clean up the custom cache
            if expected_cache.exists():
                shutil.rmtree(expected_cache, ignore_errors=True)

    def test_cache_directive_env_var_expansion(self):
        """Cache directive with $HOME expands environment variable."""
        # Create a custom cache path using $HOME
        # Note: On Windows, envy maps $HOME to USERPROFILE
        custom_cache_name = f".envy-cache-test-env-{self.unique_suffix}"
        home = Path.home()
        expected_cache = home / custom_cache_name

        try:
            # Note: @envy directive values must be quoted
            # $HOME works cross-platform (envy maps it to USERPROFILE on Windows)
            manifest = self.create_manifest(
                f"""-- @envy {CACHE_DIRECTIVE} "$HOME/{custom_cache_name}"
PACKAGES = {{
    {{ spec = "local.cache_test_pkg@v1", source = "{self.lua_path(self.spec_path)}" }},
}}
"""
            )

            # Remove any pre-existing cache
            if expected_cache.exists():
                shutil.rmtree(expected_cache)

            # Clear ENVY_CACHE_ROOT to ensure manifest directive is used
            env = test_config.get_test_env()
            env.pop("ENVY_CACHE_ROOT", None)

            result = self.run_sync(manifest, env_override=env)

            self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
            self.assertTrue(
                expected_cache.exists(),
                f"Cache should exist at {expected_cache}. stderr: {result.stderr}",
            )
        finally:
            # Clean up the custom cache
            if expected_cache.exists():
                shutil.rmtree(expected_cache, ignore_errors=True)

    def test_cli_cache_root_overrides_manifest(self):
        """CLI --cache-root takes precedence over manifest cache directive."""
        cli_cache = self.test_dir / "cli-cache"
        manifest_cache_name = f".envy-cache-test-override-{self.unique_suffix}"
        manifest_cache = Path.home() / manifest_cache_name

        try:
            # Note: @envy directive values must be quoted
            manifest = self.create_manifest(
                f"""-- @envy {CACHE_DIRECTIVE} "~/{manifest_cache_name}"
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
        manifest_cache_name = f".envy-cache-test-env-override-{self.unique_suffix}"
        manifest_cache = Path.home() / manifest_cache_name

        try:
            # Note: @envy directive values must be quoted
            manifest = self.create_manifest(
                f"""-- @envy {CACHE_DIRECTIVE} "~/{manifest_cache_name}"
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

    def test_cache_directive_plain_path(self):
        """Cache directive with plain path uses it directly."""
        custom_cache = self.test_dir / "custom-cache"

        # Note: @envy directive values must be quoted
        manifest = self.create_manifest(
            f"""-- @envy {CACHE_DIRECTIVE} "{self.lua_path(custom_cache)}"
PACKAGES = {{
    {{ spec = "local.cache_test_pkg@v1", source = "{self.lua_path(self.spec_path)}" }},
}}
"""
        )

        # Clear ENVY_CACHE_ROOT to ensure manifest directive is used
        env = test_config.get_test_env()
        env.pop("ENVY_CACHE_ROOT", None)

        result = self.run_sync(manifest, env_override=env)

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertTrue(
            custom_cache.exists(),
            f"Cache should exist at {custom_cache}",
        )
        # Verify the package was installed
        pkg_path = custom_cache / "packages" / "local.cache_test_pkg@v1"
        self.assertTrue(
            pkg_path.exists(),
            f"Package should exist at {pkg_path}",
        )


if __name__ == "__main__":
    unittest.main()
