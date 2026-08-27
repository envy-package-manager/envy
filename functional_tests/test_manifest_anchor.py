"""Which project a command operates on, and what anchors that choice.

A bin dir's scripts live in one project but are run from anywhere: `../../B/tools/uv
run ./local.py` has to resolve B's uv, in B's environment. The launcher deployed
beside them injects the global `--project` with its own directory, so the binary stops
rediscovering a project from the caller's CWD. `deploy` refuses a bin dir whose own
upward walk lands somewhere other than the manifest that owns it, because every script
it writes there resolves the project that way.
"""

import os
import shutil
import stat
import sys
import unittest
from pathlib import Path

from . import test_config
from .env import EnvyTestCase

# Pinning a version keeps the shipped launcher off the network: it finds the binary at
# $CACHE/envy/<version>/envy and execs it. The binary is a dev build (0.0.0), which never
# re-execs on a version mismatch.
PINNED_VERSION = "1.2.3"
BINARY_NAME = "envy.exe" if sys.platform == "win32" else "envy"


def _spec(identity: str, products: str) -> str:
    """A user-managed provider, so `envy product` echoes the raw value."""
    return f"""IDENTITY = "{identity}"
PRODUCTS = {products}
USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options) return true end,
    INSTALL = function(pkg_dir, options) end,
  }},
}}
"""


class _AnchorTestBase(EnvyTestCase):
    def setUp(self):
        super().setUp()
        self.tree = self.make_temp_dir("tree").resolve()
        self.outside = self.tree / "outside"
        self.outside.mkdir()

    def project(
        self,
        name: str,
        products: str,
        bin_value: str = "tools",
        directives: str = "",
        root: Path | None = None,
    ) -> Path:
        """A project rooted at `root or tree/name`, providing `products`."""
        root = root or (self.tree / name)
        root.mkdir(parents=True, exist_ok=True)
        (root / bin_value).mkdir(parents=True, exist_ok=True)
        spec = self.write_spec(f"{name}.lua", _spec(f"local.{name}@v1", products),
                               directory=root)
        (root / "envy.lua").write_text(
            f'-- @envy bin "{bin_value}"\n'
            f'-- @envy deploy "true"\n'
            f'-- @envy version "{PINNED_VERSION}"\n'
            f"{directives}"
            f'PACKAGES = {{\n  {{ spec = "local.{name}@v1", '
            f'source = "{self.lua_path(spec)}" }},\n}}\n',
            encoding="utf-8",
        )
        return root

    def seed_launcher_binary(self) -> dict[str, str]:
        """Put this build where the shipped launcher looks, and return its env.

        The launcher runs verbatim -- no stub, no download -- so the `--project`
        injection under test is the one that actually ships.
        """
        cached = self.cache_root / "envy" / PINNED_VERSION / BINARY_NAME
        cached.parent.mkdir(parents=True, exist_ok=True)
        if not cached.exists():
            # Copy on Windows: a symlink needs Developer Mode or an elevated runner.
            if sys.platform == "win32":
                shutil.copy2(self.envy, cached)
            else:
                os.symlink(self.envy, cached)
        env = dict(os.environ)
        env.update(test_config.get_test_env())
        env["ENVY_CACHE_ROOT"] = str(self.cache_root)
        for key in ("ENVY_PROJECT_ROOT", "ENVY_MIRROR"):
            env.pop(key, None)
        return env

    def resolved(self, run) -> list[str]:
        """Manifest paths this run resolved, newest last."""
        return [e.raw["path"] for e in run.events("manifest_resolved")]


