"""Product scripts run their tool in the project's activated environment.

`./bin/go` is how CI, scripts and IDE tasks reach a tool, so it must see the ENV an
activated shell would give it -- GOTOOLCHAIN, CARGO_HOME -- whoever calls it. The wrapper
execs `envy product --run`, which installs the provider's closure, activates unless the
caller already is this project's current activation, and runs the tool. People will run
one project's wrapper from inside another project's activation, nest wrappers, tamper
with activated variables and hand over stale state; every one of those is a case here.
"""

from __future__ import annotations

import json
import os
import shutil
import stat
import subprocess
import sys
import unittest
from pathlib import Path

from . import test_config
from .env import EnvyTestCase

WINDOWS = sys.platform == "win32"


def _norm(path) -> str:
    """A path as the OS compares it: case and Windows short names don't count."""
    return os.path.normcase(os.path.realpath(path)) if path else ""

PINNED_VERSION = test_config.get_envy_version()
BINARY_NAME = "envy.exe" if WINDOWS else "envy"

# Prints what the tool saw as JSON. `--call <path> args...` runs another tool and nests
# its JSON under "called"; the inner tool never sees `--call`, so nesting stops there.
_DUMP_PY = """import json, os, shutil, subprocess, sys
args = sys.argv[1:]
marker = args.pop(0)
mine = ("PATH", "ENVY_", "A_", "B_", "SHARED", "DEP_", "OTHER_", "UM_")
env = {k: v for k, v in os.environ.items() if k.upper().startswith(mine)}
out = {"marker": marker, "argv": list(args), "env": env, "which": shutil.which("tool")}
if args[:1] == ["--call"]:
    r = subprocess.run(args[1:], capture_output=True, text=True)
    out["called"] = (json.loads(r.stdout) if r.returncode == 0
                     else {"rc": r.returncode, "stderr": r.stderr})
print(json.dumps(out))
sys.exit(int(os.environ.get("DUMP_EXIT", "0")))
"""


