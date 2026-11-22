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
import select
import shutil
import stat
import subprocess
import sys
import time
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

# Prints what the tool saw as JSON. `--setenv K=V` (repeatable) then `--call <path> args`
# runs another tool with K=V added and nests its JSON under "called"; the inner tool never
# sees either flag, so nesting stops there.
_DUMP_PY = """import json, os, signal, subprocess, sys
args = sys.argv[1:]
marker = args.pop(0)
mine = ("PATH", "ENVY_", "A_", "B_", "SHARED", "DEP_", "OTHER_", "UM_", "HELPER_", "PROBE_")
env = {k: v for k, v in os.environ.items() if k.upper().startswith(mine)}
out = {"marker": marker, "argv": list(args), "env": env, "cwd": os.getcwd(),
       "tty": [os.isatty(fd) for fd in (0, 1, 2)],
       "sigint": "ign" if signal.getsignal(signal.SIGINT) == signal.SIG_IGN else "other"}
if os.environ.get("DUMP_STDIN"):
    out["stdin"] = sys.stdin.read()
extra = {}
while args[:1] == ["--setenv"]:
    k, v = args[1].split("=", 1)
    extra[k] = v
    args = args[2:]
if args[:1] == ["--call"]:
    r = subprocess.run(args[1:], capture_output=True, text=True, env={**os.environ, **extra})
    out["called"] = (json.loads(r.stdout) if r.returncode == 0
                     else {"rc": r.returncode, "stderr": r.stderr})
print(json.dumps(out))
sys.exit(int(os.environ.get("DUMP_EXIT", "0")))
"""

# The POSIX tool first records how its own `yes | head` ends: 141 is SIGPIPE at its
# default, as a shell hands it down; 1 is a SIGPIPE some ancestor left ignored.
_PROBE_SIGPIPE = (
    "PROBE_SIGPIPE=$(bash -c 'yes | head -n1 >/dev/null; echo \"${PIPESTATUS[0]}\"')\n"
    "export PROBE_SIGPIPE\n"
)


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
                "ENVY_PROJECT_ROOT", "ENVY_MIRROR", "ENVY_NO_REEXEC", "ENVY_REEXEC",
                "ENVY_IGNORE_DEPOT", "ENVY_BUNDLES",
            ):
                del env[key]
        return env

    def payload(self, marker: str) -> Path:
        """A dir holding `tool` (POSIX) and `tool.bat` that dump what they saw."""
        d = self.tree / f"payload-{marker}"
        d.mkdir(exist_ok=True)
        py, dump = sys.executable, self.dumper
        sh = d / "tool"
        sh.write_text(f'#!/bin/sh\n{_PROBE_SIGPIPE}exec "{py}" "{dump}" {marker} "$@"\n',
                      encoding="utf-8")
        sh.chmod(sh.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
        (d / "tool.bat").write_text(
            f'@"{py}" "{dump}" {marker} %*\r\n@exit /b %ERRORLEVEL%\r\n', encoding="utf-8"
        )
        return d

    def tool_spec(self, identity: str, marker: str, env_lua: str = "nil",
                  deps: str = "", products: bool = True, install_delay: float = 0) -> str:
        """A cache-managed spec whose payload is the dumping tool."""
        src = self.lua_path(self.payload(marker))
        prods = ('PRODUCTS = { tool = "bin/tool" .. '
                 '(envy.PLATFORM == "windows" and ".bat" or "") }\n') if products else ""
        delay = (f"local t = os.clock() while os.clock() - t < {install_delay} do end "
                 if install_delay else "")
        return (
            f'IDENTITY = "{identity}"\n{prods}'
            f"DEPENDENCIES = {{ {deps} }}\n"
            "FETCH = function() end\n"
            f"INSTALL = function(install_dir) {delay}"
            f'envy.copy("{src}", install_dir .. "/bin") end\n'
            f"ENV = {env_lua}\n"
        )

    @staticmethod
    def user_spec(identity: str, products: str, env: str = "nil") -> str:
        return (f'IDENTITY = "{identity}"\nUSER_MANAGED = true\nPRODUCTS = {products}\n'
                f"ENV = {env}\n"
                "SETUP = { main = { CHECK = function() return true end,\n"
                "                   INSTALL = function() end } }\n")

    def install(self, proj: Path) -> None:
        run = self.run_envy("install", "--manifest", proj / "envy.lua", env=self.env)
        self.assertEqual(0, run.returncode, run.stderr)

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
             cwd: Path | None = None, **kw) -> subprocess.CompletedProcess:
        return test_config.run(
            [str(a) for a in argv], env=env if env is not None else self.env,
            cwd=cwd or self.tree, capture_output=True, text=True, timeout=90, **kw,
        )

    def tool(self, proj: Path, *args, env: dict[str, str] | None = None,
             name: str = "tool", **kw) -> dict:
        """Run proj's wrapper; return the JSON the tool printed."""
        r = self.call([self.wrapper(proj, name), *args], env=env, **kw)
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
    return ('function(pkg_dir, pkg_state_dir) return { A_VAR = "a", SHARED = "from-a", '
            'A_STATE = pkg_state_dir, '
            'PATH = { prepend = envy.path.join(pkg_dir, "a-prepend") }'
            f"{extra} }} end")