class TestAnchorPrecedence(_AnchorTestBase):
    """Two projects, each providing `tool`, plus a directory in neither."""

    def setUp(self):
        super().setUp()
        self.a = self.project("a", '{ tool = "A-tool" }')
        self.b = self.project("b", '{ tool = "B-tool" }')

    def test_project_wins_over_a_cwd_in_another_project(self):
        """The failure mode this exists for: CWD in A, anchor in B, B must answer."""
        run = self.run_envy("--project", self.b / "tools", "product", "tool", cwd=self.a)
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertEqual("B-tool", run.stdout.strip())
        events = run.events("manifest_resolved")
        self.assertTrue(events, "no manifest_resolved event")
        self.assertEqual("project", events[0].raw["mode"])
        self.assertPathContains(events[0].raw["path"], "/b/envy.lua")

    def test_project_resolves_with_no_manifest_above_the_cwd(self):
        run = self.run_envy(
            "--project", self.b / "tools", "product", "tool", cwd=self.outside
        )
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertEqual("B-tool", run.stdout.strip())

    def test_cwd_still_anchors_without_project(self):
        run = self.run_envy("product", "tool", cwd=self.a)
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertEqual("A-tool", run.stdout.strip())
        self.assertEqual("cwd", run.events("manifest_resolved")[0].raw["mode"])

    def test_relative_project_resolves_against_the_cwd(self):
        run = self.run_envy(
            "--project", Path("..") / "b" / "tools", "product", "tool", cwd=self.a
        )
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertEqual("B-tool", run.stdout.strip())

    def test_manifest_outranks_project(self):
        run = self.run_envy(
            "--project", self.b / "tools",
            "product", "tool", "--manifest", self.a / "envy.lua",
            cwd=self.outside,
        )
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertEqual("A-tool", run.stdout.strip())
        self.assertEqual("explicit", run.events("manifest_resolved")[0].raw["mode"])

    def test_project_nonexistent_directory_is_rejected(self):
        run = self.run_envy(
            "--project", self.tree / "nope", "product", "tool", cwd=self.a
        )
        self.assertNotEqual(0, run.returncode)

    def test_discovery_failure_names_the_anchor(self):
        run = self.run_envy("--project", self.outside, "product", "tool", cwd=self.a)
        self.assertNotEqual(0, run.returncode)
        self.assertPathContains(run.stderr, str(self.outside))
        self.assertEqual([], self.resolved(run))

    def test_subproject_anchors_on_the_cwd_not_the_injected_project(self):
        """--subproject means "nearest to where I stand"; a bin dir is not "here"."""
        sub = self.project(
            "sub", '{ tool = "SUB-tool" }',
            directives='-- @envy root "false"\n',
            root=self.a / "sub",
        )
        run = self.run_envy(
            "--project", self.a / "tools", "sync", "--subproject", cwd=sub
        )
        self.assertPathContains(self.resolved(run)[0], "/a/sub/envy.lua")

        # Without --subproject the injected anchor decides, and root=false walks to A.
        run = self.run_envy("--project", self.a / "tools", "product", "tool", cwd=sub)
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertEqual("A-tool", run.stdout.strip())


