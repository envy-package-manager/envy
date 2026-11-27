"""`{{name}}` in phase shell text, expanded by envy before any shell runs.

Specs are written raw (no str.format) so braces read as envy sees them.
"""

from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path

from . import test_config
from .env import EnvyTestCase

PY = Path(sys.executable)
WINDOWS = sys.platform == "win32"
TIMEOUT = 60  # a held-Lua-lock regression hangs; this turns it into a failure

# Writes its arguments after the first, as JSON, to the file the first names.
_REC_PY = "import json, sys\nwith open(sys.argv[1], 'w') as f:\n    json.dump(sys.argv[2:], f)\n"


class TestPhaseProducts(EnvyTestCase):
    def setUp(self) -> None:
        super().setUp()
        self.rec_py = self.write_spec("rec.py", _REC_PY)
        self.py_spec = self.write_spec(
            "py.lua",
            'IDENTITY = "local.py@v1"\nUSER_MANAGED = true\n'
            f'PRODUCTS = {{ py = "{PY.as_posix()}" }}\n'
            "SETUP = { main = { CHECK = function() return true end,\n"
            "                   INSTALL = function() end } }\n",
        )
        # local.tool@v1 installs lib/rec.py; `rec` runs it through `py`.
        self.tool_spec = self.write_spec(
            "tool.lua",
            'IDENTITY = "local.tool@v1"\n'
            'DEPENDENCIES = { { product = "py" } }\n'
            "FETCH = function() end\n"
            "INSTALL = function(install_dir)\n"
            f'  envy.copy("{self.lua_path(self.rec_py)}", install_dir .. "lib/rec.py")\n'
            "end\n"
            "PRODUCTS = {\n"
            '  ["rec.py"] = { value = "lib/rec.py", script = false },\n'
            '  rec = { command = { "{{py}}", "{{rec.py}}" } },\n'
            "}\n",
        )

    # -- fixtures -----------------------------------------------------------

    def consumer(self, body: str, deps: str = '{ product = "rec" }') -> Path:
        """local.c@v1 with `body` as its verbs and a no-op FETCH."""
        return self.write_spec(
            "c.lua",
            'IDENTITY = "local.c@v1"\n'
            f"DEPENDENCIES = {{ {deps} }}\n"
            "FETCH = function() end\n" + body,
        )

    def manifest(self, *specs: tuple[str, Path], setup: list[str] | None = None,
                 header: str = "") -> Path:
        entries = [("local.py@v1", self.py_spec), ("local.tool@v1", self.tool_spec)]
        lines = [test_config.spec_entry(*e) for e in entries]
        for identity, path in specs:
            lines.append(test_config.spec_entry(identity, path, setup=setup))
        m = test_config.write_spec_manifest(self.work, lines)
        if header:
            m.write_text(m.read_text(encoding="utf-8") + header, encoding="utf-8")
        return m

    def install_ok(self, manifest: Path):
        r = self.install(manifest, timeout=TIMEOUT)
        self.assertEqual(r.returncode, 0, f"stdout:\n{r.stdout}\nstderr:\n{r.stderr}")
        return r

    def fails(self, manifest: Path, *needles: str) -> str:
        r = self.install(manifest, timeout=TIMEOUT)
        self.assertNotEqual(r.returncode, 0, r.stderr)
        for needle in needles:
            self.assertIn(needle, r.stderr)
        return r.stderr

    def pkg_dir(self, manifest: Path, identity: str) -> Path:
        r = self.run_envy("package", identity, "--manifest", manifest, timeout=TIMEOUT)
        self.assertEqual(r.returncode, 0, r.stderr)
        return Path(r.stdout.strip())

    def read(self, manifest: Path, name: str, identity: str = "local.c@v1"):
        return json.loads((self.pkg_dir(manifest, identity) / name).read_text("utf-8"))

    def recorded(self, manifest: Path, name: str = "out.json") -> list[str]:
        """Installs, then reads rec.py's output from local.c@v1's payload."""
        self.install_ok(manifest)
        return self.read(manifest, name)

    def rec_path(self, manifest: Path) -> str:
        return str(self.pkg_dir(manifest, "local.tool@v1") / "lib" / "rec.py")

    def marker(self, name: str) -> tuple[Path, str]:
        """A marker path, and Lua that writes it."""
        path = self.work / name
        return path, f'envy.json.write("{self.lua_path(path)}", {{ 1 }})'

    # -- expansion ----------------------------------------------------------

    def test_string_build_runs_a_command_product(self):
        c = self.consumer('BUILD = "{{rec}} out.json a b"\n')
        m = self.manifest(("local.c@v1", c))
        r = self.install_ok(m)
        self.assertEqual(["a", "b"], self.read(m, "out.json"))
        access = [e.raw for e in r.events("lua_ctx_product_access", "local.c@v1")]
        self.assertEqual(1, len(access), access)
        self.assertTrue(access[0]["allowed"], access)
        self.assertEqual("rec", access[0]["target"])
        self.assertIn("rec.py", access[0]["reason"])  # the rendered line

    def test_string_stage_expands(self):
        c = self.consumer(
            'FETCH = function(tmp_dir)\n'
            '  envy.json.write(tmp_dir .. "seed.json", { 1 })\n'
            '  envy.commit_fetch("seed.json")\n'
            "end\n"
            'STAGE = "{{rec}} out.json s"\n',
            deps='{ product = "rec", needed_by = "stage" }',
        )
        self.assertEqual(["s"], self.recorded(self.manifest(("local.c@v1", c))))

    def test_function_result_and_envy_run_expand(self):
        c = self.consumer(
            "BUILD = function()\n"
            '  envy.run("{{rec}} run.json r")\n'
            '  return "{{rec}} out.json ret"\n'
            "end\n"
        )
        m = self.manifest(("local.c@v1", c))
        self.assertEqual(["ret"], self.recorded(m))
        self.assertEqual(["r"], self.read(m, "run.json"))

    @unittest.skipIf(" " in str(PY), "the user-managed {{py}} is inserted as written")
    def test_envy_run_keeps_env_values_and_obeys_its_shell(self):
        c = self.consumer(
            "BUILD = function()\n"
            "  envy.run([==[{{py}} -c \"import os,json;"
            "json.dump([os.environ['X']],open('env.json','w'))\"]==],\n"
            '           { env = { X = "{{rec}}" } })\n'
            "  envy.run(\"open('shell.json', 'w').write('[\\\"{{{{x}}}}\\\"]')\",\n"
            f'           {{ shell = {{ file = {{ "{PY.as_posix()}" }}, ext = ".py" }} }})\n'
            "end\n",
            deps='{ product = "py" }',
        )
        m = self.manifest(("local.c@v1", c))
        self.install_ok(m)
        self.assertEqual(["{{rec}}"], self.read(m, "env.json"))
        self.assertEqual(["{{x}}"], self.read(m, "shell.json"))

    @unittest.skipIf(" " in str(PY), "the user-managed {{py}} is inserted as written")
    def test_path_product_is_its_text_in_quotes_and_words(self):
        c = self.consumer(
            'BUILD = [[{{py}} "{{rec.py}}" out.json "{{rec.py}}" -f={{rec.py}}]]\n',
            deps='{ product = "py" }, { product = "rec.py" }',
        )
        m = self.manifest(("local.c@v1", c))
        out = self.recorded(m)
        rec = self.rec_path(m)
        self.assertEqual([rec, f"-f={rec}"], out)

    @unittest.skipIf(WINDOWS, "a POSIX heredoc")
    def test_path_product_in_a_heredoc(self):
        c = self.consumer(
            'BUILD = "cat > out.json <<EOF\\n[\\"{{rec.py}}\\"]\\nEOF\\n"\n',
            deps='{ product = "rec.py" }',
        )
        m = self.manifest(("local.c@v1", c))
        self.assertEqual([self.rec_path(m)], self.recorded(m))

    def test_escape_emits_braces(self):
        c = self.consumer(
            "BUILD = [[{{rec}} out.json '{{{{x}}}}' '{{ {{a}} }}' "
            "'{{{{range .}}{{.ID}}{{end}}}}']]\n"
        )
        self.assertEqual(
            ["{{x}}", "{{a}}", "{{range .}}{{.ID}}{{end}}"],
            self.recorded(self.manifest(("local.c@v1", c))),
        )

    def test_setup_strings_expand(self):
        check, install = self.work / "check.json", self.work / "install.json"
        c = self.write_spec(
            "c.lua",
            'IDENTITY = "local.c@v1"\nUSER_MANAGED = true\n'
            'DEPENDENCIES = { { product = "rec" } }\n'
            "SETUP = {\n"
            f'  seen = {{ CHECK = "{{{{rec}}}} {self.lua_path(check)} c",\n'
            "           INSTALL = function() end },\n"
            "  run = { CHECK = function() return false end,\n"
            f'          INSTALL = "{{{{rec}}}} {self.lua_path(install)} i" }},\n'
            "}\n",
        )
        self.install_ok(self.manifest(("local.c@v1", c), setup=["seen", "run"]))
        self.assertEqual(["c"], json.loads(check.read_text(encoding="utf-8")))
        self.assertEqual(["i"], json.loads(install.read_text(encoding="utf-8")))

    def test_a_custom_shell_expands_paths_and_escapes(self):
        """Custom shells get paths and escapes expanded."""
        c = self.consumer(
            "BUILD = [[import json; "
            "json.dump([r'{{rec.py}}', '{{{{x}}}}'], open('out.json', 'w'))]]\n",
            deps='{ product = "rec.py" }')
        header = f'DEFAULT_SHELL = {{ file = {{ "{PY.as_posix()}" }}, ext = ".py" }}\n'
        m = self.manifest(("local.c@v1", c), header=header)
        out = self.recorded(m)
        self.assertEqual([self.rec_path(m), "{{x}}"], out)

    def test_a_custom_shell_refuses_a_command_product(self):
        c = self.consumer('BUILD = "{{rec}} out.json"\n')
        header = f'DEFAULT_SHELL = {{ file = {{ "{PY.as_posix()}" }}, ext = ".py" }}\n'
        self.fails(self.manifest(("local.c@v1", c), header=header),
                   "'{{rec}}' is a command product, and a custom shell has no quoting")

    def test_function_default_shell_still_checks_names_ahead(self):
        """Names are checked before fetch even when the shell is a function."""
        fetched, write = self.marker("fetched.json")
        c = self.consumer(f"FETCH = function() {write} end\n"
                          'BUILD = "print({{nope}})"\n')
        header = ("DEFAULT_SHELL = function()\n"
                  f'  return {{ file = {{ "{PY.as_posix()}" }}, ext = ".py" }}\nend\n')
        self.fails(self.manifest(("local.c@v1", c), header=header),
                   "local.c@v1 BUILD: pkg 'local.c@v1' does not declare product dependency "
                   "on 'nope'")
        self.assertFalse(fetched.exists())

    # -- own products -------------------------------------------------------

    def test_own_command_function_from_every_held_lock_site(self):
        """Own command functions run on the held Lua state; the phase context survives."""
        c = self.write_spec(
            "c.lua",
            'IDENTITY = "local.c@v1"\n'
            'DEPENDENCIES = { { product = "py" } }\n'
            "FETCH = function() end\n"
            "BUILD = function(install_dir)\n"
            f'  envy.copy("{self.lua_path(self.rec_py)}", install_dir .. "lib/rec.py")\n'
            '  envy.run("{{myrec}} {{a}} run")\n'
            '  envy.json.write(install_dir .. "pc.json", { envy.product("a") })\n'
            '  return "{{myrec}} {{b}} ret"\n'
            "end\n"
            "PRODUCTS = {\n"
            '  a = { value = "a.json", script = false },\n'
            '  b = { value = "b.json", script = false },\n'
            "  myrec = { command = function(pkg_dir)\n"
            '    return { "{{py}}", pkg_dir .. "lib/rec.py" }\n'
            "  end },\n"
            "}\n",
        )
        m = self.manifest(("local.c@v1", c))
        self.install_ok(m)
        self.assertEqual(["run"], self.read(m, "a.json"))
        self.assertEqual(["ret"], self.read(m, "b.json"))
        (a,) = self.read(m, "pc.json")
        self.assertEqual(str(self.pkg_dir(m, "local.c@v1") / "a.json"), str(Path(a)))

    def test_own_command_function_and_path_before_install(self):
        c = self.write_spec(
            "c.lua",
            'IDENTITY = "local.c@v1"\n'
            'DEPENDENCIES = { { product = "py" } }\n'
            "FETCH = function() end\n"
            "BUILD = function(install_dir)\n"
            f'  envy.copy("{self.lua_path(self.rec_py)}", install_dir .. "lib/rec.py")\n'
            "end\n"
            'INSTALL = "{{self_rec}} {{out}} k"\n'
            "PRODUCTS = {\n"
            '  out = { value = "out.json", script = false },\n'
            "  self_rec = { command = function(pkg_dir)\n"
            '    return { "{{py}}", pkg_dir .. "lib/rec.py" }\n'
            "  end },\n"
            "}\n",
        )
        self.assertEqual(["k"], self.recorded(self.manifest(("local.c@v1", c))))

    def test_own_command_splice_respects_needed_by_before_fetch(self):
        fetched, write = self.marker("fetched.json")
        c = self.write_spec(
            "c.lua",
            'IDENTITY = "local.c@v1"\n'
            'DEPENDENCIES = { { product = "py", needed_by = "install" } }\n'
            f"FETCH = function() {write} end\n"
            'BUILD = "{{self_py}} -c 1"\n'
            'PRODUCTS = { self_py = { command = { "{{py}}" } } }\n',
        )
        self.fails(self.manifest(("local.c@v1", c)),
                   "local.c@v1 BUILD: '{{self_py}}': product 'py' needed_by 'install'")
        self.assertFalse(fetched.exists(), "checked before anything was fetched")

    def test_envy_product_reads_own_products(self):
        c = self.write_spec(
            "c.lua",
            'IDENTITY = "local.c@v1"\n'
            'DEPENDENCIES = { { product = "py" } }\n'
            "FETCH = function() end\n"
            "INSTALL = function(install_dir)\n"
            '  envy.json.write(install_dir .. "out.json", { envy.product("own") })\n'
            "end\n"
            'PRODUCTS = { own = { value = "own.txt", script = false } }\n',
        )
        m = self.manifest(("local.c@v1", c))
        (own,) = self.recorded(m)
        self.assertEqual(str(self.pkg_dir(m, "local.c@v1") / "own.txt"), str(Path(own)))

    def test_product_dependency_on_an_own_product_is_refused(self):
        c = self.write_spec(
            "c.lua",
            'IDENTITY = "local.c@v1"\n'
            'DEPENDENCIES = { { product = "mine" } }\n'
            "FETCH = function() end\n"
            'PRODUCTS = { mine = { value = "mine", script = false } }\n',
        )
        self.fails(self.manifest(("local.c@v1", c)),
                   "spec 'local.c@v1' provides product 'mine' itself")

    # -- checked ahead ------------------------------------------------------

    def test_string_verbs_are_checked_before_fetch(self):
        for needed_by, build, needle in (
            ("install", '"{{rec}} out.json"', "needed_by 'install'"),
            ("build", '"X={{rec}} true"', "fills whole words"),
            ("build", '"{{rec out.json"', "unterminated '{{'"),
        ):
            with self.subTest(build=build):
                fetched, write = self.marker(f"fetched-{needed_by}-{len(build)}.json")
                c = self.write_spec(
                    "c.lua",
                    'IDENTITY = "local.c@v1"\n'
                    f'DEPENDENCIES = {{ {{ product = "rec", needed_by = "{needed_by}" }} }}\n'
                    f"FETCH = function() {write} end\nBUILD = {build}\n",
                )
                self.fails(self.manifest(("local.c@v1", c)), "local.c@v1 BUILD", needle)
                self.assertFalse(fetched.exists())

    def test_selected_setup_strings_are_checked_before_any_pair_runs(self):
        checked, write = self.marker("checked.json")
        c = self.write_spec(
            "c.lua",
            'IDENTITY = "local.c@v1"\nUSER_MANAGED = true\n'
            "SETUP = { main = {\n"
            f"  CHECK = function() {write} return false end,\n"
            '  INSTALL = "{{nope}} x" } }\n',
        )
        self.fails(self.manifest(("local.c@v1", c), setup=["main"]),
                   "local.c@v1 SETUP.main.INSTALL: pkg 'local.c@v1' does not declare "
                   "product dependency on 'nope'")
        self.assertFalse(checked.exists())

    def test_unselected_and_off_platform_pairs_go_unchecked(self):
        c = self.write_spec(
            "c.lua",
            'IDENTITY = "local.c@v1"\nUSER_MANAGED = true\n'
            "SETUP = {\n"
            "  main = { CHECK = function() return true end, INSTALL = function() end },\n"
            '  away = { PLATFORMS = { "plan9" }, CHECK = "{{nope}}", INSTALL = "x" },\n'
            '  other = { CHECK = "{{nope}}", INSTALL = "x" },\n'
            "}\n",
        )
        self.install_ok(self.manifest(("local.c@v1", c), setup=["main", "away"]))

    def test_a_cache_hit_is_never_checked(self):
        """`--run` through a cached package skips checking its verbs."""
        c = self.consumer('BUILD = "{{rec}} out.json built"\n'
                          'PRODUCTS = { cc = { command = { "{{rec}}" } } }\n')
        m = self.manifest(("local.c@v1", c))
        self.install_ok(m)
        self.consumer("BUILD = \"docker ps --format '{{.ID}}'\"\n"
                      'PRODUCTS = { cc = { command = { "{{rec}}" } } }\n')
        out = self.work / "ran.json"
        r = self.run_envy("product", "--manifest", m, "--run", "cc", "--", out, "z",
                          timeout=TIMEOUT)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(["z"], json.loads(out.read_text(encoding="utf-8")))

    def test_listing_ignores_a_bad_verb(self):
        """Graph-only commands skip phase-string checks."""
        c = self.consumer('BUILD = "{{nope}} x"\n')
        r = self.run_envy("product", "--json", "--manifest", self.manifest(("local.c@v1", c)),
                          timeout=TIMEOUT)
        self.assertEqual(r.returncode, 0, r.stderr)

    # -- refusals -----------------------------------------------------------

    @unittest.skipIf(WINDOWS, "a POSIX command product")
    def test_a_command_inside_quotes_is_refused(self):
        tool = self.write_spec(
            "t.lua",
            'IDENTITY = "local.t@v1"\nUSER_MANAGED = true\n'
            'PRODUCTS = { cmd = { command = { "/bin/echo", "$(printf INJECTED)" } } }\n'
            "SETUP = { main = { CHECK = function() return true end,\n"
            "                   INSTALL = function() end } }\n")
        c = self.consumer("BUILD = [[printf '%s\\n' \" {{cmd}} \" > out.txt]]\n",
                          deps='{ product = "cmd" }')
        self.fails(self.manifest(("local.t@v1", tool), ("local.c@v1", c)),
                   "local.c@v1 BUILD: '{{cmd}}' is a command product inside quoted text")

    def test_unknown_product_is_refused_with_the_escape_hint(self):
        c = self.consumer("BUILD = \"docker ps --format '{{.ID}}'\"\n")
        self.fails(self.manifest(("local.c@v1", c)),
                   "local.c@v1 BUILD: pkg 'local.c@v1' does not declare product "
                   "dependency on '.ID'; for the braces themselves write '{{{{.ID}}}}'")

    def test_transitive_product_is_refused_with_a_hint(self):
        c = self.consumer('BUILD = "{{py}} -c 1"\n')
        self.fails(self.manifest(("local.c@v1", c)), '{ product = "py" }')

    def test_path_outside_the_safe_set_is_refused(self):
        self.cache_root = self.make_temp_dir("cache") / "with space"
        c = self.consumer('BUILD = "{{py}} {{rec.py}} out.json"\n',
                          deps='{ product = "py" }, { product = "rec.py" }')
        self.fails(self.manifest(("local.c@v1", c)), "with space",
                   "under a cache root", "ENVY_CACHE_ROOT")

    def test_envy_run_without_a_package_refuses_placeholders(self):
        err = self.lua_error('envy.run("echo {{rec}}")')
        self.assertIn("'{{rec}}' names a product, but no package's phase runs this", err)