_ENV_B = ('function(pkg_dir) return { B_VAR = "b", SHARED = "from-b", '
          'PATH = { prepend = envy.path.join(pkg_dir, "b-prepend") } } end')


class TestWrapperAppliesEnv(_ProductRunBase):
    """No activation anywhere: the CI step, the IDE task."""

    def test_the_providers_env_reaches_the_tool(self) -> None:
        a = self.simple("a", "A", _env_a())
        seen = self.tool(a)
        self.assertEqual("A", seen["marker"])
        self.assertEqual("a", seen["env"].get("A_VAR"))
        self.assertSamePath(a, seen["env"].get("ENVY_PROJECT_ROOT"))
        self.assertSamePath(a, seen["env"].get("ENVY_ACTIVATE_ROOT"))

    def test_pkg_state_dir_is_under_the_project_root(self) -> None:
        a = self.simple("a", "A", _env_a())
        pkg_state_dir = self.tool(a)["env"].get("A_STATE")
        self.assertSamePath(a / ".envy" / "state" / "local.a", pkg_state_dir)

    def test_the_first_run_installs_and_still_gets_the_products_own_env(self) -> None:
        """Activation skips the uninstalled; the install has to come first."""
        a = self.simple("a", "A", _env_a())
        self.assertFalse(self.pkg_complete("local.a@r1"))
        self.assertEqual("a", self.tool(a)["env"].get("A_VAR"))
        self.assertTrue(self.pkg_complete("local.a@r1"))

    def test_dependency_env_applies_and_the_nearer_package_wins(self) -> None:
        # The dependency sorts first by key, so key order alone would pick it: only
        # nearness makes b's SHARED win.
        dep = self.tool_spec("local.aaa-dep@r1", "DEP", products=False, env_lua=(
            'function() return { DEP_VAR = "dep", SHARED = "from-dep" } end'))
        b = self.project(
            "b",
            {"dep.lua": dep,
             "b.lua": self.tool_spec(
                 "local.b@r1", "B", _ENV_B,
                 deps='{ spec = "local.aaa-dep@r1", source = "dep.lua" }')},
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
        # Windows: `!` through envy.bat's delayed expansion, & and ^ through two cmd hops.
        args = ["a b", "", "--", "--flag", "plain", "wow!", "x & y", "a ^ b"]
        if not WINDOWS:
            args += ["$HOME", "*", "it's"]
        r = self.call([self.wrapper(a), *args], env=env)
        self.assertEqual(7, r.returncode, r.stderr)
        self.assertEqual(args, json.loads(r.stdout)["argv"])

    def test_the_tool_keeps_the_callers_cwd_and_stdin(self) -> None:
        a = self.simple("a", "A", _env_a())
        elsewhere = self.make_temp_dir("cwd").resolve()
        seen = self.tool(a, env=dict(self.env, DUMP_STDIN="1"), cwd=elsewhere,
                         input="piped-in")
        self.assertSamePath(elsewhere, seen["cwd"])
        self.assertEqual("piped-in", seen["stdin"])

    def test_the_tool_sees_what_an_activated_shell_sees(self) -> None:
        """Two code paths build the activation; they must not drift apart."""
        dep = self.tool_spec("local.aaa-dep@r1", "DEP", products=False, env_lua=(
            'function() return { DEP_VAR = "dep", SHARED = "from-dep", '
            'PATH = { append = "/dep-append" } } end'))
        other = self.tool_spec("local.other@r1", "O", products=False,
                               env_lua='{ OTHER_VAR = "other", HELPER_GONE = false }')
        a = self.project(
            "a",
            {"dep.lua": dep, "other.lua": other,
             "a.lua": self.tool_spec(
                 "local.a@r1", "A", _env_a(),
                 deps='{ spec = "local.aaa-dep@r1", source = "dep.lua" }')},
            ['{ spec = "local.a@r1", source = "a.lua" }',
             '{ spec = "local.other@r1", source = "other.lua" }'],
        )
        self.install(a)
        base = dict(self.env, HELPER_GONE="set-before")
        shell = self.activated(a, env=base)
        tool = self.tool(a, env=base)["env"]
        for name in ("A_VAR", "A_STATE", "SHARED", "DEP_VAR", "OTHER_VAR", "HELPER_GONE",
                     "PATH", "ENVY_PROJECT_ROOT", "ENVY_ACTIVATE_PKGS"):
            self.assertEqual(shell.get(name), tool.get(name), name)

    def test_a_relative_user_managed_path_anchors_at_the_project_root(self) -> None:
        """CWD is never load-bearing: `scripts/tool` is the project's, from anywhere."""
        a = self.project(
            "a",
            {"um.lua": self.user_spec(
                "local.um@r1",
                '{ tool = "scripts/tool" .. (envy.PLATFORM == "windows" and ".bat" or "") }')},
            ['{ spec = "local.um@r1", source = "um.lua" }'],
        )
        shutil.copytree(self.payload("REL"), a / "scripts")
        seen = self.tool(a, cwd=self.make_temp_dir("elsewhere"))
        self.assertEqual("REL", seen["marker"])

    def test_an_absolute_user_managed_executable_gets_empty_arguments_intact(self) -> None:
        """An .exe on Windows: quoted by the CRT's rules, an empty argument included."""
        a = self.project(
            "a",
            {"um.lua": self.user_spec(
                "local.um@r1", f'{{ py = "{self.lua_path(Path(sys.executable))}" }}')},
            ['{ spec = "local.um@r1", source = "um.lua" }'],
        )
        seen = self.tool(a, self.dumper, "PY", "a b", "", "c", name="py")
        self.assertEqual("PY", seen["marker"])
        self.assertEqual(["a b", "", "c"], seen["argv"])

    def test_a_user_managed_alias_reaches_a_sibling_wrapper(self) -> None:
        """Value `tool` from product `alias`: the bin dir is skipped only for itself."""
        a = self.project(
            "a",
            {"a.lua": self.tool_spec("local.a@r1", "A", _env_a()),
             "um.lua": self.user_spec("local.um@r1", '{ alias = "tool" }')},
            ['{ spec = "local.a@r1", source = "a.lua" }',
             '{ spec = "local.um@r1", source = "um.lua" }'],
        )
        seen = self.tool(a, name="alias")
        self.assertEqual("A", seen["marker"])
        self.assertEqual("a", seen["env"].get("A_VAR"))

    def test_only_host_packages_resolve(self) -> None:
        """A root for another platform must not satisfy a weak reference, as in install."""
        other_os = "linux" if sys.platform == "darwin" else "darwin"
        fallback = self.user_spec("local.fallback@r1", '{ helper = "x" }',
                                  env='{ HELPER_FROM = "fallback" }')
        nonhost = self.user_spec("local.nonhost@r1", '{ helper = "y" }',
                                 env='{ HELPER_FROM = "nonhost" }')
        a = self.project(
            "a",
            {"fallback.lua": fallback, "nonhost.lua": nonhost,
             "a.lua": self.tool_spec(
                 "local.a@r1", "A", _env_a(),
                 deps='{ product = "helper", weak = { spec = "local.fallback@r1", '
                      'source = "fallback.lua" } }')},
            ['{ spec = "local.a@r1", source = "a.lua" }',
             f'{{ spec = "local.nonhost@r1", source = "nonhost.lua", '
             f'platforms = {{ "{other_os}" }} }}'],
        )
        self.assertEqual("fallback", self.tool(a)["env"].get("HELPER_FROM"))

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
        a = self.project(
            "a", {"um.lua": self.user_spec("local.um@r1", '{ tool = "tool" }',
                                           env='{ UM_VAR = "um" }')},
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
        self.assertNotIn("A_STATE", env)
        self.assertSamePath(self.b, env.get("ENVY_PROJECT_ROOT"))
        self.assertSamePath(self.b, env.get("ENVY_ACTIVATE_ROOT"))
        self.assertPathAbsent(seen, self.a)
        self.assertPathOnce(seen, self.b / "tools")

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
        """make -> ./tools/cc: the outer tool's activation is reused, edits and all."""
        seen = self.tool(self.a, "--setenv", "A_VAR=tampered", "--call", self.wrapper(self.a))
        inner = seen["called"]
        self.assertEqual("tampered", inner["env"].get("A_VAR"), inner)
        self.assertEqual(seen["env"].get("PATH"), inner["env"].get("PATH"))

    def test_a_legacy_activation_without_state_is_overridden(self) -> None:
        """A pre-activate hook left A's bin dir and root behind, and no state."""
        env = dict(self.env, ENVY_PROJECT_ROOT=str(self.a),
                   PATH=str(self.a / "tools") + os.pathsep + self.env.get("PATH", ""))
        seen = self.tool(self.b, env=env)
        self.assertEqual("B", seen["marker"])
        self.assertSamePath(self.b, seen["env"].get("ENVY_PROJECT_ROOT"))

    def test_state_from_another_envy_version_warns_and_runs(self) -> None:
        """Nested wrappers of projects pinning different envys: no shell to restart."""
        env = self.activated(self.a)
        env["ENVY_ACTIVATE_SCHEMA"] = "999"
        r = self.call([self.wrapper(self.b)], env=env)
        self.assertEqual(0, r.returncode, r.stderr)
        self.assertEqual("b", json.loads(r.stdout)["env"].get("B_VAR"))
        self.assertIn("another envy", r.stderr)

    def test_envy_run_activate_reuses_a_current_activation(self) -> None:
        run = [self.launcher(self.a), "run", "--activate", sys.executable, self.dumper, "RUN"]
        env = self.activated(self.a)
        env["A_VAR"] = "tampered"
        r = self.call(run, env=env)
        self.assertEqual(0, r.returncode, r.stderr)
        self.assertEqual("tampered", json.loads(r.stdout)["env"].get("A_VAR"))
        fresh = self.call(run)
        self.assertEqual(0, fresh.returncode, fresh.stderr)
        self.assertEqual("a", json.loads(fresh.stdout)["env"].get("A_VAR"))


class TestStaleActivation(_ProductRunBase):
    """The caller's activation of this project predates something the tool needs."""

    def test_activated_before_the_install_gains_the_products_env_and_keeps_edits(self):
        """Only the missing ENV is layered on: a Makefile's PATH edit survives."""
        a = self.simple("a", "A", _env_a())
        env = self.activated(a)
        self.assertIsNone(env.get("A_VAR"))
        custom = str(self.make_temp_dir("custom"))
        env["PATH"] = custom + os.pathsep + env["PATH"]
        env["SHARED"] = "edited"  # a variable activation never touched for an uninstalled a
        seen = self.tool(a, env=env)
        self.assertEqual("a", seen["env"].get("A_VAR"))
        self.assertPathOnce(seen, Path(custom))
        self.assertEqual("from-a", seen["env"].get("SHARED"))  # the tool's own ENV wins

    def test_another_project_installing_the_package_still_gets_its_env(self) -> None:
        """A shared cache: B's install leaves A's generation alone, but not A's tool."""
        spec = self.tool_spec("local.shared@r1", "S", _env_a())
        src = self.tree / "shared.lua"
        src.write_text(spec, encoding="utf-8")
        entry = f'{{ spec = "local.shared@r1", source = "{self.lua_path(src)}" }}'
        a = self.project("a", {}, [entry])
        b = self.project("b", {}, [entry])
        env = self.activated(a)
        self.assertIsNone(env.get("A_VAR"))
        self.install(b)
        self.assertEqual("a", self.tool(a, env=env)["env"].get("A_VAR"))

    def test_a_bumped_generation_alone_keeps_the_callers_env(self) -> None:
        """The tool's closure is all there: nothing to redo, the caller's edits stand."""
        a = self.simple("a", "A", _env_a())
        self.install(a)
        env = self.activated(a)
        env["A_VAR"] = "tampered"
        Path(env["ENVY_ACTIVATE_GENFILE"]).write_text("bumped\n", encoding="utf-8")
        self.assertEqual("tampered", self.tool(a, env=env)["env"].get("A_VAR"))

    def test_a_failing_env_is_fatal_from_an_activated_shell_too(self) -> None:
        """activate skipped the broken ENV with a warning; the tool must not run without it."""
        a = self.simple("a", "A", 'function() error("boom-in-env") end')
        self.install(a)
        env = self.activated(a)
        r = self.call([self.wrapper(a)], env=env)
        self.assertNotEqual(0, r.returncode)
        self.assertIn("boom-in-env", r.stderr)


@unittest.skipIf(WINDOWS, "POSIX signals, fds and terminals")
class TestProcessHandoff(_ProductRunBase):
    """What the tool inherits from envy beyond its environment."""

    def test_sigpipe_is_back_at_its_default(self) -> None:
        """envy ignores SIGPIPE for its sockets; `./tools/x | head` must still stop x."""
        seen = self.tool(self.simple("a", "A", _env_a()))
        self.assertEqual("141", seen["env"].get("PROBE_SIGPIPE"))

    def test_an_ignored_sigint_stays_ignored(self) -> None:
        """`nohup`/background jobs: what the caller ignored, the tool ignores."""
        a = self.simple("a", "A", _env_a())
        r = self.call(["bash", "-c", 'trap "" INT; exec "$0"', self.wrapper(a)])
        self.assertEqual(0, r.returncode, r.stderr)
        self.assertEqual("ign", json.loads(r.stdout)["sigint"])

    def test_a_closed_stdout_stays_closed(self) -> None:
        """Not quietly swapped for stderr: the tool's write fails as it would directly."""
        a = self.simple("a", "A", _env_a())
        r = self.call(["bash", "-c", 'exec "$0" >&-', self.wrapper(a)])
        self.assertNotIn('"marker"', r.stderr)

    def test_a_terminal_gets_its_cursor_back_and_the_outcome_row(self) -> None:
        """A lazy install paints progress; the tool must start on a clean, visible terminal."""
        import pty

        a = self.project(
            "a", {"a.lua": self.tool_spec("local.a@r1", "A", _env_a(), install_delay=0.6)},
            ['{ spec = "local.a@r1", source = "a.lua" }'])
        primary, secondary = pty.openpty()
        proc = test_config.popen(
            [str(self.wrapper(a))], cwd=str(self.tree),
            env={**self.env, "TERM": "xterm-256color"},
            stdin=subprocess.DEVNULL, stdout=secondary, stderr=secondary,
        )
        os.close(secondary)
        chunks: list[bytes] = []
        deadline = time.monotonic() + 60
        try:
            while time.monotonic() < deadline:
                if not select.select([primary], [], [], 1)[0]:
                    if proc.poll() is not None:
                        break
                    continue
                try:
                    data = os.read(primary, 65536)
                except OSError:
                    break
                if not data:
                    break
                chunks.append(data)
        finally:
            proc.wait(timeout=30)
            os.close(primary)
        out = b"".join(chunks)
        before, _, tool_json = out.partition(b'{"marker"')
        self.assertTrue(tool_json, out[-500:])
        self.assertIn(b"\x1b[?25l", before, "the install never painted progress")
        self.assertGreater(before.rfind(b"\x1b[?25h"), before.rfind(b"\x1b[?25l"),
                           "the tool started with the cursor hidden")
        self.assertIn(b"installed", before)
        self.assertEqual([False, True, True], json.loads(b'{"marker"' + tool_json)["tty"])

    def test_activate_cmd_is_refused_off_windows(self) -> None:
        a = self.simple("a", "A", _env_a())
        for verb in ("activate", "deactivate"):
            r = self.call([self.launcher(a), verb, "cmd"])
            self.assertNotEqual(0, r.returncode)
            self.assertIn("only on Windows", r.stderr)


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

    def test_a_failed_cmd_activation_fails_the_launcher(self) -> None:
        """No `exit /b 0` from envy means no success: `activate cmd || exit` must fire."""
        env = dict(self.env, ENVY_ACTIVATE_VARS="X", ENVY_ACTIVATE_SCHEMA="999")
        r = subprocess.run(f'cmd /d /s /c "call "{self.launcher(self.a)}" activate cmd"',
                           env=env, capture_output=True, text=True, timeout=90)
        self.assertNotEqual(0, r.returncode, r.stdout + r.stderr)

    def test_a_trailing_backslash_survives_the_batch_hop(self) -> None:
        """tool.bat hands %* to a native program, which reads `\\"` as an escaped quote."""
        args = ["C:\\dir with space\\", "next", 'say \\"hi\\"']
        r = self.call([self.wrapper(self.a), *args])
        self.assertEqual(0, r.returncode, r.stderr)
        self.assertEqual(args, json.loads(r.stdout)["argv"])

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


@unittest.skipIf(WINDOWS, "the bash launcher and product script")
class TestWrapperExecsOnlyEnvy(_ProductRunBase):
    """With envy cached, a wrapper starts only bash, envy and the tool.

    Wrapper and launcher run on every tool call; with only bash on PATH, any other program
    fails.
    """

    def bash_only(self, env: dict[str, str]) -> dict[str, str]:
        d = self.tree / "bash-only"
        d.mkdir(exist_ok=True)
        os.symlink(shutil.which("bash", path=env.get("PATH")), d / "bash")
        return dict(env, PATH=str(d))

    def test_a_pinned_project_under_cache_root(self) -> None:
        proj = self.simple("pinned", "pinned", "nil")
        self.install(proj)
        seen = self.tool(proj, "arg", env=self.bash_only(self.env))
        self.assertEqual(["arg"], seen["argv"])

    def test_an_unpinned_project_in_the_user_wide_cache(self) -> None:
        # No ENVY_CACHE_ROOT and no pin: the launcher finds the platform's cache dir and
        # reads `latest`, which names a version other than the launcher's own fallback.
        env = test_config.sandbox_home_env(self.tree / "home", self.env)
        root = test_config.sandbox_user_wide_root(env)
        env["OSTYPE"] = "linux-gnu" if sys.platform == "darwin" else "darwin"  # misleads bash
        latest = "999.0.0"
        (root / "envy" / latest).mkdir(parents=True)
        os.symlink(self.envy, root / "envy" / latest / BINARY_NAME)
        (root / "envy" / "latest").write_text(latest + "\n", encoding="utf-8")
        proj = self.simple("unpinned", "unpinned", "nil")
        manifest = proj / "envy.lua"
        pin = f'-- @envy version "{PINNED_VERSION}"\n'
        self.assertIn(pin, manifest.read_text(encoding="utf-8"))
        manifest.write_text(manifest.read_text(encoding="utf-8").replace(pin, ""),
                            encoding="utf-8")
        seen = self.tool(proj, "arg", env=self.bash_only(env))  # product --run installs it
        self.assertEqual(["arg"], seen["argv"])


if __name__ == "__main__":
    unittest.main()