class TestAnchoredCommands(_AnchorTestBase):
    """--project is global, so every manifest-loading command has to honor it."""

    def setUp(self):
        super().setUp()
        self.a = self.project("a", '{ tool = "A-tool" }')
        self.b = self.project("b", '{ tool = "B-tool" }')

    def assert_anchored(self, *args, expect: Path, ok: bool = True, **kwargs):
        """Assert on the resolved manifest, not the exit code.

        A command can legitimately fail *after* resolving (a user-managed package has no
        cache entry to name), and the resolution is the thing under test.
        """
        run = self.run_envy(*args, cwd=self.a, **kwargs)
        if ok:
            self.assertEqual(0, run.returncode, run.stderr)
        resolved = self.resolved(run)
        self.assertTrue(resolved, f"no manifest_resolved event; stderr: {run.stderr}")
        self.assertPathEndsWith(resolved[0], f"{expect.name}/envy.lua")
        return run

    def test_install_anchors_on_project(self):
        self.assert_anchored("--project", self.b, "install", expect=self.b)

    def test_deploy_anchors_on_project(self):
        self.assert_anchored("--project", self.b, "deploy", expect=self.b)
        self.assertTrue((self.b / "tools" / "tool").exists())
        self.assertFalse((self.a / "tools" / "tool").exists())

    def test_package_anchors_on_project(self):
        # Resolves B, then refuses: a user-managed package has no cache directory to name.
        run = self.assert_anchored(
            "--project", self.b, "package", "local.b@v1", expect=self.b, ok=False
        )
        self.assertIn("not cache-managed", run.stderr)

    def test_export_anchors_on_project(self):
        out = self.tree / "export-out"
        self.assert_anchored(
            "--project", self.b, "export", "--output-dir", out, expect=self.b
        )

    def test_cache_anchors_on_project(self):
        """`envy cache` reads '@envy cache-*' out of the anchored manifest's text."""
        b = self.tree / "cachey"
        (b / "tools").mkdir(parents=True)
        (b / "envy.lua").write_text(
            '-- @envy bin "tools"\n'
            '-- @envy cache-posix "local-cache"\n'
            '-- @envy cache-windows "local-cache"\n'
            "PACKAGES = {}\n",
            encoding="utf-8",
        )
        # No --cache-root: the override short-circuits manifest lookup by design.
        result = test_config.run(
            [str(self.envy), "--project", str(b), "cache"],
            cwd=self.a, capture_output=True, text=True, timeout=30,
        )
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertPathContains(result.stdout + result.stderr, "cachey/local-cache")

    def test_use_anchors_on_project(self):
        """`use` edits a manifest, so anchoring it on the wrong project rewrites it."""
        run = self.run_envy(
            "--project", self.b, "use", "9.9.9", "--force", cwd=self.a
        )
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertIn('"9.9.9"', (self.b / "envy.lua").read_text(encoding="utf-8"))
        self.assertIn(f'"{PINNED_VERSION}"',
                      (self.a / "envy.lua").read_text(encoding="utf-8"))

    @unittest.skipIf(sys.platform == "win32", "POSIX shell script")
    def test_run_anchors_on_project(self):
        """`run` execs, so it emits no trace: assert the PATH the child inherits."""
        for project, name in ((self.a, "only-in-a"), (self.b, "only-in-b")):
            marker = project / "tools" / name
            marker.write_text(f"#!/usr/bin/env bash\necho {name.upper()}\n",
                              encoding="utf-8")
            marker.chmod(0o755)

        run = self.run_envy("--project", self.b, "run", "only-in-b", cwd=self.a)
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertIn("ONLY-IN-B", run.stdout)

        # Without the anchor the CWD decides, and A's bin dir has no only-in-b.
        run = self.run_envy("run", "only-in-b", cwd=self.a)
        self.assertNotEqual(0, run.returncode)
        self.assertNotIn("ONLY-IN-B", run.stdout)

    def test_nearest_is_traced(self):
        """The trace has to say which mode ran, or --subproject bugs are invisible."""
        sub = self.project(
            "sub", '{ tool = "SUB-tool" }',
            directives='-- @envy root "false"\n',
            root=self.a / "sub",
        )
        run = self.run_envy("deploy", "--subproject", cwd=sub)
        event = run.events("manifest_resolved")[0]
        self.assertTrue(event.raw["nearest"])
        self.assertEqual("cwd", event.raw["mode"])

        run = self.run_envy("deploy", cwd=self.a)
        self.assertFalse(run.events("manifest_resolved")[0].raw["nearest"])


