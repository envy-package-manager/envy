"""`envy activate` / `envy deactivate` and the shell hooks that drive them.

Every project here is made by `envy init`, so its bin dir holds a real launcher: the hook
runs `<bin>/envy activate <shell>` on entry and `envy deactivate` on the way out, and the
environment after leaving must be byte-for-byte what it was before entering.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import unittest
from pathlib import Path

from . import test_config
from .env import EnvyTestCase

_RUST = """IDENTITY = "local.rust@r0"
FETCH = function() end
INSTALL = function(install_dir) envy.run("mkdir -p bin", { cwd = install_dir }) end
ENV = function(pkg_dir, pkg_state_dir, options)
  return {
    RUSTUP_HOME = envy.path.join(pkg_dir, "rustup"),
    CARGO_HOME = pkg_state_dir,
    PATH = { prepend = envy.path.join(pkg_dir, "bin") },
    TRICKY = "it's $HOME \\"q\\" \\\\ ;\\nsecond line",
    PYTHONHOME = false,
  }
end
"""

# Volatile per-shell variables a cd or a subshell changes on its own.
_IGNORED = {"PWD", "OLDPWD", "_", "SHLVL"}


def _env_dump(raw: bytes) -> dict[str, str]:
    out = {}
    for entry in raw.split(b"\0"):
        if b"=" in entry:
            k, v = entry.split(b"=", 1)
            if k.decode() not in _IGNORED:
                out[k.decode()] = v.decode(errors="replace")
    return out


@unittest.skipIf(sys.platform == "win32", "POSIX shell hooks")
class _ActivateBase(EnvyTestCase):
    envy_watchdog_timeout = 60

    def setUp(self) -> None:
        super().setUp()
        self.root = self.make_temp_dir("activate")
        self.cache = self.root / "cache"
        self.env = {
            k: v
            for k, v in test_config.get_test_env().items()
            if not k.startswith(("ENVY_SHELL_", "ENVY_ACTIVATE_"))
        }
        self.env["ENVY_CACHE_ROOT"] = str(self.cache)
        self.env["PYTHONHOME"] = "/original/pythonhome"  # ENV unsets it while active

    def project(self, name: str, specs: dict[str, str], packages: list[str]) -> Path:
        """`envy init` a project, then give it local specs and PACKAGES entries."""
        proj = self.root / name
        result = test_config.run(
            [str(self.envy), "init", str(proj), str(proj / "tools")],
            capture_output=True,
            text=True,
            env=self.env,
            timeout=30,
        )
        self.assertEqual(0, result.returncode, result.stderr)
        manifest = proj / "envy.lua"
        header = manifest.read_text().split("PACKAGES", 1)[0]
        for file_name, content in specs.items():
            (proj / file_name).write_text(content)
        manifest.write_text(header + "PACKAGES = {\n" + "\n".join(packages) + "\n}\n")
        return proj

    def install(self, proj: Path) -> None:
        result = test_config.run(
            [str(proj / "tools" / "envy"), "install"],
            capture_output=True,
            text=True,
            env=self.env,
            timeout=60,
        )
        self.assertEqual(0, result.returncode, result.stderr)

    def shell(self, argv: list[str], script: str):
        return test_config.run(
            [*argv, script],
            capture_output=True,
            text=True,
            env=self.env,
            timeout=60,
            cwd=self.root,
        )


class TestActivateBash(_ActivateBase):
    def setUp(self) -> None:
        super().setUp()
        self.rust = self.project(
            "rusty",
            {"rust.lua": _RUST},
            ['  { spec = "local.rust@r0", source = "rust.lua" },'],
        )
        self.hook = self.cache / "shell" / "hook.bash"

    def bash(self, script: str):
        return self.shell(["bash", "--noprofile", "--norc", "-e", "-c"], script)

    def test_enter_applies_env_and_leave_restores_exactly(self) -> None:
        self.install(self.rust)
        before, after = self.root / "before", self.root / "after"
        result = self.bash(
            f'source "{self.hook}"\n'
            f'env -0 > "{before}"\n'
            f'cd "{self.rust}"; _envy_hook\n'
            'echo "CARGO_HOME=$CARGO_HOME"; echo "TRICKY=$TRICKY"\n'
            'echo "PYTHONHOME=${PYTHONHOME-unset}"; echo "PATH=$PATH"\n'
            f'cd "{self.root}"; _envy_hook\n'
            f'env -0 > "{after}"\n'
        )
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertIn(f"CARGO_HOME={self.rust.resolve()}/.envy/state/local.rust/", result.stdout)
        self.assertIn("TRICKY=it's $HOME \"q\" \\ ;\nsecond line", result.stdout)
        self.assertIn("PYTHONHOME=unset", result.stdout)
        self.assertIn(str((self.rust / "tools").resolve()), result.stdout)
        self.assertIn("envy: entering rusty", result.stderr)
        self.assertIn("envy: leaving rusty", result.stderr)
        self.assertEqual(_env_dump(before.read_bytes()), _env_dump(after.read_bytes()))

    def test_uninstalled_packages_are_silent_then_install_refreshes(self) -> None:
        result = self.bash(
            f'source "{self.hook}"\n'
            f'cd "{self.rust}"; _envy_hook\n'
            'echo "BEFORE=${CARGO_HOME-unset}"\n'
            f'"{self.rust}/tools/envy" install >/dev/null 2>&1\n'
            "_envy_hook\n"  # the next prompt: no cd, the generation file changed
            'echo "AFTER=${CARGO_HOME-unset}"\n'
        )
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertNotIn("warning", result.stderr)  # shims install lazily: nothing to nag
        self.assertIn("BEFORE=unset", result.stdout)
        self.assertIn(f"AFTER={self.rust.resolve()}/.envy/state/local.rust/", result.stdout)
        self.assertIn("envy: refreshed rusty", result.stderr)

    def test_lazy_install_refreshes_and_cache_hit_does_not(self) -> None:
        lazy = self.project(
            "lazy",
            {"z.lua": 'IDENTITY = "local.z@r0"\nFETCH = function() end\n'
             'INSTALL = function() end\nPRODUCTS = { zed = "zed" }\nENV = { LAZY = "1" }\n'},
            ['  { spec = "local.z@r0", source = "z.lua" },'],
        )
        shim = f'"{lazy}/tools/envy" product zed >/dev/null\n_envy_hook\n'  # a product shim
        result = self.bash(
            f'source "{self.hook}"\n'
            f'cd "{lazy}"; _envy_hook\n'
            f'{shim}echo "LAZY=${{LAZY-unset}}"\n'
            f'{shim}'  # installs nothing: no refresh
        )
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertIn("LAZY=1", result.stdout)
        self.assertEqual(1, result.stderr.count("envy: refreshed lazy"), result.stderr)

    def test_switching_projects_leaves_nothing_of_the_first(self) -> None:
        other = self.project(
            "other",
            {"o.lua": 'IDENTITY = "local.o@r0"\nFETCH = function() end\n'
             'INSTALL = function() end\nENV = { ONLY_OTHER = "o" }\n'},
            ['  { spec = "local.o@r0", source = "o.lua" },'],
        )
        self.install(self.rust)
        self.install(other)
        result = self.bash(
            f'source "{self.hook}"\n'
            f'cd "{self.rust}"; _envy_hook\n'
            f'cd "{other}"; _envy_hook\n'
            'echo "CARGO_HOME=${CARGO_HOME-unset} ONLY_OTHER=${ONLY_OTHER-unset}"\n'
            'echo "ROOT=$ENVY_ACTIVATE_ROOT"; echo "PATH=$PATH"\n'
        )
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertIn("CARGO_HOME=unset ONLY_OTHER=o", result.stdout)
        self.assertIn(f"ROOT={other.resolve()}", result.stdout)
        self.assertNotIn(str((self.rust / "tools").resolve()), result.stdout)

    def test_nested_shell_inherits_without_reapplying(self) -> None:
        self.install(self.rust)
        tools = str((self.rust / "tools").resolve())
        result = self.bash(
            f'source "{self.hook}"\n'
            f'cd "{self.rust}"; _envy_hook\n'
            f"bash --noprofile --norc -c 'source \"{self.hook}\"; echo \"NESTED=$PATH\"'\n"
        )
        self.assertEqual(0, result.returncode, result.stderr)
        nested = next(l for l in result.stdout.splitlines() if l.startswith("NESTED="))
        self.assertEqual(1, nested.removeprefix("NESTED=").split(os.pathsep).count(tools))

    def test_manual_activation_switches_and_deactivates(self) -> None:
        other = self.project("other", {}, [])
        self.install(self.rust)
        before, after = self.root / "before", self.root / "after"
        result = self.bash(
            f'env -0 > "{before}"\n'
            f'eval "$("{self.rust}/tools/envy" activate bash)"\n'
            'echo "FIRST=${CARGO_HOME-unset}"\n'
            f'eval "$("{other}/tools/envy" activate bash)"\n'
            'echo "SECOND=${CARGO_HOME-unset} ROOT=$ENVY_ACTIVATE_ROOT"\n'
            f'eval "$("{other}/tools/envy" deactivate bash)"\n'
            f'env -0 > "{after}"\n'
        )
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertIn(f"FIRST={self.rust.resolve()}/.envy/state/local.rust/", result.stdout)
        self.assertIn(f"SECOND=unset ROOT={other.resolve()}", result.stdout)
        self.assertEqual(_env_dump(before.read_bytes()), _env_dump(after.read_bytes()))

    def test_lua_stdout_never_reaches_the_eval(self) -> None:
        noisy = self.project(
            "noisy",
            {"n.lua": 'IDENTITY = "local.n@r0"\nFETCH = function() end\nINSTALL = function() end\n'
             'ENV = function() envy.stdout("echo CORRUPTED"); io.write("echo CORRUPTED2\\n")\n'
             '  return { NOISY = "ok" } end\n'},
            ['  { spec = "local.n@r0", source = "n.lua" },'],
        )
        self.install(noisy)
        result = self.bash(
            f'eval "$("{noisy}/tools/envy" activate bash)"\n' 'echo "NOISY=$NOISY"\n'
        )
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertIn("NOISY=ok", result.stdout)
        self.assertNotIn("CORRUPTED", result.stdout)

    def test_uncached_remote_spec_is_never_fetched(self) -> None:
        served = self.make_temp_dir("served")
        (served / "r.lua").write_text('IDENTITY = "remote.r@r0"\nFETCH = function() end\n')
        requests: list[str] = []
        url = self.serve_directory(served, requests)
        remote = self.project(
            "remote", {}, [f'  {{ spec = "remote.r@r0", source = "{url}/r.lua" }},']
        )
        result = self.bash(
            f'eval "$("{remote}/tools/envy" activate bash)"\n' 'echo "PATH=$PATH"\n'
        )
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertEqual([], requests)
        self.assertNotIn("warning", result.stderr)
        self.assertIn(str((remote / "tools").resolve()), result.stdout)

    def test_failing_launcher_falls_back_to_bin_on_path(self) -> None:
        proj = self.root / "legacy"
        (proj / "tools").mkdir(parents=True)
        (proj / "envy.lua").write_text('-- @envy bin "tools"\nPACKAGES = {}\n')
        launcher = proj / "tools" / "envy"
        launcher.write_text("#!/bin/sh\nexit 1\n")  # an envy that predates `activate`
        launcher.chmod(0o755)
        result = self.bash(
            f'source "{self.hook}"\n'
            f'cd "{proj}"; _envy_hook\n'
            'echo "IN=$PATH"\n'
            f'cd "{self.root}"; _envy_hook\n'
            'echo "OUT=$PATH"\n'
        )
        self.assertEqual(0, result.returncode, result.stderr)
        tools = str((proj / "tools").resolve())
        lines = dict(l.split("=", 1) for l in result.stdout.splitlines() if "=" in l)

        def entries(raw: str) -> list[str]:  # bash keeps the logical /var spelling
            return [str(Path(e).resolve()) for e in raw.split(os.pathsep) if e]

        self.assertIn(tools, entries(lines["IN"]))
        self.assertNotIn(tools, entries(lines["OUT"]))
        self.assertIn("tools added to PATH", result.stderr)

    def test_run_activate_exposes_env_plain_run_does_not(self) -> None:
        self.install(self.rust)
        launcher = self.rust / "tools" / "envy"
        activated = test_config.run(
            [str(launcher), "run", "--activate", "printenv", "CARGO_HOME"],
            capture_output=True, text=True, env=self.env, timeout=60,
        )
        self.assertEqual(0, activated.returncode, activated.stderr)
        self.assertEqual(f"{self.rust.resolve()}/.envy/state/local.rust/", activated.stdout.strip())

        plain = test_config.run(
            [str(launcher), "run", "printenv", "CARGO_HOME"],
            capture_output=True, text=True, env=self.env, timeout=60,
        )
        self.assertNotEqual(0, plain.returncode)

    def test_uncached_root_keeps_installed_roots_env(self) -> None:
        local = self.project(
            "mixed",
            {"l.lua": 'IDENTITY = "local.l@r0"\nFETCH = function() end\n'
             'INSTALL = function() end\nENV = { LOCAL_OK = "1" }\n'},
            ['  { spec = "local.l@r0", source = "l.lua" },'],
        )
        self.install(local)
        served = self.make_temp_dir("served")
        (served / "r.lua").write_text('IDENTITY = "remote.r@r0"\nFETCH = function() end\n')
        requests: list[str] = []
        url = self.serve_directory(served, requests)
        manifest = local / "envy.lua"
        manifest.write_text(manifest.read_text().replace(
            "PACKAGES = {\n", f'PACKAGES = {{\n  {{ spec = "remote.r@r0", source = "{url}/r.lua" }},\n'))
        result = self.bash(
            f'eval "$("{local}/tools/envy" activate bash)"\n' 'echo "LOCAL_OK=${LOCAL_OK-unset}"\n'
        )
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertIn("LOCAL_OK=1", result.stdout)
        self.assertEqual([], requests)

    def activate_stderr(self, proj: Path) -> list[str]:
        result = test_config.run(
            [str(proj / "tools" / "envy"), "activate", "bash"],
            capture_output=True, text=True, env=self.env, timeout=60,
        )
        self.assertEqual(0, result.returncode, result.stderr)
        return result.stderr.splitlines()

    def tools_bundle(self) -> str:
        """An uncached bundle providing products cmake and ninja; returns its decl."""
        bundle = self.root / "bundle"
        (bundle / "specs").mkdir(parents=True)
        (bundle / "envy-bundle.lua").write_text(
            'BUNDLE = "test.tools@v1"\nSPECS = {\n'
            '  ["envy.cmake@r0"] = "specs/cmake.lua",\n'
            '  ["envy.ninja@r0"] = "specs/ninja.lua",\n}\n'
        )
        for name in ("cmake", "ninja"):
            (bundle / "specs" / f"{name}.lua").write_text(
                f'IDENTITY = "envy.{name}@r0"\nFETCH = function() end\n'
                f'INSTALL = function() end\nPRODUCTS = {{ {name} = "{name}" }}\n'
            )
        return f'bundle = {{ identity = "test.tools@v1", source = "{bundle}" }}'

    def test_cold_cache_product_dependency_is_silent(self) -> None:
        bundle = self.tools_bundle()
        codec = self.project(
            "codec",
            {"brotli.lua": 'IDENTITY = "local.brotli@r0"\n'
             'DEPENDENCIES = { { product = "cmake" }, { product = "ninja" } }\n'
             'FETCH = function() end\nINSTALL = function() end\nENV = { BROTLI = "1" }\n'},
            ['  { spec = "local.brotli@r0", source = "brotli.lua" },',
             f'  {{ spec = "envy.cmake@r0", {bundle} }},',
             f'  {{ spec = "envy.ninja@r0", {bundle} }},'],
        )
        lines = self.activate_stderr(codec)
        self.assertEqual(1, len(lines), lines)
        self.assertRegex(lines[0], r"^envy: entering codec .+ 2 variables set$")

    def test_cached_product_chain_resolves_despite_uncached_sibling(self) -> None:
        # brotli -> ninja -> cmake by product, listed so brotli retries before ninja resolves.
        def spec(name: str, deps: str, rest: str) -> str:
            return (f'IDENTITY = "local.{name}@r0"\nDEPENDENCIES = {{ {deps} }}\n'
                    f'FETCH = function() end\nINSTALL = function() end\n{rest}\n')

        chain = self.project(
            "chain",
            {"brotli.lua": spec("brotli", '{ product = "ninja" }', 'ENV = { BROTLI = "1" }'),
             "ninja.lua": spec("ninja", '{ product = "cmake" }',
                               'PRODUCTS = { ninja = "ninja" }\nENV = { NINJA = "1" }'),
             "cmake.lua": spec("cmake", "", 'PRODUCTS = { cmake = "cmake" }')},
            [f'  {{ spec = "local.{n}@r0", source = "{n}.lua" }},'
             for n in ("brotli", "ninja", "cmake")],
        )
        self.install(chain)
        served = self.make_temp_dir("served")
        (served / "r.lua").write_text('IDENTITY = "remote.r@r0"\nFETCH = function() end\n')
        url = self.serve_directory(served)
        manifest = chain / "envy.lua"
        manifest.write_text(manifest.read_text().replace(
            "PACKAGES = {\n", f'PACKAGES = {{\n  {{ spec = "remote.r@r0", source = "{url}/r.lua" }},\n'))
        result = self.bash(
            f'eval "$("{chain}/tools/envy" activate bash)"\n'
            'echo "BROTLI=${BROTLI-unset} NINJA=${NINJA-unset}"\n'
        )
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertIn("BROTLI=1 NINJA=1", result.stdout)
        self.assertNotIn("warning", result.stderr)

    def test_product_nobody_provides_still_warns(self) -> None:
        orphan = self.project(
            "orphan",
            {"brotli.lua": 'IDENTITY = "local.brotli@r0"\n'
             'DEPENDENCIES = { { product = "cmake" } }\n'
             'FETCH = function() end\nINSTALL = function() end\n'},
            ['  { spec = "local.brotli@r0", source = "brotli.lua" },'],
        )
        lines = self.activate_stderr(orphan)  # the engine's error runs on for a line
        self.assertEqual("warning: activate: skipping local.brotli@r0: Product 'cmake' in "
                         "spec 'local.brotli@r0' was not found", lines[0])
        self.assertEqual(1, sum(l.startswith("warning:") for l in lines), lines)
        self.assertRegex(lines[-1], r"^envy: entering orphan ")

    def test_install_during_activation_still_refreshes(self) -> None:
        # The ENV function plays an install finishing mid-activation: it bumps the
        # generation file. The recorded generation must predate that, so the next prompt
        # refreshes.
        racy = self.project(
            "racy",
            {"r.lua": 'IDENTITY = "local.r@r0"\nFETCH = function() end\nINSTALL = function() end\n'
             'ENV = function()\n'
             '  local f = os.getenv("TEST_BUMP")\n'
             '  if f then local h = io.open(f, "w"); h:write("bumped\\n"); h:close() end\n'
             '  return { RACY = "1" } end\n'},
            ['  { spec = "local.r@r0", source = "r.lua" },'],
        )
        self.install(racy)
        probe = self.bash(f'"{racy}/tools/envy" activate bash')
        genfile = next(
            l.split("=", 1)[1].strip("'")
            for l in probe.stdout.splitlines()
            if l.startswith("export ENVY_ACTIVATE_GENFILE=")
        )
        self.env["TEST_BUMP"] = genfile
        result = self.bash(
            f'eval "$("{racy}/tools/envy" activate bash)"\n' 'echo "GEN=$ENVY_ACTIVATE_GEN"\n'
        )
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertNotIn("GEN=bumped", result.stdout)

    def test_env_names_that_match_hook_locals_reach_the_shell(self) -> None:
        names = ["out", "gen", "refresh", "manifest_dir", "bin_val", "bin_dir"]
        shadow = self.project(
            "shadow",
            {"s.lua": 'IDENTITY = "local.s@r0"\nFETCH = function() end\nINSTALL = function() end\n'
             "ENV = { " + ", ".join(f'{n} = "v_{n}"' for n in names) + " }\n"},
            ['  { spec = "local.s@r0", source = "s.lua" },'],
        )
        self.install(shadow)
        result = self.bash(
            f'source "{self.hook}"\n'
            f'cd "{shadow}"; _envy_hook\n'
            + "".join(f'echo "{n}=${{{n}-unset}}"\n' for n in names)
        )
        self.assertEqual(0, result.returncode, result.stderr)
        for n in names:
            self.assertIn(f"{n}=v_{n}", result.stdout)

    def test_run_activate_warnings_are_not_lost(self) -> None:
        broken = self.project(
            "broken",
            {"b.lua": 'IDENTITY = "local.b@r0"\nFETCH = function() end\nINSTALL = function() end\n'
             'ENV = function() error("boom") end\n'},
            ['  { spec = "local.b@r0", source = "b.lua" },'],
        )
        self.install(broken)
        result = test_config.run(
            [str(broken / "tools" / "envy"), "run", "--activate", "true"],
            capture_output=True, text=True, env=self.env, timeout=60,
        )
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertIn("activate: skipping ENV of local.b@r0", result.stderr)

    def test_unexported_announce_switch_is_honored(self) -> None:
        self.install(self.rust)
        result = self.bash(
            "ENVY_SHELL_NO_ENTER_EXIT_ANNOUNCE=1\n"  # a plain shell variable, not exported
            f'source "{self.hook}"\n'
            f'cd "{self.rust}"; _envy_hook\n'
            f'cd "{self.root}"; _envy_hook\n'
        )
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertNotIn("envy: entering", result.stderr)
        self.assertNotIn("envy: leaving", result.stderr)


@unittest.skipIf(shutil.which("zsh") is None, "zsh not installed")
class TestActivateZsh(_ActivateBase):
    def test_enter_applies_env_and_leave_restores_exactly(self) -> None:
        rust = self.project(
            "rusty", {"rust.lua": _RUST}, ['  { spec = "local.rust@r0", source = "rust.lua" },']
        )
        self.install(rust)
        hook = self.cache / "shell" / "hook.zsh"
        before, after = self.root / "before", self.root / "after"
        result = self.shell(
            ["zsh", "-f", "-c"],
            f'source "{hook}"\n'
            f'env -0 > "{before}"\n'
            f'cd "{rust}"\n'  # chpwd fires the hook
            'echo "CARGO_HOME=$CARGO_HOME"\n'
            f'cd "{self.root}"\n'
            f'env -0 > "{after}"\n',
        )
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertIn(f"CARGO_HOME={rust.resolve()}/.envy/state/local.rust/", result.stdout)
        self.assertEqual(_env_dump(before.read_bytes()), _env_dump(after.read_bytes()))

    def test_env_names_that_match_hook_locals_reach_the_shell(self) -> None:
        names = ["out", "gen", "manifest_dir", "bin_val", "bin_dir"]
        shadow = self.project(
            "shadow",
            {"s.lua": 'IDENTITY = "local.s@r0"\nFETCH = function() end\nINSTALL = function() end\n'
             "ENV = { " + ", ".join(f'{n} = "v_{n}"' for n in names) + " }\n"},
            ['  { spec = "local.s@r0", source = "s.lua" },'],
        )
        self.install(shadow)
        hook = self.cache / "shell" / "hook.zsh"
        result = self.shell(
            ["zsh", "-f", "-c"],
            f'source "{hook}"\n' f'cd "{shadow}"\n'
            + "".join(f'echo "{n}=${{{n}-unset}}"\n' for n in names),
        )
        self.assertEqual(0, result.returncode, result.stderr)
        for n in names:
            self.assertIn(f"{n}=v_{n}", result.stdout)


@unittest.skipIf(shutil.which("pwsh") is None, "pwsh not installed")
class TestActivatePowerShell(_ActivateBase):
    """Non-ASCII values survive the round trip through a non-UTF-8 console code page."""

    @unittest.skipIf(sys.platform == "win32", "project fixtures use the POSIX launcher")
    def test_non_ascii_values_round_trip(self) -> None:
        proj = self.project(
            "unicode",
            {"u.lua": 'IDENTITY = "local.u@r0"\nFETCH = function() end\nINSTALL = function() end\n'
             'ENV = { WHO = "Jos\u00e9" }\n'},
            ['  { spec = "local.u@r0", source = "u.lua" },'],
        )
        self.install(proj)
        hook = self.cache / "shell" / "hook.ps1"
        result = test_config.run_pwsh(
            ["pwsh", "-NoProfile", "-Command",
             "[Console]::OutputEncoding = [System.Text.Encoding]::GetEncoding(28591)\n"
             f'. "{hook}"\n'
             f'Set-Location "{proj}"; _envy_hook\n'
             '[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()\n'
             'Write-Output "WHO=$env:WHO"\n'],
            capture_output=True, text=True, env=self.env, timeout=60, encoding="utf-8",
        )
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertIn("WHO=Jos\u00e9", result.stdout)



_RT_VALUE = "a \"b\" $c %d% !e! & f; g's \\h"  # every shell's quoting, at once
_DUMP_TO = "import json, os, sys; json.dump(dict(os.environ), open(sys.argv[1], 'w'))"


class TestActivateRoundTrip(EnvyTestCase):
    """Every dialect: nothing before, everything during, exactly the original after.

    One package ENV exercises each op -- a set full of shell metacharacters, an unset,
    a PATH prepend and append -- and the env is dumped before activate, after it, and
    after deactivate, from inside the shell that evaluated the script.
    """

    envy_watchdog_timeout = 120

    def setUp(self) -> None:
        super().setUp()
        self.root = self.make_temp_dir("rt").resolve()
        self.env = {
            k: v
            for k, v in test_config.get_test_env().items()
            if not k.startswith(("ENVY_SHELL_", "ENVY_ACTIVATE_", "ENVY_PROJECT_ROOT"))
        }
        self.env["ENVY_CACHE_ROOT"] = str(self.root / "cache")
        self.env["UNSET_ME"] = "orig"
        self.env["ENVY_SHELL_NO_ENTER_EXIT_ANNOUNCE"] = "1"
        self.append_dir = str(self.root / "append-dir")
        self.proj = self.root / "proj"
        r = test_config.run([str(self.envy), "init", str(self.proj), str(self.proj / "tools")],
                            capture_output=True, text=True, env=self.env, timeout=60)
        self.assertEqual(0, r.returncode, r.stderr)
        manifest = self.proj / "envy.lua"
        header = manifest.read_text().split("PACKAGES", 1)[0]
        (self.proj / "rt.lua").write_text(
            'IDENTITY = "local.rt@r0"\nFETCH = function() end\nINSTALL = function() end\n'
            "ENV = function(pkg_dir) return {\n"
            f"  SET_ME = [[{_RT_VALUE}]],\n"
            "  UNSET_ME = false,\n"
            '  PATH = { prepend = envy.path.join(pkg_dir, "pre"), '
            f"append = [[{self.append_dir}]] }},\n"
            "} end\n"
        )
        manifest.write_text(header + 'PACKAGES = {\n  { spec = "local.rt@r0", '
                            'source = "rt.lua" },\n}\n')
        r = test_config.run([str(self.launcher), "install"], capture_output=True, text=True,
                            env=self.env, timeout=120)
        self.assertEqual(0, r.returncode, r.stderr)
        self.dumper = self.root / "dump.py"
        self.dumper.write_text(_DUMP_TO)

    @property
    def launcher(self) -> Path:
        return self.proj / "tools" / ("envy.bat" if sys.platform == "win32" else "envy")

    def dumps(self) -> tuple[Path, Path, Path]:
        return self.root / "before.json", self.root / "during.json", self.root / "after.json"

    def check(self, result) -> None:
        self.assertEqual(0, result.returncode, result.stderr)
        before, during, after = (json.loads(p.read_text()) for p in self.dumps())
        sep = os.pathsep

        self.assertNotIn("SET_ME", before)
        self.assertEqual("orig", before.get("UNSET_ME"))

        self.assertEqual(_RT_VALUE, during.get("SET_ME"))
        self.assertNotIn("UNSET_ME", during)
        path = during.get("PATH", during.get("Path", "")).split(sep)
        self.assertEqual(os.path.normcase(str((self.proj / "tools").resolve())),
                         os.path.normcase(os.path.realpath(path[0])))
        self.assertTrue(path[1].rstrip("\\/").endswith("pre"), path[:3])
        self.assertEqual(self.append_dir, path[-1])
        self.assertEqual(os.path.normcase(str(self.proj)),
                         os.path.normcase(during.get("ENVY_PROJECT_ROOT", "")))
        self.assertIn("ENVY_ACTIVATE_ROOT", during)

        volatile = {"PWD", "OLDPWD", "_", "SHLVL"}
        strip = lambda e: {k: v for k, v in e.items() if k not in volatile}  # noqa: E731
        self.assertEqual(strip(before), strip(after), "deactivate left a difference")

    def posix(self, argv: list[str], script: str):
        b, d, a = self.dumps()
        py, dump, L = sys.executable, self.dumper, self.launcher
        body = script.format(before=f"'{py}' '{dump}' '{b}'", during=f"'{py}' '{dump}' '{d}'",
                             after=f"'{py}' '{dump}' '{a}'", L=f"'{L}'")
        return test_config.run([*argv, body], capture_output=True, text=True,
                               env=self.env, timeout=120, cwd=self.root)

    @unittest.skipIf(sys.platform == "win32", "envy activate bash is POSIX-only")
    def test_bash(self) -> None:
        self.check(self.posix(["bash", "--noprofile", "--norc", "-e", "-c"],
                              '{before}\neval "$({L} activate bash)"\n{during}\n'
                              'eval "$({L} deactivate bash)"\n{after}\n'))

    @unittest.skipIf(sys.platform == "win32" or not shutil.which("zsh"), "zsh")
    def test_zsh(self) -> None:
        self.check(self.posix(["zsh", "-f", "-e", "-c"],
                              '{before}\neval "$({L} activate zsh)"\n{during}\n'
                              'eval "$({L} deactivate zsh)"\n{after}\n'))

    @unittest.skipIf(sys.platform == "win32" or not shutil.which("fish"), "fish")
    def test_fish(self) -> None:
        self.check(self.posix(["fish", "--no-config", "-c"],
                              "{before}\n{L} activate fish | source\n{during}\n"
                              "{L} deactivate fish | source\n{after}\n"))

    def powershell(self, exe: str):
        b, d, a = self.dumps()
        py, dump, L = sys.executable, self.dumper, self.launcher
        script = (f"& '{py}' '{dump}' '{b}'\n"
                  f"& '{L}' activate powershell | Out-String | Invoke-Expression\n"
                  f"& '{py}' '{dump}' '{d}'\n"
                  f"& '{L}' deactivate powershell | Out-String | Invoke-Expression\n"
                  f"& '{py}' '{dump}' '{a}'\n")
        return test_config.run_pwsh([exe, "-NoProfile", "-NonInteractive", "-Command", script],
                                    capture_output=True, text=True, env=self.env, timeout=120,
                                    cwd=self.root)

    @unittest.skipIf(not shutil.which("pwsh"), "pwsh not installed")
    def test_pwsh(self) -> None:
        self.check(self.powershell("pwsh"))

    @unittest.skipUnless(sys.platform == "win32", "Windows PowerShell")
    def test_windows_powershell(self) -> None:
        self.check(self.powershell("powershell"))

    @unittest.skipUnless(sys.platform == "win32", "cmd.exe")
    def test_cmd(self) -> None:
        b, d, a = self.dumps()
        py, dump, L = sys.executable, self.dumper, self.launcher
        line = (f'"{py}" "{dump}" "{b}" && call "{L}" activate cmd && '
                f'"{py}" "{dump}" "{d}" && call "{L}" deactivate cmd && "{py}" "{dump}" "{a}"')
        self.check(subprocess.run(f'cmd /d /s /c "{line}"', capture_output=True, text=True,
                                  env=self.env, timeout=120, cwd=self.root))

if __name__ == "__main__":
    unittest.main()
