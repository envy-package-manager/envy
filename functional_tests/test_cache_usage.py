"""Functional tests for 'envy cache' (location + disk usage report)."""

import shutil
import sys
import tempfile
import unittest
from pathlib import Path

from . import test_config
from .env import EnvyTestCase


def parse_report(stdout: str) -> tuple[str, dict[str, list[tuple[str, str]]], str]:
    """Split the report into (cache path, {section: [(label, size)]}, total)."""
    root = ""
    total = ""
    sections: dict[str, list[tuple[str, str]]] = {}
    current: list[tuple[str, str]] | None = None

    for line in stdout.splitlines():
        if line.startswith("Cache: "):
            # The report names the tier that decided, e.g. "Cache: /x  (@envy cache-local)".
            # Every reader-vs-reader bug in this area was two things silently disagreeing,
            # so the resolved root now says why it is what it is.
            root = line[len("Cache: ") :].split("  (")[0]
        elif line.endswith(":") and not line.startswith(" "):
            current = sections.setdefault(line[:-1], [])
        elif line.startswith("  ") and line.strip() != "(none)":
            label, _, size = line.strip().rpartition("  ")
            label, size = label.strip(), size.strip()
            if label == "TOTAL":
                total = size
            elif current is not None:
                current.append((label, size))

    return root, sections, total