class TestDeployedScripts(_AnchorTestBase):
    """The shipped launcher and product scripts, run from a foreign CWD.

    Both platforms: the product script and launcher differ per platform, and the .bat
    half is only ever exercised here -- everything else about it is a string match.
    """

    def setUp(self):
        super().setUp()
        self.a = self.project("a", '{ tool = "A-tool" }')

    def _payload(self, name: str, body: str) -> Path:
        """An executable a product value can point at, in this platform's dialect."""
        if sys.platform == "win32":
            path = self.tree / f"{name}.cmd"
            path.write_text(f"@echo off\r\n{body}\r\n", encoding="utf-8")
            return path
        path = self.tree / f"{name}.sh"
        path.write_text(f"#!/usr/bin/env bash\n{body}\n", encoding="utf-8")
        path.chmod(path.stat().st_mode | stat.S_IXUSR)
        return path

    def _launcher(self, project: Path) -> Path:
        return project / "tools" / ("envy.bat" if sys.platform == "win32" else "envy")

    def _product(self, project: Path, name: str) -> Path:
        return project / "tools" / (f"{name}.bat" if sys.platform == "win32" else name)

    def test_deployed_script_resolves_its_own_project(self):
        inner = self._payload("inner", "echo INNER-OK")
        # `outer` shells out to sibling product `inner` by bare name and reports the
        # project root, so this covers resolution *and* the environment around it.
        outer = self._payload(
            "outer",
            'echo ROOT=%ENVY_PROJECT_ROOT%\r\ncall inner'
            if sys.platform == "win32"
            else 'echo "ROOT=$ENVY_PROJECT_ROOT"\ninner',
        )

        b = self.project(
            "b",
            f'{{ outer = "{self.lua_path(outer)}", inner = "{self.lua_path(inner)}" }}',
        )
        env = self.seed_launcher_binary()
        run = self.sync(b / "envy.lua", cwd=b, env=env)
        self.assertEqual(0, run.returncode, run.stderr)

        result = test_config.run(
            [str(self._product(b, "outer"))],
            cwd=self.a, env=env, capture_output=True, text=True, timeout=60,
        )
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertPathContains(result.stdout, f"ROOT={b}")
        self.assertIn("INNER-OK", result.stdout)

    def test_deployed_launcher_anchors_every_subcommand(self):
        b = self.project("b", '{ tool = "B-tool" }')
        env = self.seed_launcher_binary()
        run = self.sync(b / "envy.lua", cwd=b, env=env)
        self.assertEqual(0, run.returncode, run.stderr)

        result = test_config.run(
            [str(self._launcher(b)), "product", "tool"],
            cwd=self.a, env=env, capture_output=True, text=True, timeout=60,
        )
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertEqual("B-tool", result.stdout.strip())

    def test_a_typed_project_overrides_the_injected_one(self):
        b = self.project("b", '{ tool = "B-tool" }')
        env = self.seed_launcher_binary()
        self.assertEqual(0, self.sync(b / "envy.lua", cwd=b, env=env).returncode)
        self.assertEqual(0, self.sync(self.a / "envy.lua", cwd=self.a, env=env).returncode)

        result = test_config.run(
            [str(self._launcher(b)), "--project", str(self.a), "product", "tool"],
            cwd=self.outside, env=env, capture_output=True, text=True, timeout=60,
        )
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertEqual("A-tool", result.stdout.strip())


