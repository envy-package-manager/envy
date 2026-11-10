"""`envy activate` / `envy deactivate` and the shell hooks that drive them.

Every project here is made by `envy init`, so its bin dir holds a real launcher: the hook
runs `<bin>/envy activate <shell>` on entry and `envy deactivate` on the way out, and the
environment after leaving must be byte-for-byte what it was before entering.
"""

from __future__ import annotations

import os
import shutil
import sys
import unittest
from pathlib import Path

from . import test_config
from .env import EnvyTestCase

_RUST = """IDENTITY = "local.rust@r0"
FETCH = function() end
INSTALL = function(install_dir) envy.run("mkdir -p bin", { cwd = install_dir }) end
ENV = function(pkg_path, root, options)
  return {
    RUSTUP_HOME = envy.path.join(pkg_path, "rustup"),
    CARGO_HOME = envy.path.join(root, ".cargo"),
    PATH = { prepend = envy.path.join(pkg_path, "bin") },
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
    use_production_binary = True
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
        self.assertIn(f"CARGO_HOME={self.rust.resolve()}/.cargo", result.stdout)
        self.assertIn("TRICKY=it's $HOME \"q\" \\ ;\nsecond line", result.stdout)
        self.assertIn("PYTHONHOME=unset", result.stdout)
        self.assertIn(str((self.rust / "tools").resolve()), result.stdout)
        self.assertIn("envy: entering rusty", result.stderr)
        self.assertIn("envy: leaving rusty", result.stderr)
        self.assertEqual(_env_dump(before.read_bytes()), _env_dump(after.read_bytes()))

    def test_uninstalled_packages_hint_then_install_refreshes(self) -> None:
        result = self.bash(
            f'source "{self.hook}"\n'
            f'cd "{self.rust}"; _envy_hook\n'
            'echo "BEFORE=${CARGO_HOME-unset}"\n'
            f'"{self.rust}/tools/envy" install >/dev/null 2>&1\n'
            "_envy_hook\n"  # the next prompt: no cd, the generation file changed
            'echo "AFTER=${CARGO_HOME-unset}"\n'
        )
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertIn("run 'envy install'", result.stderr)
        self.assertIn("BEFORE=unset", result.stdout)
        self.assertIn(f"AFTER={self.rust.resolve()}/.cargo", result.stdout)
        self.assertIn("envy: refreshed rusty", result.stderr)

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
        self.assertIn(f"FIRST={self.rust.resolve()}/.cargo", result.stdout)
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
        self.assertIn("run 'envy install'", result.stderr)
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
        self.assertEqual(f"{self.rust.resolve()}/.cargo", activated.stdout.strip())

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
        launcher = self.rust / "tools" / "envy"  # rust is not installed: activation warns
        result = test_config.run(
            [str(launcher), "run", "--activate", "true"],
            capture_output=True, text=True, env=self.env, timeout=60,
        )
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertIn("run 'envy install'", result.stderr)

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
        self.assertIn(f"CARGO_HOME={rust.resolve()}/.cargo", result.stdout)
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


if __name__ == "__main__":
    unittest.main()