class _ProductRunBase(EnvyTestCase):
    envy_watchdog_timeout = 120

    def setUp(self) -> None:
        super().setUp()
        self.tree = self.make_temp_dir("tree").resolve()
        self.dumper = self.tree / "dump.py"
        self.dumper.write_text(_DUMP_PY, encoding="utf-8")
        self.env = self._launcher_env()

    # -- fixtures -----------------------------------------------------------

    def _launcher_env(self) -> dict[str, str]:
        """Put this build where the shipped launcher looks; return the env to run it in."""
        cached = self.cache_root / "envy" / PINNED_VERSION / BINARY_NAME
        cached.parent.mkdir(parents=True, exist_ok=True)
        if WINDOWS:
            shutil.copy2(self.envy, cached)  # a symlink needs elevation
        else:
            os.symlink(self.envy, cached)
        env = dict(test_config.get_test_env())
        env["ENVY_CACHE_ROOT"] = str(self.cache_root)
        for key in list(env):
            if key.startswith(("ENVY_ACTIVATE_", "ENVY_SHELL_")) or key in (
                "ENVY_PROJECT_ROOT", "ENVY_MIRROR"
            ):
                del env[key]
        return env

    def payload(self, marker: str) -> Path:
        """A dir holding `tool` (POSIX) and `tool.bat` that dump what they saw."""
        d = self.tree / f"payload-{marker}"
        d.mkdir(exist_ok=True)
        py, dump = sys.executable, self.dumper
        sh = d / "tool"
        sh.write_text(f'#!/bin/sh\nexec "{py}" "{dump}" {marker} "$@"\n', encoding="utf-8")
        sh.chmod(sh.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
        (d / "tool.bat").write_text(
            f'@"{py}" "{dump}" {marker} %*\r\n@exit /b %ERRORLEVEL%\r\n', encoding="utf-8"
        )
        return d

    def tool_spec(self, identity: str, marker: str, env_lua: str = "nil",
                  deps: str = "", products: bool = True) -> str:
        """A cache-managed spec whose payload is the dumping tool."""
        src = self.lua_path(self.payload(marker))
        prods = ('PRODUCTS = { tool = "bin/tool" .. '
                 '(envy.PLATFORM == "windows" and ".bat" or "") }\n') if products else ""
        return (
            f'IDENTITY = "{identity}"\n{prods}'
            f"DEPENDENCIES = {{ {deps} }}\n"
            "FETCH = function() end\n"
            f'INSTALL = function(install_dir) envy.copy("{src}", install_dir .. "/bin") end\n'
            f"ENV = {env_lua}\n"
        )

    def project(self, name: str, specs: dict[str, str], packages: list[str]) -> Path:
        root = self.tree / name
        (root / "tools").mkdir(parents=True, exist_ok=True)
        for file_name, content in specs.items():
            (root / file_name).write_text(content, encoding="utf-8")
        (root / "envy.lua").write_text(
            '-- @envy bin "tools"\n'
            '-- @envy deploy "true"\n'
            f'-- @envy version "{PINNED_VERSION}"\n'
            "PACKAGES = {\n" + "".join(f"  {p},\n" for p in packages) + "}\n",
            encoding="utf-8",
        )
        # deploy, not sync: the wrappers without the installs, which tests do themselves
        run = self.run_envy("deploy", "--manifest", root / "envy.lua", env=self.env)
        self.assertEqual(0, run.returncode, run.stderr)
        return root

    def simple(self, name: str, marker: str, env_lua: str) -> Path:
        """A project whose one package provides `tool` with ENV `env_lua`."""
        return self.project(
            name,
            {f"{name}.lua": self.tool_spec(f"local.{name}@r1", marker, env_lua)},
            [f'{{ spec = "local.{name}@r1", source = "{name}.lua" }}'],
        )

    @staticmethod
    def wrapper(proj: Path, name: str = "tool") -> Path:
        return proj / "tools" / (f"{name}.bat" if WINDOWS else name)

    @staticmethod
    def launcher(proj: Path) -> Path:
        return proj / "tools" / ("envy.bat" if WINDOWS else "envy")

    # -- running ------------------------------------------------------------

    def call(self, argv: list, env: dict[str, str] | None = None,
             cwd: Path | None = None) -> subprocess.CompletedProcess:
        return test_config.run(
            [str(a) for a in argv], env=env if env is not None else self.env,
            cwd=cwd or self.tree, capture_output=True, text=True, timeout=90,
        )

    def tool(self, proj: Path, *args, env: dict[str, str] | None = None,
             name: str = "tool") -> dict:
        """Run proj's wrapper; return the JSON the tool printed."""
        r = self.call([self.wrapper(proj, name), *args], env=env)
        self.assertEqual(0, r.returncode, r.stderr)
        try:
            return json.loads(r.stdout)
        except json.JSONDecodeError:
            self.fail(f"tool stdout is not the tool's JSON alone:\n{r.stdout}\n{r.stderr}")

    def activated(self, proj: Path, env: dict[str, str] | None = None) -> dict[str, str]:
        """The environment a shell has after loading proj's activation."""
        # The whole env, as the next call's: assert on it with .get(), never print it.
        dump_env = "import json, os; print(json.dumps(dict(os.environ)))"
        env = env if env is not None else self.env
        if WINDOWS:
            script = self.tree / "dump_env.py"
            script.write_text(dump_env, encoding="utf-8")
            r = subprocess.run(
                f'cmd /d /s /c "call "{self.launcher(proj)}" activate cmd && '
                f'"{sys.executable}" "{script}""',
                env=env, capture_output=True, text=True, timeout=90,
            )
        else:
            r = self.call(
                ["bash", "-c", 'eval "$("$1" activate bash)" && exec "$2" -c "$3"', "_",
                 self.launcher(proj), sys.executable, dump_env],
                env=env,
            )
        self.assertEqual(0, r.returncode, r.stderr)
        out = json.loads(r.stdout)
        self.assertSamePath(proj, out.get("ENVY_ACTIVATE_ROOT"), r.stderr)
        return out

    # -- assertions ---------------------------------------------------------

    def assertSamePath(self, want: Path, got: str | None, msg: str | None = None) -> None:
        self.assertIsNotNone(got, msg)
        self.assertEqual(_norm(want), _norm(got), msg)

    def path_entries(self, seen: dict) -> list[str]:
        return seen["env"].get("PATH", "").split(os.pathsep)

    def assertPathOnce(self, seen: dict, entry: Path) -> None:
        want = _norm(entry)
        hits = [p for p in self.path_entries(seen) if _norm(p) == want]
        self.assertEqual(1, len(hits), f"{entry} in PATH {len(hits)} times")

    def assertPathAbsent(self, seen: dict, fragment: Path) -> None:
        want = _norm(fragment)
        hits = [p for p in self.path_entries(seen) if want in _norm(p)]
        self.assertEqual([], hits, f"{fragment} leaked into PATH")


def _env_a(extra: str = "") -> str:
    return ('function(pkg_path, root) return { A_VAR = "a", SHARED = "from-a", '
            'A_ROOT = root, PATH = { prepend = envy.path.join(pkg_path, "a-prepend") }'
            f"{extra} }} end")


_ENV_B = ('function(pkg_path, root) return { B_VAR = "b", SHARED = "from-b", '
          'PATH = { prepend = envy.path.join(pkg_path, "b-prepend") } } end')


class TestWrapperAppliesEnv(_ProductRunBase):
    """No activation anywhere: the CI step, the IDE task."""

    def test_the_providers_env_reaches_the_tool(self) -> None:
        a = self.simple("a", "A", _env_a())
        seen = self.tool(a)
        self.assertEqual("A", seen["marker"])
        self.assertEqual("a", seen["env"].get("A_VAR"))
        self.assertSamePath(a, seen["env"].get("ENVY_PROJECT_ROOT"))
        self.assertSamePath(a, seen["env"].get("ENVY_ACTIVATE_ROOT"))

    def test_root_is_the_project_root(self) -> None:
        a = self.simple("a", "A", _env_a())
        self.assertPathContains(self.tool(a)["env"].get("A_ROOT", ""), str(a))

    def test_the_first_run_installs_and_still_gets_the_products_own_env(self) -> None:
        """Activation skips the uninstalled; the install has to come first."""
        a = self.simple("a", "A", _env_a())
        self.assertFalse(self.pkg_complete("local.a@r1"))
        self.assertEqual("a", self.tool(a)["env"].get("A_VAR"))
        self.assertTrue(self.pkg_complete("local.a@r1"))

    def test_dependency_env_applies_and_the_nearer_package_wins(self) -> None:
        dep = self.tool_spec("local.dep@r1", "DEP", products=False, env_lua=(
            'function() return { DEP_VAR = "dep", SHARED = "from-dep" } end'))
        b = self.project(
            "b",
            {"dep.lua": dep,
             "b.lua": self.tool_spec("local.b@r1", "B", _ENV_B,
                                     deps='{ spec = "local.dep@r1", source = "dep.lua" }')},
            ['{ spec = "local.b@r1", source = "b.lua" }'],
        )
        env = self.tool(b)["env"]
        self.assertEqual("dep", env.get("DEP_VAR"))
        self.assertEqual("from-b", env.get("SHARED"))

    def test_path_holds_the_bin_dir_and_each_prepend_once(self) -> None:
        a = self.simple("a", "A", _env_a())
        seen = self.tool(a)
        self.assertPathOnce(seen, a / "tools")
        prepends = [p for p in self.path_entries(seen) if p.endswith("a-prepend")]
        self.assertEqual(1, len(prepends), self.path_entries(seen))

    def test_an_uninstalled_sibling_stays_uninstalled_and_contributes_nothing(self) -> None:
        other = self.tool_spec("local.other@r1", "O", products=False,
                               env_lua='{ OTHER_VAR = "other" }')
        a = self.project(
            "a",
            {"a.lua": self.tool_spec("local.a@r1", "A", _env_a()), "other.lua": other},
            ['{ spec = "local.a@r1", source = "a.lua" }',
             '{ spec = "local.other@r1", source = "other.lua" }'],
        )
        env = self.tool(a)["env"]
        self.assertNotIn("OTHER_VAR", env)
        self.assertFalse(self.pkg_complete("local.other@r1"))

    def test_an_installed_sibling_applies_as_in_an_activated_shell(self) -> None:
        other = self.tool_spec("local.other@r1", "O", products=False,
                               env_lua='{ OTHER_VAR = "other" }')
        a = self.project(
            "a",
            {"a.lua": self.tool_spec("local.a@r1", "A", _env_a()), "other.lua": other},
            ['{ spec = "local.a@r1", source = "a.lua" }',
             '{ spec = "local.other@r1", source = "other.lua" }'],
        )
        run = self.run_envy("install", "--manifest", a / "envy.lua", env=self.env)
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertEqual("other", self.tool(a)["env"].get("OTHER_VAR"))

    def test_arguments_and_exit_code_pass_through(self) -> None:
        a = self.simple("a", "A", _env_a())
        env = dict(self.env, DUMP_EXIT="7")
        args = ["a b", "", "--", "--flag", "plain"]
        r = self.call([self.wrapper(a), *args], env=env)
        self.assertEqual(7, r.returncode, r.stderr)
        self.assertEqual(args, json.loads(r.stdout)["argv"])

    def test_nothing_but_the_tool_writes_stdout(self) -> None:
        noisy = self.tool_spec("local.a@r1", "A", _env_a()).replace(
            "FETCH = function() end",
            'envy.stdout("NOISE-AT-LOAD\\n")\n'
            'FETCH = function() envy.stdout("NOISE-IN-FETCH\\n") end',
        )
        a = self.project("a", {"a.lua": noisy},
                         ['{ spec = "local.a@r1", source = "a.lua" }'])
        r = self.call([self.wrapper(a)])
        self.assertEqual(0, r.returncode, r.stderr)
        self.assertEqual("A", json.loads(r.stdout)["marker"])  # stdout is the JSON alone
        self.assertIn("NOISE-AT-LOAD", r.stderr)

    def test_a_failing_providers_env_stops_the_tool(self) -> None:
        a = self.simple("a", "A", 'function() error("boom-in-env") end')
        r = self.call([self.wrapper(a)])
        self.assertNotEqual(0, r.returncode)
        self.assertIn("boom-in-env", r.stderr)
        self.assertNotIn('"marker"', r.stdout)

    def test_a_failing_unrelated_env_warns_and_the_tool_runs(self) -> None:
        bad = self.tool_spec("local.bad@r1", "X", products=False,
                             env_lua='function() error("boom-elsewhere") end')
        a = self.project(
            "a",
            {"a.lua": self.tool_spec("local.a@r1", "A", _env_a()), "bad.lua": bad},
            ['{ spec = "local.a@r1", source = "a.lua" }',
             '{ spec = "local.bad@r1", source = "bad.lua" }'],
        )
        run = self.run_envy("install", "--manifest", a / "envy.lua", env=self.env)
        self.assertEqual(0, run.returncode, run.stderr)
        r = self.call([self.wrapper(a)])
        self.assertEqual(0, r.returncode, r.stderr)
        self.assertEqual("a", json.loads(r.stdout)["env"].get("A_VAR"))
        self.assertIn("boom-elsewhere", r.stderr)

    def test_a_bare_user_managed_command_skips_its_own_wrapper(self) -> None:
        """Value `tool`, product `tool`: PATH has the wrapper first, which must not loop."""
        real = self.payload("REAL")
        spec = ('IDENTITY = "local.um@r1"\nUSER_MANAGED = true\n'
                'PRODUCTS = { tool = "tool" }\n'
                'ENV = { UM_VAR = "um" }\n'
                "SETUP = { main = { CHECK = function() return true end,\n"
                "                   INSTALL = function() end } }\n")
        a = self.project("a", {"um.lua": spec},
                         ['{ spec = "local.um@r1", source = "um.lua" }'])
        env = dict(self.env, PATH=str(real) + os.pathsep + self.env.get("PATH", ""))
        seen = self.tool(a, env=env)
        self.assertEqual("REAL", seen["marker"])
        self.assertEqual("um", seen["env"].get("UM_VAR"))


class TestCallerAlreadyActivated(_ProductRunBase):
    """The caller's environment is an activation: this project's, or someone else's."""

    def setUp(self) -> None:
        super().setUp()
        self.a = self.simple("a", "A", _env_a())
        self.b = self.simple("b", "B", _ENV_B)
        for proj in (self.a, self.b):
            run = self.run_envy("install", "--manifest", proj / "envy.lua", env=self.env)
            self.assertEqual(0, run.returncode, run.stderr)

    def test_this_projects_current_activation_is_used_as_is(self) -> None:
        """Nothing to do or undo: a value the user changed in the shell stands."""
        env = self.activated(self.a)
        env["A_VAR"] = "tampered"
        self.assertEqual("tampered", self.tool(self.a, env=env)["env"].get("A_VAR"))

    def test_an_activated_caller_gets_no_doubled_path(self) -> None:
        seen = self.tool(self.a, env=self.activated(self.a))
        self.assertPathOnce(seen, self.a / "tools")
        prepends = [p for p in self.path_entries(seen) if p.endswith("a-prepend")]
        self.assertEqual(1, len(prepends), self.path_entries(seen))

    def test_another_projects_wrapper_from_this_activation(self) -> None:
        """A is activated; the user runs ../b/tools/tool. B's environment, none of A's."""
        seen = self.tool(self.b, env=self.activated(self.a))
        env = seen["env"]
        self.assertEqual("B", seen["marker"])
        self.assertEqual("b", env.get("B_VAR"))
        self.assertEqual("from-b", env.get("SHARED"))
        self.assertNotIn("A_VAR", env)
        self.assertNotIn("A_ROOT", env)
        self.assertSamePath(self.b, env.get("ENVY_PROJECT_ROOT"))
        self.assertSamePath(self.b, env.get("ENVY_ACTIVATE_ROOT"))
        self.assertPathAbsent(seen, self.a)
        self.assertPathOnce(seen, self.b / "tools")

    def test_a_bare_tool_inside_the_other_project_resolves_to_its_own(self) -> None:
        """Both provide `tool`; inside B's tool, `tool` on PATH is B's, not A's."""
        seen = self.tool(self.b, env=self.activated(self.a))
        self.assertIsNotNone(seen["which"])
        self.assertPathContains(seen["which"], str(self.b / "tools"))

    def test_nested_wrappers_across_projects(self) -> None:
        """B's tool runs A's wrapper: A's environment again, none of B's."""
        seen = self.tool(self.b, "--call", self.wrapper(self.a), env=self.activated(self.a))
        inner = seen["called"]
        self.assertEqual("A", inner.get("marker"), inner)
        self.assertEqual("a", inner["env"].get("A_VAR"))
        self.assertNotIn("B_VAR", inner["env"])
        self.assertSamePath(self.a, inner["env"].get("ENVY_ACTIVATE_ROOT"))
        self.assertPathAbsent(inner, self.b)

    def test_nested_wrappers_in_one_project_share_the_activation(self) -> None:
        seen = self.tool(self.a, "--call", self.wrapper(self.a))
        inner = seen["called"]
        self.assertEqual(seen["env"].get("PATH"), inner["env"].get("PATH"))
        self.assertEqual(seen["env"].get("ENVY_ACTIVATE_GEN"),
                         inner["env"].get("ENVY_ACTIVATE_GEN"))

    def test_a_legacy_activation_without_state_is_overridden(self) -> None:
        """A pre-activate hook left A's bin dir and root behind, and no state."""
        env = dict(self.env, ENVY_PROJECT_ROOT=str(self.a),
                   PATH=str(self.a / "tools") + os.pathsep + self.env.get("PATH", ""))
        seen = self.tool(self.b, env=env)
        self.assertEqual("B", seen["marker"])
        self.assertSamePath(self.b, seen["env"].get("ENVY_PROJECT_ROOT"))

    def test_state_from_another_envy_generation_is_refused(self) -> None:
        env = self.activated(self.a)
        env["ENVY_ACTIVATE_SCHEMA"] = "999"
        r = self.call([self.wrapper(self.b)], env=env)
        self.assertNotEqual(0, r.returncode)
        self.assertIn("restart your shell", r.stderr)


class TestStaleActivation(_ProductRunBase):
    def test_activated_before_the_install_still_gets_the_products_env(self) -> None:
        """The activation skipped the uninstalled tool; the lazy install outdates it."""
        a = self.simple("a", "A", _env_a())
        env = self.activated(a)
        self.assertIsNone(env.get("A_VAR"))
        seen = self.tool(a, env=env)
        self.assertEqual("a", seen["env"].get("A_VAR"))
        self.assertNotEqual(env.get("ENVY_ACTIVATE_GEN"),
                            seen["env"].get("ENVY_ACTIVATE_GEN"))

    def test_a_bumped_generation_reactivates(self) -> None:
        a = self.simple("a", "A", _env_a())
        run = self.run_envy("install", "--manifest", a / "envy.lua", env=self.env)
        self.assertEqual(0, run.returncode, run.stderr)
        env = self.activated(a)
        env["A_VAR"] = "tampered"  # survives only if the activation is reused
        Path(env["ENVY_ACTIVATE_GENFILE"]).write_text("bumped\n", encoding="utf-8")
        self.assertEqual("a", self.tool(a, env=env)["env"].get("A_VAR"))


@unittest.skipUnless(WINDOWS, "cmd.exe and PowerShell")
class TestWindowsShells(_ProductRunBase):
    def setUp(self) -> None:
        super().setUp()
        self.a = self.simple("a", "A", _env_a())
        run = self.run_envy("install", "--manifest", self.a / "envy.lua", env=self.env)
        self.assertEqual(0, run.returncode, run.stderr)

    def test_cmd_activate_and_deactivate_change_the_session(self) -> None:
        launcher = self.launcher(self.a)
        r = subprocess.run(
            f'cmd /d /s /c "call "{launcher}" activate cmd && call echo A=%A_VAR% && '
            f'call "{launcher}" deactivate cmd && call echo A=%A_VAR%"',
            env=self.env, capture_output=True, text=True, timeout=90,
        )
        self.assertEqual(0, r.returncode, r.stderr)
        lines = [ln.strip() for ln in r.stdout.splitlines() if ln.startswith("A=")]
        self.assertEqual(["A=a", "A=%A_VAR%"], lines, r.stdout + r.stderr)

    def test_powershell_runs_the_wrapper(self) -> None:
        env = dict(self.env, DUMP_EXIT="5")
        r = subprocess.run(
            ["powershell", "-NoProfile", "-Command",
             f"& '{self.wrapper(self.a)}' 'a b' plain; exit $LASTEXITCODE"],
            env=env, capture_output=True, text=True, timeout=90,
        )
        self.assertEqual(5, r.returncode, r.stderr)
        seen = json.loads(r.stdout)
        self.assertEqual(["a b", "plain"], seen["argv"])
        self.assertEqual("a", seen["env"].get("A_VAR"))


if __name__ == "__main__":
    unittest.main()