class TestNestedSubprojectScripts(_AnchorTestBase):
    """A superproject calling a script that lives in a nested `root "false"` tree.

    The nested tree's bin dir is deployed in its own standalone checkout and comes along
    as committed content, so its scripts run nested without ever being deployed there.
    They must resolve the enclosing project -- that is what `root "false"` declares --
    and must not claim the nested directory as ENVY_PROJECT_ROOT.
    """

    @unittest.skipIf(sys.platform == "win32", "POSIX launcher and product script")
    def test_nested_script_resolves_the_enclosing_project(self):
        payload = self.tree / "payload.sh"
        payload.write_text(
            "#!/usr/bin/env bash\n"
            'echo "ROOT=${ENVY_PROJECT_ROOT:-<unset>}"\n',
            encoding="utf-8",
        )
        payload.chmod(payload.stat().st_mode | stat.S_IXUSR)
        env = self.seed_launcher_binary()

        # Deployed standalone: its own .git makes it the topmost manifest, so the
        # round-trip holds and the bin dir gets its committed scripts.
        sub = self.project(
            "sub", f'{{ subtool = "{self.lua_path(payload)}" }}',
            directives='-- @envy root "false"\n',
        )
        (sub / ".git").mkdir()
        self.assertEqual(0, self.sync(sub / "envy.lua", cwd=sub, env=env).returncode)
        self.assertIn('ENVY_PROJECT_ROOT_HOP=""',
                      (sub / "tools" / "subtool").read_text(encoding="utf-8"))

        # Now nest it, superproject providing the same product, as a superproject reusing
        # the subproject's PACKAGES table does.
        super_root = self.tree / "super"
        (super_root / "src").mkdir(parents=True)
        shutil.copytree(sub, super_root / "src" / "sub")
        shutil.rmtree(super_root / "src" / "sub" / ".git")
        (super_root / "src" / "sub" / ".git").write_text("gitdir: x\n", encoding="utf-8")
        (super_root / ".git").mkdir()
        (super_root / "tools").mkdir()
        (super_root / "envy.lua").write_text(
            (sub / "envy.lua").read_text(encoding="utf-8").replace(
                '-- @envy root "false"', '-- @envy root "true"'
            ),
            encoding="utf-8",
        )
        self.assertEqual(
            0, self.sync(super_root / "envy.lua", cwd=super_root, env=env).returncode
        )

        nested = super_root / "src" / "sub" / "tools" / "subtool"
        result = test_config.run(
            [str(nested)], cwd=super_root, env=env,
            capture_output=True, text=True, timeout=60,
        )
        self.assertEqual(0, result.returncode, result.stderr)
        # Not the nested directory: the manifest that resolved is the superproject's.
        self.assertNotIn("src/sub", result.stdout)

    @unittest.skipIf(sys.platform == "win32", "POSIX launcher")
    def test_a_root_project_does_stamp_its_own_root(self):
        payload = self.tree / "payload.sh"
        payload.write_text(
            '#!/usr/bin/env bash\necho "ROOT=$ENVY_PROJECT_ROOT"\n', encoding="utf-8"
        )
        payload.chmod(payload.stat().st_mode | stat.S_IXUSR)
        env = self.seed_launcher_binary()

        b = self.project("b", f'{{ tool = "{self.lua_path(payload)}" }}')
        self.assertEqual(0, self.sync(b / "envy.lua", cwd=b, env=env).returncode)

        result = test_config.run(
            [str(b / "tools" / "tool")], cwd=self.outside, env=env,
            capture_output=True, text=True, timeout=60,
        )
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertPathContains(result.stdout, f"ROOT={b}")


class TestStampedVersionSkew(_AnchorTestBase):
    """The launcher passes options only its own generation accepts.

    Stamping bin scripts from a build the manifest does not pin leaves every
    './bin/envy' invocation failing on an unrecognized option, because the launcher
    execs the pinned release. Reachable only where re-exec is skipped -- a dev build,
    or ENVY_NO_REEXEC -- which is exactly how an envy developer tests a consumer repo.
    """

    def test_a_pin_the_running_build_does_not_match_warns(self):
        b = self.project("b", '{ tool = "B-tool" }')
        run = self.sync(b / "envy.lua", cwd=b)
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertIn(f"pins {PINNED_VERSION}", run.stderr)
        self.assertIn("Retarget the pin", run.stderr)

    def test_a_matching_pin_is_silent(self):
        b = self.project("b", '{ tool = "B-tool" }')
        env = dict(os.environ)
        env.update(test_config.get_test_env())
        env["ENVY_TEST_SELF_VERSION"] = PINNED_VERSION
        env["ENVY_NO_REEXEC"] = "1"
        run = self.sync(b / "envy.lua", cwd=b, env=env)
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertNotIn("bin scripts stamped from", run.stderr)

    def test_an_unpinned_manifest_is_silent(self):
        """A project that floats to latest has nothing to disagree with."""
        b = self.tree / "floating"
        (b / "tools").mkdir(parents=True)
        spec = self.write_spec("f.lua", _spec("local.f@v1", '{ tool = "F" }'),
                               directory=b)
        (b / "envy.lua").write_text(
            '-- @envy bin "tools"\n-- @envy deploy "true"\n'
            'PACKAGES = { { spec = "local.f@v1", '
            f'source = "{self.lua_path(spec)}" }} }}\n',
            encoding="utf-8",
        )
        run = self.sync(b / "envy.lua", cwd=b)
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertNotIn("bin scripts stamped from", run.stderr)


