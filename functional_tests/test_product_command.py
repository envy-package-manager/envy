"""Command products: a PRODUCTS entry whose value is an argv, not a path.

`yarn = { command = { "{{node}}", "{{yarn.cjs}}" } }` runs a script through another
product's interpreter. Every `{{name}}` is a product of the provider or its dependency
closure, rendered at each use -- `envy product --run`, `envy.product_command`, `envy
product` -- so nothing lands in the payload. These pin that.
"""

from __future__ import annotations

import json
import os
import stat
import subprocess
import sys
import unittest
from pathlib import Path

from . import test_config
from .env import EnvyTestCase

WINDOWS = sys.platform == "win32"
PY = Path(sys.executable)

_TOOL_PY = "import json, sys\nprint(json.dumps(sys.argv))\n"

# `tool` runs its own `tool.py` through `py`; `wrapped` splices `tool` as a whole element.
_PRODUCTS = """{
  ["tool.py"] = { value = "lib/tool.py", script = false },
  tool = { command = { "{{py}}", "{{tool.py}}", "--fixed" } },
  wrapped = { command = { "{{tool}}", "--extra" } },
}"""


def _norm(path) -> str:
    return os.path.normcase(os.path.normpath(path))


class TestProductCommand(EnvyTestCase):
    def setUp(self) -> None:
        super().setUp()
        self.tool_py = self.write_spec("tool.py", _TOOL_PY)
        self.py_spec = self.write_spec("py.lua", self.user_spec("local.py@v1"))

    # -- fixtures -----------------------------------------------------------

    @staticmethod
    def user_spec(identity: str, products: str | None = None) -> str:
        """A user-managed spec; by default `py` is this very Python."""
        return (
            f'IDENTITY = "{identity}"\nUSER_MANAGED = true\n'
            f"PRODUCTS = {products or '{ py = ' + json.dumps(PY.as_posix()) + ' }'}\n"
            "SETUP = { main = { CHECK = function() return true end,\n"
            "                   INSTALL = function() end } }\n"
        )

    def tool_spec(self, products: str, deps: str = '{ product = "py" }') -> Path:
        """A cache-managed package holding lib/tool.py, which prints its argv."""
        return self.write_spec(
            "tool.lua",
            'IDENTITY = "local.tool@v1"\n'
            f"DEPENDENCIES = {{ {deps} }}\n"
            "FETCH = function() end\n"
            "INSTALL = function(install_dir)\n"
            f'  envy.copy("{self.lua_path(self.tool_py)}", install_dir .. "/lib/tool.py")\n'
            "end\n"
            f"PRODUCTS = {products}\n",
        )

    def consumer_spec(self, deps: str, install: str, build: str = "") -> Path:
        """local.consumer@v1: `install` runs in INSTALL, its JSON-able `out` saved."""
        return self.write_spec(
            "consumer.lua",
            'IDENTITY = "local.consumer@v1"\n'
            f"DEPENDENCIES = {{ {deps} }}\n"
            "FETCH = function() end\n"
            + (f"BUILD = function()\n{build}\nend\n" if build else "")
            + "INSTALL = function(install_dir)\n"
            f"{install}\n"
            '  envy.json.write(install_dir .. "/out.json", out)\n'
            "end\n",
        )

    def manifest(self, *specs: tuple[str, Path], header: str = "") -> Path:
        if not header:
            return test_config.write_spec_manifest(self.work, specs)
        entries = "\n".join(test_config.spec_entry(i, p) for i, p in specs)
        return self.write_manifest(f"{header}\nPACKAGES = {{\n{entries}\n}}\n")

    def standard(self, *more: tuple[str, Path], **kw) -> Path:
        return self.manifest(("local.py@v1", self.py_spec),
                             ("local.tool@v1", self.tool_spec(_PRODUCTS)), *more, **kw)

    def ok(self, *args, **kw) -> str:
        r = self.run_envy(*args, **kw)
        self.assertEqual(r.returncode, 0, f"stdout:\n{r.stdout}\nstderr:\n{r.stderr}")
        return r.stdout

    def fails(self, manifest: Path, *needles: str) -> None:
        r = self.install(manifest)
        self.assertNotEqual(r.returncode, 0, r.stderr)
        for needle in needles:
            self.assertIn(needle, r.stderr)

    def out(self, m: Path):
        """Install, then read what local.consumer@v1's INSTALL saved."""
        self.ok("install", "--manifest", m)
        pkg = Path(self.ok("package", "local.consumer@v1", "--manifest", m).strip())
        return json.loads((pkg / "out.json").read_text(encoding="utf-8"))

    def assert_tool_argv(self, m: Path, argv: list[str], rest: list[str]) -> None:
        pkg = Path(self.ok("package", "local.tool@v1", "--manifest", m).strip())
        self.assertEqual(_norm(argv[0]), _norm(pkg / "lib" / "tool.py"))
        self.assertEqual(argv[1:], rest)

    def path_env(self) -> dict[str, str]:
        """The test env with this Python's directory on PATH, for bare program names."""
        env = dict(test_config.get_test_env())
        env["PATH"] = str(PY.parent) + os.pathsep + env.get("PATH", "")
        return env

    # -- running ------------------------------------------------------------

    def test_run_renders_command_and_appends_args(self):
        m = self.standard()
        argv = json.loads(self.ok("product", "--manifest", m, "--run", "tool", "--",
                                  "a", "b c"))
        self.assert_tool_argv(m, argv, ["--fixed", "a", "b c"])

    def test_run_splices_a_whole_element_command(self):
        m = self.standard()
        argv = json.loads(self.ok("product", "--manifest", m, "--run", "wrapped", "--",
                                  "z"))
        self.assert_tool_argv(m, argv, ["--fixed", "--extra", "z"])

    def test_run_through_a_strong_edge(self):
        """The interpreter's package named by spec, no `product =`: still the closure."""
        deps = f'{{ spec = "local.py@v1", source = "{self.lua_path(self.py_spec)}" }}'
        m = self.manifest(("local.tool@v1", self.tool_spec(_PRODUCTS, deps)))
        argv = json.loads(self.ok("product", "--manifest", m, "--run", "tool"))
        self.assert_tool_argv(m, argv, ["--fixed"])

    @unittest.skipIf(WINDOWS, "the decoy wrapper is a POSIX script")
    def test_run_skips_the_wrapper_of_the_product_that_supplied_the_program(self):
        """`{{python3}}` renders bare; the bin dir's `python3` wrapper is not it."""
        name = PY.name
        products = '{ ["%s"] = "%s", tool = { command = { "{{%s}}", "%s", "--fixed" } } }'
        um = self.write_spec("um.lua", self.user_spec(
            "local.um@v1", products % (name, name, name, self.lua_path(self.tool_py))))
        m = self.manifest(("local.um@v1", um))
        decoy = self.work / "envy-bin" / name
        decoy.parent.mkdir()
        decoy.write_text("#!/bin/sh\necho '[\"decoy\"]'\n", encoding="utf-8")
        decoy.chmod(decoy.stat().st_mode | stat.S_IXUSR)
        argv = json.loads(self.ok("product", "--manifest", m, "--run", "tool",
                                  env=self.path_env()))
        self.assertEqual(argv[1:], ["--fixed"])

    def test_user_managed_command_takes_arguments(self):
        """A user-managed product, its own interpreter included, gains fixed args."""
        um = self.write_spec("um.lua", self.user_spec(
            "local.um@v1",
            f'{{ py = {json.dumps(PY.as_posix())}, '
            'hi = { command = { "{{py}}", "-c", "print(42)" } } }'))
        m = self.manifest(("local.um@v1", um))
        self.assertEqual(self.ok("product", "--manifest", m, "--run", "hi").strip(), "42")

    # -- envy.product_command / envy.product --------------------------------

    def test_product_command_runs_in_a_consumer_phase(self):
        consumer = self.consumer_spec(
            '{ product = "wrapped" }',
            '  local r = envy.run(envy.product_command("wrapped") .. " \'it s\'",\n'
            "                     { capture = true })\n"
            "  out = envy.json.decode(r.stdout)")
        m = self.standard(("local.consumer@v1", consumer))
        self.assert_tool_argv(m, self.out(m), ["--fixed", "--extra", "it s"])

    def test_product_command_is_authorized_and_traced_like_product(self):
        consumer = self.consumer_spec(
            '{ product = "tool", needed_by = "install" }',
            '  out = envy.product_command("tool")',
            build='  local ok, err = pcall(envy.product_command, "tool")\n'
                  "  assert(not ok and err:find(\"envy.product_command: product 'tool' "
                  "needed_by 'install' but accessed during 'build'\", 1, true), err)\n"
                  '  ok, err = pcall(envy.product_command, "undeclared")\n'
                  "  assert(not ok and err:find(\"envy.product_command: pkg "
                  "'local.consumer@v1' does not declare product dependency on "
                  "'undeclared'\", 1, true), err)")
        m = self.standard(("local.consumer@v1", consumer))
        r = self.run_envy("install", "--manifest", m)
        self.assertEqual(r.returncode, 0, r.stderr)
        pkg = Path(self.ok("package", "local.consumer@v1", "--manifest", m).strip())
        allowed_cmd = json.loads((pkg / "out.json").read_text(encoding="utf-8"))

        events = [e.raw for e in r.events("lua_ctx_product_access", "local.consumer@v1")]
        denied = [e for e in events if e["allowed"] is False]
        allowed = [e for e in events if e["allowed"] is True]
        self.assertEqual(len(denied), 2, events)
        self.assertIn("needed_by 'install' but accessed during 'build'", denied[0]["reason"])
        self.assertEqual(denied[0]["provider"], "local.tool@v1")
        self.assertEqual(len(allowed), 1, events)
        self.assertEqual(allowed[0]["target"], "tool")
        self.assertEqual(allowed[0]["reason"], allowed_cmd)

    def test_product_command_runs_a_path_product(self):
        """A path product is a one-element command: consumers need not know which."""
        consumer = self.consumer_spec(
            '{ product = "py" }',
            '  local r = envy.run(envy.product_command("py") .. " -c \\"print(7)\\"",\n'
            "                     { capture = true })\n"
            "  out = r.stdout")
        m = self.manifest(("local.py@v1", self.py_spec), ("local.consumer@v1", consumer))
        self.assertEqual(self.out(m).strip(), "7")

    def test_product_command_resolves_a_bare_program_on_path(self):
        um = self.write_spec("um.lua", self.user_spec(
            "local.py@v1",
            f'{{ py = "{PY.name}", nope = "envy-test-no-such-program" }}'))
        consumer = self.consumer_spec(
            '{ product = "py" }, { product = "nope" }',
            '  local ok, err = pcall(envy.product_command, "nope")\n'
            "  assert(not ok and err:find(\"envy.product_command: "
            "'envy-test-no-such-program' not on PATH\", 1, true), err)\n"
            '  out = envy.product_command("py")')
        m = self.manifest(("local.py@v1", um), ("local.consumer@v1", consumer))
        self.ok("install", "--manifest", m, env=self.path_env())
        pkg = Path(self.ok("package", "local.consumer@v1", "--manifest", m,
                           env=self.path_env()).strip())
        line = json.loads((pkg / "out.json").read_text(encoding="utf-8"))
        self.assertIn(_norm(PY.parent), _norm(line.replace("'", "")))

    def test_product_command_carries_or_refuses_awkward_arguments(self):
        """sh carries an empty argument and a '"' intact; powershell.exe 5.1 would drop
        one and strip the other, so on Windows envy refuses rather than mangle."""
        tool = self.tool_spec(
            '{ ["tool.py"] = { value = "lib/tool.py", script = false },\n'
            '  odd = { command = { "{{py}}", "{{tool.py}}", "", "say \\"hi\\"" } } }')
        consumer = self.consumer_spec(
            '{ product = "odd" }',
            '  local ok, cmd = pcall(envy.product_command, "odd")\n'
            "  out = ok and envy.json.decode(envy.run(cmd, { capture = true }).stdout)\n"
            "    or cmd")
        m = self.manifest(("local.py@v1", self.py_spec), ("local.tool@v1", tool),
                          ("local.consumer@v1", consumer))
        out = self.out(m)
        if WINDOWS:
            self.assertIn("envy.product_command: PowerShell cannot quote '': "
                          "powershell.exe drops an empty argument", out)
        else:
            self.assert_tool_argv(m, out, ["", 'say "hi"'])

    @unittest.skipIf(WINDOWS, "a POSIX custom shell")
    def test_custom_default_shell_needs_a_named_shell(self):
        consumer = self.consumer_spec(
            '{ product = "tool" }',
            '  local ok, err = pcall(envy.product_command, "tool")\n'
            "  assert(not ok and err:find(\"envy.product_command: the shell is custom\", "
            "1, true), err)\n"
            '  local r = envy.run(envy.product_command("tool", ENVY_SHELL.SH) .. " q",\n'
            "                     { capture = true })\n"
            "  out = envy.json.decode(r.stdout)")
        m = self.standard(("local.consumer@v1", consumer),
                          header='DEFAULT_SHELL = { file = { "/bin/sh" }, ext = ".sh" }')
        self.assert_tool_argv(m, self.out(m), ["--fixed", "q"])

    def test_product_refuses_a_command(self):
        consumer = self.consumer_spec('{ product = "tool" }', '  out = envy.product("tool")')
        m = self.standard(("local.consumer@v1", consumer))
        self.fails(m, "envy.product: product 'tool' is a command, not a path; use "
                      "envy.product_command")

    def test_weak_fallback_that_provides_transitively(self):
        """The fallback reaches `py` through its own dependency; both accessors find it."""
        fallback = self.write_spec(
            "f.lua",
            'IDENTITY = "local.f@v1"\n'
            f'DEPENDENCIES = {{ {{ spec = "local.py@v1", '
            f'source = "{self.lua_path(self.py_spec)}" }} }}\n'
            "FETCH = function() end\nINSTALL = function() end\n")
        consumer = self.consumer_spec(
            f'{{ product = "py", weak = {{ spec = "local.f@v1", '
            f'source = "{self.lua_path(fallback)}" }} }}',
            '  out = { path = envy.product("py"), command = envy.product_command("py") }')
        m = self.manifest(("local.consumer@v1", consumer))
        out = self.out(m)
        self.assertEqual(out["path"], PY.as_posix())
        self.assertIn(_norm(PY), _norm(out["command"].replace("'", "").lstrip("& ")))

    # -- printing -----------------------------------------------------------

    def test_print_round_trips_through_the_platform_shell(self):
        m = self.standard()
        line = self.ok("product", "--manifest", m, "tool").strip()
        shell = (["powershell", "-NoLogo", "-NoProfile", "-NonInteractive", "-Command"]
                 if WINDOWS else ["bash", "-c"])
        r = subprocess.run([*shell, line], capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 0, f"{line}\n{r.stderr}")
        self.assert_tool_argv(m, json.loads(r.stdout), ["--fixed"])

    def test_json_lists_a_command_as_its_predicted_argv(self):
        m = self.standard()
        data = json.loads(self.ok("product", "--manifest", m, "--json"))
        self.assertEqual(data["py"], PY.as_posix())
        self.assertEqual(data["tool"], [data["py"], data["tool.py"], "--fixed"])
        self.assertEqual(data["wrapped"], data["tool"] + ["--extra"])
        self.ok("install", "--manifest", m)
        self.assert_tool_argv(m, data["tool"][1:], ["--fixed"])  # where it then landed

    def test_listing_shows_the_command_as_declared(self):
        m = self.standard()
        r = self.run_envy("product", "--manifest", m)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("{{py}} {{tool.py}} --fixed", r.stderr)

    # -- validation ---------------------------------------------------------

    def test_placeholder_must_come_from_the_providers_closure(self):
        """`py` exists in the graph, but nothing in local.tool's closure provides it."""
        tool = self.tool_spec('{ tool = { command = { "{{py}}", "x" } } }', deps="")
        m = self.manifest(("local.py@v1", self.py_spec), ("local.tool@v1", tool))
        self.fails(m, "Product 'tool' in spec 'local.tool@v1' runs '{{py}}', but nothing "
                      "in its dependency closure provides 'py'")

    def test_validation_reports_every_bad_command(self):
        tool = self.tool_spec('{ a = { command = { "{{nope}}" } },\n'
                              '  b = { command = { "{{c}}" } },\n'
                              '  c = { command = { "{{b}}", "x" } } }')
        m = self.manifest(("local.py@v1", self.py_spec), ("local.tool@v1", tool))
        self.fails(m,
                   "Product 'a' in spec 'local.tool@v1' runs '{{nope}}'",
                   "Product 'b' in spec 'local.tool@v1' is a command cycle: b -> c -> b",
                   "Product 'c' in spec 'local.tool@v1' is a command cycle: c -> b -> c")

    def test_command_for_another_platform_is_not_validated(self):
        other = "linux" if WINDOWS else "windows"
        tool = self.tool_spec(
            f'{{ tool = {{ command = {{ "{{{{nope}}}}" }}, platforms = {{ "{other}" }} }} }}')
        m = self.manifest(("local.py@v1", self.py_spec), ("local.tool@v1", tool))
        self.ok("install", "--manifest", m)

    def test_malformed_commands_are_refused_at_load(self):
        cases = {
            '{ tool = { value = "x", command = { "x" } } }':
                "PRODUCTS entry 'tool' has both 'value' and 'command'",
            '{ tool = { command = "x" } }':
                "PRODUCTS entry 'tool' command must be a list of strings",
            '{ tool = { command = { "{{py}}", 42 } } }':
                "PRODUCTS entry 'tool' command must be a list of strings",
            '{ tool = { command = { "x", flag = "y" } } }':
                "PRODUCTS entry 'tool' command must be a list of strings",
            '{ tool = { command = { "x", nil, "y" } } }':
                "PRODUCTS entry 'tool' command must not have holes",
            "{ tool = { command = {} } }":
                "PRODUCTS entry 'tool' command must be a non-empty list starting with a "
                "program",
            '{ tool = { command = { "", "x" } } }':
                "PRODUCTS entry 'tool' command must be a non-empty list starting with a "
                "program",
            '{ tool = { command = { "{{py" } } }':
                "PRODUCTS entry 'tool' command unterminated '{{' in '{{py'",
            '{ tool = { command = { "{{ }}" } } }':
                "PRODUCTS entry 'tool' command empty placeholder in '{{ }}'",
            '{ tool = { command = { "bin/tool" } } }':
                "PRODUCTS entry 'tool' command starts with relative path 'bin/tool'",
        }
        for products, needle in cases.items():
            with self.subTest(products=products):
                m = self.manifest(("local.py@v1", self.py_spec),
                                  ("local.tool@v1", self.tool_spec(products)))
                self.fails(m, needle)