class TestCacheUsage(EnvyTestCase):
    """'envy cache' reports the cache location and per-entry sizes."""

    # Every invocation self-deploys the (sanitizer-instrumented) binary into the
    # scratch cache root, which is a large copy.
    envy_watchdog_timeout = 60

    def setUp(self):
        super().setUp()

    def run_cache(self):
        result = test_config.run(
            [str(self.envy), "--cache-root", str(self.cache_root), "cache"],
            capture_output=True,
            text=True,
            env=test_config.get_test_env(),
        )
        self.assertEqual(result.returncode, 0, f"cache failed: {result.stderr}")
        return parse_report(result.stdout)

    def write_package(self, identity: str, key: str, size: int):
        entry = self.cache_root / "packages" / identity / key / "pkg"
        entry.mkdir(parents=True, exist_ok=True)
        (entry / "payload.bin").write_bytes(b"\0" * size)
        (entry.parent / "envy-complete").write_bytes(b"")

    def test_empty_cache_reports_location_and_no_packages(self):
        root, sections, total = self.run_cache()

        self.assertEqual(root, str(self.cache_root))
        self.assertEqual(sections["Packages"], [])
        # main() self-deploys before dispatch, so the running version is present.
        self.assertTrue(sections["Envy deployments"], "expected a deployed envy")
        self.assertTrue(total)

    def test_reports_each_package_and_deployment(self):
        self.write_package("pkg.big@1", "darwin-arm64-blake3-aaaa1111", 4096)
        self.write_package("pkg.small@2", "linux-x86_64-blake3-bbbb2222", 1024)

        _, sections, total = self.run_cache()

        packages = dict(sections["Packages"])
        self.assertEqual(packages["pkg.big@1/darwin-arm64-blake3-aaaa1111"], "4.00KB")
        self.assertEqual(packages["pkg.small@2/linux-x86_64-blake3-bbbb2222"], "1.00KB")

        # Largest first: the report exists to show what is worth reclaiming.
        labels = [label for label, _ in sections["Packages"]]
        self.assertEqual(labels[0], "pkg.big@1/darwin-arm64-blake3-aaaa1111")

        deployed = sections["Envy deployments"]
        self.assertEqual(len(deployed), 1, f"expected one deployment, got {deployed}")
        self.assertNotEqual(deployed[0][1], "0B")
        self.assertTrue(total)

    def test_nested_package_contents_are_summed(self):
        entry = self.cache_root / "packages" / "pkg.deep@1" / "darwin-arm64-blake3-cccc"
        deep = entry / "pkg" / "a" / "b" / "c"
        deep.mkdir(parents=True)
        (deep / "one.bin").write_bytes(b"\0" * 1024)
        (entry / "pkg" / "a" / "two.bin").write_bytes(b"\0" * 1024)

        _, sections, _ = self.run_cache()

        packages = dict(sections["Packages"])
        self.assertEqual(packages["pkg.deep@1/darwin-arm64-blake3-cccc"], "2.00KB")

    def test_manifest_directive_selects_reported_root(self):
        """The report follows the manifest's cache directive, anchored to the manifest.

        Reporting the platform default while every other command uses the project's tree
        would send a reader to an empty directory.
        """
        project = self.make_temp_dir("project")
        self.addCleanup(shutil.rmtree, project, ignore_errors=True)
        (project / "envy.lua").write_bytes(
            b'-- @envy cache-local "relcache"\n\nPACKAGES = {}\n'
        )
        sub = project / "sub"
        sub.mkdir()

        # No --cache-root and no ENVY_CACHE_ROOT: the directive is the tier under test.
        # The default-root variables are redirected into the temp tree so main()'s
        # pre-dispatch self-deploy cannot reach the developer's real cache.
        env = test_config.get_test_env()
        env.pop("ENVY_CACHE_ROOT", None)
        sandbox = project / "home"
        env["HOME"] = str(sandbox)
        env["USERPROFILE"] = str(sandbox)
        env["XDG_CACHE_HOME"] = str(sandbox / "cache")
        env["LOCALAPPDATA"] = str(sandbox / "AppData" / "Local")

        # From a subdirectory: discovery walks up, and the directive anchors to what it
        # finds, not to the cwd.
        result = test_config.run(
            [str(self.envy), "cache"],
            cwd=sub,
            capture_output=True,
            text=True,
            env=env,
        )
        self.assertEqual(result.returncode, 0, f"cache failed: {result.stderr}")

        # Both sides resolved: the manifest path envy anchored to came from the cwd it was
        # handed, which on a Windows runner carries 8.3 short components (RUNNER~1) that
        # Path.resolve() expands, and on macOS a /var -> /private/var symlink.
        root, _, _ = parse_report(result.stdout)
        self.assertEqual(Path(root).resolve(), (project / "relcache").resolve())

    def test_override_skips_manifest_discovery(self):
        """`--cache-root` decides alone: no manifest above the cwd is even read.

        Discovery and directive parsing both throw, so consulting a manifest that cannot
        change the answer would turn any broken envy.lua in an ancestor directory into a
        failed report.
        """
        project = self.make_temp_dir("project")
        self.addCleanup(shutil.rmtree, project, ignore_errors=True)
        # A sums pin with no '@envy version' to pin it to: parse_envy_meta rejects it.
        (project / "envy.lua").write_bytes(
            b'-- @envy sha256sums "' + b"a" * 64 + b'"\n\nPACKAGES = {}\n'
        )

        result = test_config.run(
            [str(self.envy), "--cache-root", str(self.cache_root), "cache"],
            cwd=project,
            capture_output=True,
            text=True,
            env=test_config.get_test_env(),
        )

        self.assertEqual(result.returncode, 0, f"cache failed: {result.stderr}")
        root, _, _ = parse_report(result.stdout)
        self.assertEqual(root, str(self.cache_root))

    def test_override_skips_manifest_discovery_for_every_cache_consumer(self):
        """Not just `envy cache`: every path into the cache must honor the override alone.

        Three separate call sites resolved the cache root by discovering a manifest even when
        `--cache-root` already decided it, and `discover()` parses directives and throws --
        so one stale directive anywhere above the cwd broke commands that named their cache
        explicitly.
        """
        project = self.make_temp_dir("project")
        self.addCleanup(shutil.rmtree, project, ignore_errors=True)
        # A removed directive: parse_envy_meta throws on it by design.
        (project / "envy.lua").write_bytes(
            b'-- @envy cache-posix "/opt/whatever"\n\nPACKAGES = {}\n'
        )

        env = test_config.get_test_env()
        env.pop("ENVY_CACHE_ROOT", None)

        for args in (
            ["cache"],
            ["cache", "--root"],
            ["import", str(project / "missing.zst")],
            ["shell", "zsh"],
        ):
            with self.subTest(args=args):
                result = test_config.run(
                    [str(self.envy), "--cache-root", str(self.cache_root), *args],
                    cwd=project,
                    capture_output=True,
                    text=True,
                    env=env,
                )
                # It may still fail on its own arguments; it must not fail on the manifest.
                self.assertNotIn("cache-posix", result.stderr)
                self.assertNotIn("directive removed", result.stderr)

    def test_shell_only_warns_when_the_hook_is_somewhere_unusual(self):
        """The warning is about losing hooks, so it must key on the root, not the tier.

        `@envy cache-mode "shared"` reports a non-default tier while resolving to the plain
        platform default; warning that *that* cache is easily deleted is just wrong.
        """
        project = self.make_temp_dir("shellproj")
        self.addCleanup(shutil.rmtree, project, ignore_errors=True)
        home = project / "home"
        home.mkdir()
        env = test_config.get_test_env()
        env.pop("ENVY_CACHE_ROOT", None)
        env["HOME"] = str(home)
        env["USERPROFILE"] = str(home)
        env["XDG_CACHE_HOME"] = str(home / "cache")
        env["LOCALAPPDATA"] = str(home / "AppData" / "Local")

        def run_shell() -> str:
            result = test_config.run(
                [str(self.envy), "shell", "zsh"],
                cwd=project,
                capture_output=True,
                text=True,
                env=env,
            )
            return result.stdout + result.stderr

        # Declares where --local would go, but defaults to the user-wide cache.
        (project / "envy.lua").write_bytes(
            b'-- @envy bin "bin"\n'
            b'-- @envy cache-local "out/.envy"\n'
            b'-- @envy cache-mode "shared"\n'
            b"PACKAGES = {}\n"
        )
        self.assertNotIn("Moving or deleting", run_shell())

        # Actually local: the hooks really are inside a tree an rm -rf will take.
        (project / "envy.lua").write_bytes(
            b'-- @envy bin "bin"\n-- @envy cache-local "out/.envy"\nPACKAGES = {}\n'
        )
        self.assertIn("Moving or deleting", run_shell())

    def test_non_package_directories_are_reported(self):
        specs = self.cache_root / "specs"
        specs.mkdir(parents=True)
        (specs / "some.spec@1.lua").write_bytes(b"\0" * 2048)

        _, sections, _ = self.run_cache()

        other = dict(sections["Other"])
        self.assertEqual(other["specs"], "2.00KB")


if __name__ == "__main__":
    unittest.main()