class TestProductScriptFailures(_AnchorTestBase):
    """What a user sees when a deployed script cannot resolve its product."""

    def _deployed(self) -> tuple[Path, dict[str, str]]:
        b = self.project("b", '{ tool = "B-tool" }')
        env = self.seed_launcher_binary()
        self.assertEqual(0, self.sync(b / "envy.lua", cwd=b, env=env).returncode)
        return b, env

    @unittest.skipIf(sys.platform == "win32", "POSIX product script")
    def test_a_dropped_product_propagates_envys_own_error(self):
        """`|| exit $?`: envy's diagnosis reaches the user instead of an exec failure."""
        b, env = self._deployed()
        # Drop the product but leave the script, as anyone editing a manifest without
        # re-syncing does.
        (b / "b.lua").write_text(_spec("local.b@v1", '{ other = "x" }'), encoding="utf-8")

        result = test_config.run(
            [str(b / "tools" / "tool")],
            cwd=self.outside, env=env, capture_output=True, text=True, timeout=60,
        )
        self.assertEqual(1, result.returncode)
        self.assertIn("'tool' has no provider", result.stderr)
        self.assertNotIn("exec:", result.stderr)

    @unittest.skipIf(sys.platform == "win32", "POSIX product script")
    def test_an_empty_resolution_is_reported_not_exec_d(self):
        """The other half of the guard: a success with no path on stdout."""
        b, env = self._deployed()
        stub = b / "tools" / "envy"
        stub.write_text("#!/usr/bin/env bash\nexit 0\n", encoding="utf-8")
        stub.chmod(0o755)

        result = test_config.run(
            [str(b / "tools" / "tool")],
            cwd=self.outside, env=env, capture_output=True, text=True, timeout=60,
        )
        self.assertEqual(1, result.returncode)
        self.assertIn("envy: failed to resolve product 'tool'", result.stderr)


class TestWindowsProductScript(_AnchorTestBase):
    """The .bat cannot run here; assert the shape cmd.exe will be handed."""

    def test_bat_stamps_the_project_root_and_calls_its_sibling_launcher(self):
        p = self.project("winny", '{ tool = "W-tool" }', bin_value="build/tools")
        run = self.sync(p / "envy.lua", "--platform", "all", cwd=p)
        self.assertEqual(0, run.returncode, run.stderr)

        script = (p / "build" / "tools" / "tool.bat").read_text(encoding="utf-8")
        self.assertNotIn("@@", script)
        # %~dp0 keeps its own trailing backslash here, so the hop concatenates cleanly and
        # the closing quote is never the escaped one.
        self.assertIn('set "ENVY_PROJECT_ROOT_HOP=../.."', script)
        self.assertIn(
            'for %%I in ("%~dp0%ENVY_PROJECT_ROOT_HOP%") do '
            'set "ENVY_PROJECT_ROOT=%%~fI"',
            script,
        )
        self.assertIn('set "PATH=%~dp0.;%PATH%"', script)
        self.assertIn('call "%~dp0envy.bat" product "tool"', script)
        self.assertIn("if not defined PRODUCT_PATH", script)

    def test_bat_scopes_its_environment_writes(self):
        """Without setlocal the PATH and PRODUCT_PATH writes escape into the caller.

        PATH would gain a copy of the bin dir per invocation, and a sibling product
        reached through that PATH would inherit PRODUCT_PATH -- passing the guard and
        re-running the first script's payload forever.
        """
        p = self.project("winny", '{ tool = "W-tool" }')
        run = self.sync(p / "envy.lua", "--platform", "all", cwd=p)
        self.assertEqual(0, run.returncode, run.stderr)

        script = (p / "tools" / "tool.bat").read_text(encoding="utf-8")
        lines = [ln.strip() for ln in script.splitlines()]
        self.assertIn("setlocal", lines)
        # Before any write, and plain: EnableDelayedExpansion would eat a '!' in a path.
        self.assertLess(lines.index("setlocal"),
                        next(i for i, ln in enumerate(lines) if ln.startswith("set ")))
        self.assertNotIn("setlocal EnableDelayedExpansion", lines)

    def test_bat_launcher_injects_the_anchor(self):
        p = self.project("winny", '{ tool = "W-tool" }')
        run = self.sync(p / "envy.lua", "--platform", "all", cwd=p)
        self.assertEqual(0, run.returncode, run.stderr)

        launcher = (p / "tools" / "envy.bat").read_text(encoding="utf-8")
        self.assertIn('--project "%~dp0."', launcher)


