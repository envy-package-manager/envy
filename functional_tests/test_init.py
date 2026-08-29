"""Tests for envy init command and self-deployment."""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from . import test_config
from .env import EnvyTestCase


def _get_envy_version() -> str:
    """Get the baked-in version from the envy binary."""
    result = subprocess.run(
        [str(test_config.get_envy_production_executable()), "version"],
        capture_output=True,
        text=True,
        timeout=10,
    )
    # First line: "envy version X.Y.Z (...)"
    for line in result.stderr.splitlines():
        if line.startswith("envy version "):
            return line.split()[2]
    raise RuntimeError("Could not parse envy version from: " + result.stderr)


class TestEnvyInit(EnvyTestCase):
    """Test the envy init command."""

    def setUp(self) -> None:
        self._temp_dir = self.make_temp_dir("_temp_dir")
        self._project_dir = self._temp_dir / "project"
        self._bin_dir = self._temp_dir / "project" / "tools"
        self._cache_dir = self._temp_dir / "cache"
        self._envy = test_config.get_envy_production_executable()

    def tearDown(self) -> None:
        if hasattr(self, "_temp_dir") and self._temp_dir.exists():
            shutil.rmtree(self._temp_dir, ignore_errors=True)

    def _run_init(
        self, project_dir: Path | None = None, bin_dir: Path | None = None, **kwargs
    ) -> subprocess.CompletedProcess[str]:
        """Run envy init command."""
        project = str(project_dir or self._project_dir)
        bindir = str(bin_dir or self._bin_dir)
        cmd = [str(self._envy), "init", project, bindir]

        if "mirror" in kwargs:
            cmd.append(f"--mirror={kwargs['mirror']}")
        if "deploy" in kwargs:
            cmd.append(f"--deploy={kwargs['deploy']}")
        if "root" in kwargs:
            cmd.append(f"--root={kwargs['root']}")
        if "platform" in kwargs:
            cmd.append(f"--platform={kwargs['platform']}")
        if kwargs.get("pin_sums"):
            cmd.append("--pin-sums")

        env = test_config.get_test_env()
        env["ENVY_CACHE_ROOT"] = str(self._cache_dir)

        return test_config.run(cmd, capture_output=True, text=True, env=env, timeout=30)

    def _stage_sums_mirror(self, body: bytes | None = None) -> tuple[str, str]:
        """Publish a SHA256SUMS for this build's version on a file:// mirror.

        Returns (mirror uri, expected pin). --pin-sums fetches the file for the running
        version, so the mirror layout has to match what a real release would serve.
        """
        import hashlib

        version = _get_envy_version()
        mirror = self._temp_dir / "mirror"
        (mirror / f"v{version}").mkdir(parents=True, exist_ok=True)
        if body is None:
            body = f"{'a' * 64}  envy-linux-x86_64.tar.gz\n".encode()
        (mirror / f"v{version}" / "SHA256SUMS").write_bytes(body)
        return mirror.as_uri(), hashlib.sha256(body).hexdigest()

    def test_init_creates_all_expected_files(self) -> None:
        """Init creates bootstrap script, manifest, and .luarc.json."""
        result = self._run_init()
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")

        # Check bootstrap script
        import sys

        if sys.platform == "win32":
            bootstrap = self._bin_dir / "envy.bat"
        else:
            bootstrap = self._bin_dir / "envy"
        self.assertTrue(bootstrap.exists(), f"Bootstrap not created at {bootstrap}")

        # Check manifest
        manifest = self._project_dir / "envy.lua"
        self.assertTrue(manifest.exists(), f"Manifest not created at {manifest}")

        # Check .luarc.json
        luarc = self._project_dir / ".luarc.json"
        self.assertTrue(luarc.exists(), f".luarc.json not created at {luarc}")

    def test_init_creates_directories_if_missing(self) -> None:
        """Init creates project and bin directories if they don't exist."""
        nested_project = self._temp_dir / "deep" / "nested" / "project"
        nested_bin = nested_project / "tools"

        result = self._run_init(project_dir=nested_project, bin_dir=nested_bin)
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")

        self.assertTrue(nested_project.exists())
        self.assertTrue(nested_bin.exists())

    def test_init_manifest_contains_envy_version_directive(self) -> None:
        """Manifest contains @envy version directive."""
        result = self._run_init()
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")

        manifest = self._project_dir / "envy.lua"
        content = manifest.read_text()
        self.assertIn("@envy version", content)

    def test_init_does_not_overwrite_existing_manifest(self) -> None:
        """Init does not overwrite existing manifest."""
        self._project_dir.mkdir(parents=True)
        manifest = self._project_dir / "envy.lua"
        manifest.write_text("-- Existing manifest\nPACKAGES = {}\n")

        result = self._run_init()
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")

        content = manifest.read_text()
        self.assertIn("Existing manifest", content)
        self.assertIn("already exists", result.stderr)

    def test_init_bootstrap_is_executable(self) -> None:
        """Bootstrap script has executable permissions (Unix)."""
        import sys

        if sys.platform == "win32":
            self.skipTest("Executable permissions not applicable on Windows")

        result = self._run_init()
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")

        bootstrap = self._bin_dir / "envy"
        self.assertTrue(os.access(bootstrap, os.X_OK))

    def test_init_luarc_is_valid_json(self) -> None:
        """.luarc.json is valid JSON."""
        result = self._run_init()
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")

        luarc = self._project_dir / ".luarc.json"
        content = luarc.read_text()
        data = json.loads(content)

        self.assertIn("workspace.library", data)
        self.assertIn("diagnostics.globals", data)
        self.assertIn("envy", data["diagnostics.globals"])

    def test_init_luarc_points_to_cache(self) -> None:
        """.luarc.json workspace.library points to cache."""
        result = self._run_init()
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")

        luarc = self._project_dir / ".luarc.json"
        data = json.loads(luarc.read_text())

        library_paths = data["workspace.library"]
        # The project-local tree plus the three platform defaults: a committed .luarc.json
        # has to resolve whether or not the reader ran `envy cache --local`.
        self.assertEqual(4, len(library_paths))
        for entry in library_paths:
            self.assertIn("envy", entry)

    def test_init_prints_guidance_when_luarc_exists(self) -> None:
        """Init prints guidance when .luarc.json already exists."""
        self._project_dir.mkdir(parents=True)
        luarc = self._project_dir / ".luarc.json"
        luarc.write_text('{"existing": "config"}\n')

        result = self._run_init()
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")

        # Should print guidance, not overwrite
        self.assertIn("already exists", result.stderr)
        self.assertIn("workspace.library", result.stderr)

        # Original content preserved
        data = json.loads(luarc.read_text())
        self.assertEqual("config", data.get("existing"))

    def test_init_with_mirror_option(self) -> None:
        """--mirror lands in the manifest, which is the only place it belongs.

        The bootstrap scripts parse `@envy mirror` at run time and carry no copy: a stamped
        one is unreachable while the directive exists, and once it is deleted it resolved the
        script to the stale mirror while the re-exec'd binary went to upstream.
        """
        custom_mirror = "https://internal.corp/envy-releases"
        result = self._run_init(mirror=custom_mirror)
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")

        import sys

        manifest = (self._project_dir / "envy.lua").read_text()
        self.assertIn(f'-- @envy mirror "{custom_mirror}"', manifest)

        if sys.platform == "win32":
            bootstrap = self._bin_dir / "envy.bat"
        else:
            bootstrap = self._bin_dir / "envy"

        content = bootstrap.read_text()
        self.assertNotIn(custom_mirror, content)
        # Guards against passing vacuously against an unstamped template.
        self.assertNotIn("@@DOWNLOAD_URL@@", content)

    def test_init_pin_sums_writes_the_directive(self) -> None:
        """--pin-sums records the fetched SHA256SUMS's own hash, not an archive's."""
        mirror, expected = self._stage_sums_mirror()

        result = self._run_init(mirror=mirror, pin_sums=True)
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")

        manifest = (self._project_dir / "envy.lua").read_text()
        self.assertIn(f'-- @envy sha256sums "{expected}"', manifest)
        # The pin is meaningless without a pinned version, and the template stamps one.
        self.assertIn(f'-- @envy version "{_get_envy_version()}"', manifest)

    def test_init_without_pin_sums_writes_no_directive(self) -> None:
        """Attestation is opt-in, and plain `envy init` must not need the network."""
        result = self._run_init()
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")

        manifest = (self._project_dir / "envy.lua").read_text()
        self.assertNotIn("@envy sha256sums", manifest)
        # No stray blank line where the directive would have gone.
        self.assertNotIn("\n\n\n", manifest)

    def test_init_pin_sums_writes_nothing_when_the_fetch_fails(self) -> None:
        """Fail before creating files: a half-initialized unattested project is worse.

        The pin is fetched up front precisely so this case leaves no manifest and no
        bootstrap script behind for someone to run unknowingly.
        """
        result = self._run_init(mirror="file:///nonexistent/mirror", pin_sums=True)

        self.assertNotEqual(0, result.returncode)
        self.assertIn("SHA256SUMS", result.stderr)
        self.assertFalse((self._project_dir / "envy.lua").exists())
        self.assertFalse((self._bin_dir / "envy").exists())
        self.assertFalse((self._bin_dir / "envy.bat").exists())

    def test_init_pinned_manifest_round_trips_through_the_parser(self) -> None:
        """A pin envy writes must be one envy accepts; `envy lua` reloads the manifest."""
        mirror, _ = self._stage_sums_mirror()
        self.assertEqual(0, self._run_init(mirror=mirror, pin_sums=True).returncode)

        script = self._temp_dir / "probe.lua"
        script.write_text("print('ok')\n")

        env = test_config.get_test_env()
        env["ENVY_CACHE_ROOT"] = str(self._cache_dir)
        result = test_config.run(
            [str(self._envy), "lua", str(script)],
            capture_output=True,
            text=True,
            env=env,
            cwd=self._project_dir,
            timeout=30,
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")

    def test_init_extracts_type_definitions_to_cache(self) -> None:
        """Init extracts type definitions to cache."""
        result = self._run_init()
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")

        # Type definitions should be in cache/envy/<version>/envy.lua
        envy_cache = self._cache_dir / "envy"
        self.assertTrue(envy_cache.exists(), f"Envy cache not created at {envy_cache}")

        # Find the version directory (0.0.0 for dev builds)
        version_dirs = [d for d in envy_cache.iterdir() if d.is_dir()]
        self.assertGreater(len(version_dirs), 0, "No version directories in cache")

        types_file = version_dirs[0] / "envy.lua"
        self.assertTrue(types_file.exists(), f"Type definitions not at {types_file}")

        # Verify it's valid Lua type definitions
        content = types_file.read_text()
        self.assertIn("---@meta", content)
        self.assertIn("envy", content)

    def test_init_deploy_true_stamps_directive(self) -> None:
        """--deploy=true stamps deploy directive into manifest."""
        result = self._run_init(deploy="true")
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")

        manifest = self._project_dir / "envy.lua"
        content = manifest.read_text()
        self.assertIn('@envy deploy "true"', content)

    def test_init_deploy_false_stamps_directive(self) -> None:
        """--deploy=false stamps deploy directive into manifest."""
        result = self._run_init(deploy="false")
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")

        manifest = self._project_dir / "envy.lua"
        content = manifest.read_text()
        self.assertIn('@envy deploy "false"', content)

    def test_init_default_deploy_is_true(self) -> None:
        """Without --deploy, manifest has deploy=true by default."""
        result = self._run_init()
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")

        manifest = self._project_dir / "envy.lua"
        content = manifest.read_text()
        self.assertIn('@envy deploy "true"', content)

    def test_init_root_true_stamps_directive(self) -> None:
        """--root=true stamps root directive into manifest."""
        result = self._run_init(root="true")
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")

        manifest = self._project_dir / "envy.lua"
        content = manifest.read_text()
        self.assertIn('@envy root "true"', content)

    def test_init_root_false_stamps_directive(self) -> None:
        """--root=false stamps root directive into manifest."""
        result = self._run_init(root="false")
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")

        manifest = self._project_dir / "envy.lua"
        content = manifest.read_text()
        self.assertIn('@envy root "false"', content)

    def test_init_default_root_is_true(self) -> None:
        """Without --root, manifest has root=true by default."""
        result = self._run_init()
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")

        manifest = self._project_dir / "envy.lua"
        content = manifest.read_text()
        self.assertIn('@envy root "true"', content)

    def test_init_both_deploy_and_root_stamps_both(self) -> None:
        """--deploy and --root together stamp both directives."""
        result = self._run_init(deploy="true", root="false")
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")

        manifest = self._project_dir / "envy.lua"
        content = manifest.read_text()
        self.assertIn('@envy deploy "true"', content)
        self.assertIn('@envy root "false"', content)

    def test_init_platform_posix_creates_only_posix_bootstrap(self) -> None:
        """--platform=posix creates only POSIX bootstrap script."""
        result = self._run_init(platform="posix")
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")

        self.assertTrue((self._bin_dir / "envy").exists())
        self.assertFalse((self._bin_dir / "envy.bat").exists())

    def test_init_platform_windows_creates_only_windows_bootstrap(self) -> None:
        """--platform=windows creates only Windows bootstrap script."""
        result = self._run_init(platform="windows")
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")

        self.assertTrue((self._bin_dir / "envy.bat").exists())
        self.assertFalse((self._bin_dir / "envy").exists())

    def test_init_platform_all_creates_both_bootstraps(self) -> None:
        """--platform=all creates both POSIX and Windows bootstrap scripts."""
        result = self._run_init(platform="all")
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")

        self.assertTrue((self._bin_dir / "envy").exists())
        self.assertTrue((self._bin_dir / "envy.bat").exists())

    @unittest.skipIf(sys.platform == "win32", "Unix permissions test")
    def test_init_platform_all_posix_script_is_executable(self) -> None:
        """--platform=all sets executable bit on POSIX script."""
        result = self._run_init(platform="all")
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")

        self.assertTrue(os.access(self._bin_dir / "envy", os.X_OK))

    def test_init_platform_all_both_contain_envy_managed(self) -> None:
        """--platform=all produces scripts that both contain envy-managed marker."""
        result = self._run_init(platform="all")
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")

        posix_content = (self._bin_dir / "envy").read_text()
        windows_content = (self._bin_dir / "envy.bat").read_text()
        self.assertIn("envy-managed", posix_content)
        self.assertIn("envy-managed", windows_content)


class TestSelfDeployment(EnvyTestCase):
    """Test envy self-deployment on startup."""

    def setUp(self) -> None:
        self._temp_dir = self.make_temp_dir("_temp_dir")
        self._cache_dir = self._temp_dir / "cache"
        self._project_dir = self._temp_dir / "project"
        self._bin_dir = self._temp_dir / "bin"
        self._envy = test_config.get_envy_production_executable()

    def tearDown(self) -> None:
        if hasattr(self, "_temp_dir") and self._temp_dir.exists():
            shutil.rmtree(self._temp_dir, ignore_errors=True)

    def _run_envy(self, *args) -> subprocess.CompletedProcess[str]:
        """Run envy with custom cache root."""
        env = test_config.get_test_env()
        env["ENVY_CACHE_ROOT"] = str(self._cache_dir)

        cmd = [str(self._envy), *args]
        return test_config.run(cmd, capture_output=True, text=True, env=env, timeout=30)

    def _run_envy_with_self_deploy(self) -> subprocess.CompletedProcess[str]:
        """Run an envy command that triggers self-deployment."""
        # Only commands that call cache::ensure() trigger self-deployment
        # init is a good choice as it requires cache but not manifest
        return self._run_envy("init", str(self._project_dir), str(self._bin_dir))

    def test_self_deploy_creates_binary_in_cache(self) -> None:
        """Running a cache-aware command deploys envy binary to cache."""
        import sys

        result = self._run_envy_with_self_deploy()
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")

        # Check binary exists in cache
        envy_cache = self._cache_dir / "envy"
        self.assertTrue(envy_cache.exists())

        version_dirs = [d for d in envy_cache.iterdir() if d.is_dir()]
        self.assertGreater(len(version_dirs), 0)

        if sys.platform == "win32":
            cached_binary = version_dirs[0] / "envy.exe"
        else:
            cached_binary = version_dirs[0] / "envy"

        self.assertTrue(cached_binary.exists(), f"Binary not at {cached_binary}")

    def test_self_deploy_creates_type_definitions(self) -> None:
        """Self-deployment also extracts type definitions."""
        result = self._run_envy_with_self_deploy()
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")

        envy_cache = self._cache_dir / "envy"
        version_dirs = [d for d in envy_cache.iterdir() if d.is_dir()]
        types_file = version_dirs[0] / "envy.lua"

        self.assertTrue(types_file.exists(), f"Types not at {types_file}")
        content = types_file.read_text()
        self.assertIn("---@meta", content)

    def test_self_deploy_fast_path_when_cached(self) -> None:
        """Second run uses fast path (no re-deployment)."""
        import sys

        # First run deploys
        result1 = self._run_envy_with_self_deploy()
        self.assertEqual(0, result1.returncode, f"stderr: {result1.stderr}")

        # Verify binary exists after first run
        envy_cache = self._cache_dir / "envy"
        version_dirs = [d for d in envy_cache.iterdir() if d.is_dir()]
        if sys.platform == "win32":
            cached_binary = version_dirs[0] / "envy.exe"
        else:
            cached_binary = version_dirs[0] / "envy"
        self.assertTrue(cached_binary.exists())

        # Get modification time
        mtime1 = cached_binary.stat().st_mtime

        # Second run should hit fast path (binary not modified)
        # Use a fresh project dir to avoid "manifest already exists" message
        self._project_dir = self._temp_dir / "project2"
        self._bin_dir = self._temp_dir / "bin2"
        result2 = self._run_envy_with_self_deploy()
        self.assertEqual(0, result2.returncode, f"stderr: {result2.stderr}")

        mtime2 = cached_binary.stat().st_mtime
        self.assertEqual(mtime1, mtime2, "Binary should not be modified on second run")

    def test_self_deploy_binary_is_executable(self) -> None:
        """Self-deployed binary has executable permissions (Unix)."""
        import sys

        if sys.platform == "win32":
            self.skipTest("Executable permissions not applicable on Windows")

        result = self._run_envy_with_self_deploy()
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")

        envy_cache = self._cache_dir / "envy"
        version_dirs = [d for d in envy_cache.iterdir() if d.is_dir()]
        cached_binary = version_dirs[0] / "envy"

        self.assertTrue(os.access(cached_binary, os.X_OK))

    def test_self_deploy_cached_binary_works(self) -> None:
        """The cached binary can be executed directly."""
        import sys

        # First, trigger self-deployment
        result = self._run_envy_with_self_deploy()
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")

        # Find and run the cached binary
        envy_cache = self._cache_dir / "envy"
        version_dirs = [d for d in envy_cache.iterdir() if d.is_dir()]

        if sys.platform == "win32":
            cached_binary = version_dirs[0] / "envy.exe"
        else:
            cached_binary = version_dirs[0] / "envy"

        env = test_config.get_test_env()
        env["ENVY_CACHE_ROOT"] = str(self._cache_dir)

        result2 = test_config.run(
            [str(cached_binary), "version"],
            capture_output=True,
            text=True,
            env=env,
            timeout=30,
        )
        self.assertEqual(0, result2.returncode, f"stderr: {result2.stderr}")
        self.assertIn("envy version", result2.stderr)


class TestLatestFileGuarding(EnvyTestCase):
    """Test that the 'latest' pointer only advances forward."""

    def setUp(self) -> None:
        self._temp_dir = self.make_temp_dir("_temp_dir")
        self._cache_dir = self._temp_dir / "cache"
        self._project_dir = self._temp_dir / "project"
        self._bin_dir = self._temp_dir / "bin"
        self._envy = test_config.get_envy_production_executable()
        self._version = _get_envy_version()

    def tearDown(self) -> None:
        if hasattr(self, "_temp_dir") and self._temp_dir.exists():
            shutil.rmtree(self._temp_dir, ignore_errors=True)

    def _run_envy_init(self) -> subprocess.CompletedProcess[str]:
        """Run envy init to trigger self-deployment."""
        env = test_config.get_test_env()
        env["ENVY_CACHE_ROOT"] = str(self._cache_dir)
        cmd = [str(self._envy), "init", str(self._project_dir), str(self._bin_dir)]
        return test_config.run(cmd, capture_output=True, text=True, env=env, timeout=30)

    def test_latest_not_downgraded_by_older_version(self) -> None:
        """Pre-populated latest with higher version is not overwritten."""
        # Pre-populate cache with a fake version higher than the binary's
        envy_dir = self._cache_dir / "envy"
        envy_dir.mkdir(parents=True)
        latest = envy_dir / "latest"
        latest.write_text("999.0.0")

        result = self._run_envy_init()
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")

        # Binary version < 999.0.0, so latest must remain 999.0.0
        self.assertEqual("999.0.0", latest.read_text())

    def test_latest_written_when_no_file_exists(self) -> None:
        """With no pre-existing latest file, self-deploy writes it."""
        result = self._run_envy_init()
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")

        latest = self._cache_dir / "envy" / "latest"
        self.assertTrue(latest.exists(), "latest file should be created")
        self.assertEqual(self._version, latest.read_text())

    def test_latest_overwritten_when_corrupt(self) -> None:
        """Corrupt latest file is overwritten (unparseable current -> write)."""
        envy_dir = self._cache_dir / "envy"
        envy_dir.mkdir(parents=True)
        latest = envy_dir / "latest"
        latest.write_text("not-a-version")

        result = self._run_envy_init()
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")

        # Corrupt current -> overwrite with binary's version
        self.assertEqual(self._version, latest.read_text())



class TestEnvyInitGitignore(EnvyTestCase):
    """`envy init` keeps envy's project-local state out of git.

    Init-only by design: no later command touches a tracked file the user owns. And more
    careful than the manifest/.luarc.json writers, which simply refuse to touch an existing
    file -- appending has failure modes they do not.
    """

    _ENTRIES = (".envy/", ".envy-cache-*")

    def setUp(self) -> None:
        self._temp_dir = self.make_temp_dir("_temp_dir")
        self._project_dir = self._temp_dir / "project"
        self._project_dir.mkdir(parents=True)
        # Only inside a repo: init into a plain directory has no business creating one.
        (self._project_dir / ".git").mkdir()
        self._envy = test_config.get_envy_production_executable()

    def tearDown(self) -> None:
        if hasattr(self, "_temp_dir") and self._temp_dir.exists():
            shutil.rmtree(self._temp_dir, ignore_errors=True)

    def _init(self) -> subprocess.CompletedProcess[str]:
        result = test_config.run(
            [
                str(self._envy),
                "init",
                str(self._project_dir),
                str(self._project_dir / "tools"),
            ],
            capture_output=True,
            text=True,
            env=test_config.get_test_env(),
        )
        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        return result

    @property
    def _gitignore(self) -> Path:
        return self._project_dir / ".gitignore"

    def test_creates_gitignore_when_absent(self) -> None:
        self._init()
        lines = self._gitignore.read_text().splitlines()
        for entry in self._ENTRIES:
            self.assertIn(entry, lines)

    def test_appends_to_an_existing_gitignore(self) -> None:
        self._gitignore.write_text("out/\nbuild/\n")
        self._init()
        lines = self._gitignore.read_text().splitlines()
        self.assertEqual(["out/", "build/", *self._ENTRIES], lines)

    def test_appends_safely_when_the_file_lacks_a_trailing_newline(self) -> None:
        """The corruption case: 'out/' and '.envy/' would fuse into one broken rule.

        That silently stops ignoring two things, with no error anywhere.
        """
        self._gitignore.write_text("out/")
        self._init()
        lines = self._gitignore.read_text().splitlines()
        self.assertEqual(["out/", *self._ENTRIES], lines)

    def test_leaves_the_file_alone_when_already_ignored(self) -> None:
        original = ".envy/\n.envy-cache-*\nout/\n"
        self._gitignore.write_text(original)
        self._init()
        self.assertEqual(original, self._gitignore.read_text())

    def test_recognizes_equivalent_spellings(self) -> None:
        """git honors several forms for the same path; none should produce a duplicate."""
        original = "/.envy/\n**/.envy-cache-*\n"
        self._gitignore.write_text(original)
        self._init()
        self.assertEqual(original, self._gitignore.read_text())

    def test_reinit_does_not_duplicate_entries(self) -> None:
        self._init()
        first = self._gitignore.read_text()
        self._init()
        self.assertEqual(first, self._gitignore.read_text())

    def test_no_gitignore_outside_a_repo(self) -> None:
        """init into a plain directory should not conjure a .gitignore."""
        shutil.rmtree(self._project_dir / ".git")
        self._init()
        self.assertFalse(self._gitignore.exists())

if __name__ == "__main__":
    unittest.main()