class TestBinDirRoundTrip(_AnchorTestBase):
    """deploy refuses a bin dir that cannot resolve the manifest that owns it."""

    def test_bin_escaping_upward_is_refused(self):
        self.project("mono", '{ tool = "MONO-tool" }')
        proj = self.project(
            "proj", '{ tool = "PROJ-tool" }',
            bin_value="../shared-bin",
            root=self.tree / "mono" / "proj",
        )
        run = self.sync(proj / "envy.lua", cwd=proj)
        self.assertNotEqual(0, run.returncode)
        self.assertPathContains(run.stderr, "shared-bin")
        self.assertPathContains(run.stderr, "mono/envy.lua")
        self.assertPathContains(run.stderr, "mono/proj/envy.lua")

    def test_a_non_root_nested_tree_deploys_in_place(self):
        """'@envy root "false"' declares the walk continues past it, so its bin dir
        resolving the enclosing project is the design -- not a layout error.

        This is the workflow that matters: a superproject checkout has to be able to
        restamp a submodule's committed bin dir without cloning it separately.
        """
        self.project("root", '{ tool = "ROOT-tool" }')
        sub = self.project(
            "sub", '{ tool = "SUB-tool" }',
            directives='-- @envy root "false"\n',
            root=self.tree / "root" / "sub",
        )
        # --subproject excludes --manifest, so this anchors on the CWD by design.
        run = self.run_envy("sync", "--subproject", cwd=sub)
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertTrue((sub / "tools" / "tool").exists())
        # It resolved the nested manifest, not the enclosing one.
        self.assertPathEndsWith(self.resolved(run)[0], "root/sub/envy.lua")
        self.assertTrue(run.events("manifest_resolved")[0].raw["nearest"])

    def test_a_nested_deploy_matches_a_standalone_one_byte_for_byte(self):
        """Why deploying nested is safe: nothing in a non-root bin dir names its tree.

        No absolute path, and the project-root stamp is empty for '@envy root "false"',
        so the files a superproject checkout writes are the ones a standalone clone
        writes. If that ever stops holding, the in-place restamp becomes a lie.
        """
        directives = '-- @envy root "false"\n'
        standalone = self.project("alone", '{ tool = "T" }', directives=directives)
        (standalone / ".git").mkdir()
        self.assertEqual(0, self.sync(standalone / "envy.lua", cwd=standalone).returncode)

        self.project("super", '{ tool = "T" }')
        nested = self.project(
            "nested", '{ tool = "T" }', directives=directives,
            root=self.tree / "super" / "src" / "nested",
        )
        (nested / ".git").write_text("gitdir: x\n", encoding="utf-8")
        self.assertEqual(0, self.run_envy("sync", "--subproject", cwd=nested).returncode)

        for name in ("tool", "envy"):
            self.assertEqual(
                (standalone / "tools" / name).read_bytes(),
                (nested / "tools" / name).read_bytes(),
                f"{name} differs between a standalone and a nested deploy",
            )
            self.assertNotIn(
                str(standalone).encode(), (standalone / "tools" / name).read_bytes()
            )

    def test_a_root_manifest_still_cannot_misplace_its_bin_dir(self):
        """The refusal survives where it means something: a root that owns nothing."""
        self.project("mono", '{ tool = "MONO-tool" }')
        proj = self.project(
            "proj", '{ tool = "PROJ-tool" }',
            bin_value="../shared-bin",
            root=self.tree / "mono" / "proj",
        )
        run = self.run_envy("sync", "--subproject", cwd=proj)
        self.assertNotEqual(0, run.returncode)
        self.assertPathContains(run.stderr, "shared-bin")

    def test_a_variant_manifest_warns_rather_than_refusing(self):
        """`--manifest ci.lua` is a workflow, not a broken layout.

        Discovery only looks for envy.lua, so a variant manifest is invisible to the walk
        the deployed scripts do -- no bin dir placement fixes that, and refusing would
        break every variant build in a tree that also has an envy.lua.
        """
        root = self.project("root", '{ tool = "ROOT-tool" }')
        (root / "variants" / "tools").mkdir(parents=True)
        # Same content, different filename: that alone is what discovery cannot see.
        (root / "variants" / "ci.lua").write_text(
            (root / "envy.lua").read_text(encoding="utf-8"), encoding="utf-8"
        )

        run = self.run_envy("sync", "--manifest", root / "variants" / "ci.lua", cwd=root)
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertIn("not named envy.lua", run.stderr)
        self.assertNotIn("has to sit under", run.stderr)

    def test_git_boundary_between_bin_dir_and_manifest_warns(self):
        """A .git in between stops discovery, so the scripts resolve nothing at all."""
        x = self.project("x", '{ tool = "X-tool" }', bin_value="repo/tools")
        (x / "repo" / ".git").mkdir(parents=True, exist_ok=True)
        run = self.sync(x / "envy.lua", cwd=x)
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertIn("no manifest", run.stderr)

        run = self.run_envy(
            "--project", x / "repo" / "tools", "product", "tool", cwd=x
        )
        self.assertNotEqual(0, run.returncode)
        self.assertPathContains(run.stderr, "repo/tools")

    def test_bin_dir_equal_to_the_manifest_dir(self):
        """'@envy bin "."' -- the relative hop to the project root is "." itself."""
        p = self.project("flat", '{ tool = "FLAT-tool" }', bin_value=".")
        run = self.sync(p / "envy.lua", cwd=p)
        self.assertEqual(0, run.returncode, run.stderr)
        script = (p / "tool").read_text(encoding="utf-8")
        self.assertNotIn("@@PROJECT_ROOT_REL@@", script)
        self.assertIn('ENVY_PROJECT_ROOT_HOP="."', script)

    def test_an_ordinary_layout_is_silent(self):
        p = self.project("plain", '{ tool = "PLAIN-tool" }', bin_value="build/tools")
        run = self.sync(p / "envy.lua", cwd=p)
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertNotIn("would resolve", run.stderr)
        self.assertNotIn("no manifest", run.stderr)
        # The stamped hop has to climb out of a nested bin dir, not assume one level.
        script = (p / "build" / "tools" / "tool").read_text(encoding="utf-8")
        self.assertIn("../..", script)


class TestAnchoredExecutionEnvironment(_AnchorTestBase):
    """What runs under an anchor runs in the anchored project, not the caller's tree."""

    @unittest.skipIf(sys.platform == "win32", "POSIX shell verbs")
    def test_default_shell_runs_in_the_manifest_directory(self):
        """DEFAULT_SHELL belongs to the project; the caller's CWD is not its cwd."""
        marker = self.tree / "shell_cwd.txt"
        spec = self.write_spec(
            "shell_provider.lua",
            'IDENTITY = "local.shellp@v1"\n'
            'PRODUCTS = { tool = "B-TOOL" }\n'
            "FETCH = function(fetch_dir, tmp_dir, options) end\n"
            'INSTALL = "true"\n',
            directory=self.tree,
        )
        b = self.tree / "b"
        (b / "tools").mkdir(parents=True)
        (b / "envy.lua").write_text(
            '-- @envy bin "tools"\n'
            "DEFAULT_SHELL = function()\n"
            f'  envy.run("pwd > {marker.as_posix()}")\n'
            "  return ENVY_SHELL.BASH\n"
            "end\n"
            f'PACKAGES = {{ {{ spec = "local.shellp@v1", '
            f'source = "{self.lua_path(spec)}" }} }}\n',
            encoding="utf-8",
        )

        run = self.run_envy("--project", b, "product", "tool", cwd=self.outside)
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertTrue(marker.exists(), "DEFAULT_SHELL never ran")
        self.assertEqual(str(b.resolve()), marker.read_text().strip())


if __name__ == "__main__":
    unittest.main()
