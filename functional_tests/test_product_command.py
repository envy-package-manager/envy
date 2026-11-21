"""Command products: a PRODUCTS entry whose value is an argv, not a path.

`yarn = { command = { "{{node}}", "{{yarn.cjs}}" } }` runs a script through another
product's interpreter. Every `{{name}}` is a product of the provider or its dependency
closure, rendered at each use -- `envy product --run`, `envy.product_command`, `envy
product` -- so nothing lands in the payload. These pin that.
"""

from __future__ import annotations

import json
import shlex
import sys
from pathlib import Path

from .env import EnvyTestCase

WINDOWS = sys.platform == "win32"

_TOOL_PY = "import json, sys\nprint(json.dumps(sys.argv))\n"

# `tool` runs its own `tool.py` through `py`; `wrapped` splices `tool` as a whole element.
_PRODUCTS = """{
  ["tool.py"] = { value = "lib/tool.py", script = false },
  tool = { command = { "{{py}}", "{{tool.py}}", "--fixed" } },
  wrapped = { command = { "{{tool}}", "--extra" } },
}"""


class TestProductCommand(EnvyTestCase):
    def setUp(self) -> None:
        super().setUp()
        self.tool_py = self.write_spec("tool.py", _TOOL_PY)
        self.py_spec = self.write_spec("py.lua", self.user_spec("local.py@v1"))

    # -- fixtures -----------------------------------------------------------

    @staticmethod
    def user_spec(identity: str, products: str | None = None) -> str:
        """A user-managed interpreter: `py` is this very Python."""
        py = Path(sys.executable).as_posix()
        return (
            f'IDENTITY = "{identity}"\nUSER_MANAGED = true\n'
            f"PRODUCTS = {products or '{ py = ' + json.dumps(py) + ' }'}\n"
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

    def manifest(self, *specs: tuple[str, Path]) -> Path:
        entries = "".join(
            f'  {{ spec = "{i}", source = "{self.lua_path(p)}" }},\n' for i, p in specs
        )
        return self.write_manifest(f"PACKAGES = {{\n{entries}}}\n")

    def standard(self, *more: tuple[str, Path]) -> Path:
        return self.manifest(("local.py@v1", self.py_spec),
                             ("local.tool@v1", self.tool_spec(_PRODUCTS)), *more)

    def ok(self, *args) -> str:
        r = self.run_envy(*args)
        self.assertEqual(r.returncode, 0, f"stdout:\n{r.stdout}\nstderr:\n{r.stderr}")
        return r.stdout

    def fails(self, manifest: Path, needle: str) -> None:
        r = self.install(manifest)
        self.assertNotEqual(r.returncode, 0, r.stderr)
        self.assertIn(needle, r.stderr)

    def assert_tool_argv(self, argv: list[str], rest: list[str]) -> None:
        self.assertPathEndsWith(argv[0], "/lib/tool.py")
        self.assertPathContains(argv[0], self.cache_root.as_posix())
        self.assertEqual(argv[1:], rest)

    # -- running ------------------------------------------------------------

    def test_run_renders_command_and_appends_args(self):
        m = self.standard()
        argv = json.loads(self.ok("product", "--manifest", m, "--run", "tool", "--",
                                  "a", "b c"))
        self.assert_tool_argv(argv, ["--fixed", "a", "b c"])

    def test_run_splices_a_whole_element_command(self):
        m = self.standard()
        argv = json.loads(self.ok("product", "--manifest", m, "--run", "wrapped", "--",
                                  "z"))
        self.assert_tool_argv(argv, ["--fixed", "--extra", "z"])

    def test_product_command_runs_in_a_consumer_phase(self):
        consumer = self.write_spec(
            "consumer.lua",
            'IDENTITY = "local.consumer@v1"\n'
            'DEPENDENCIES = { { product = "wrapped" } }\n'
            "FETCH = function() end\n"
            "INSTALL = function(install_dir)\n"
            '  local r = envy.run(envy.product_command("wrapped") .. " \'it s\'",\n'
            "                     { capture = true })\n"
            '  envy.json.write(install_dir .. "/out.json", envy.json.decode(r.stdout))\n'
            "end\n",
        )
        m = self.standard(("local.consumer@v1", consumer))
        self.ok("install", "--manifest", m)
        pkg = Path(self.ok("package", "local.consumer@v1", "--manifest", m).strip())
        argv = json.loads((pkg / "out.json").read_text(encoding="utf-8"))
        self.assert_tool_argv(argv, ["--fixed", "--extra", "it s"])

    def test_product_refuses_a_command(self):
        consumer = self.write_spec(
            "consumer.lua",
            'IDENTITY = "local.consumer@v1"\n'
            'DEPENDENCIES = { { product = "tool" } }\n'
            "FETCH = function() end\n"
            'INSTALL = function() envy.product("tool") end\n',
        )
        m = self.standard(("local.consumer@v1", consumer))
        self.fails(m, "envy.product: product 'tool' is a command, not a path; use "
                      "envy.product_command")

    # -- printing -----------------------------------------------------------

    def test_print_quotes_for_the_platform_shell(self):
        m = self.standard()
        line = self.ok("product", "--manifest", m, "tool").strip()
        if WINDOWS:
            self.assertTrue(line.startswith("& "), line)
            self.assertPathContains(line, "/lib/tool.py --fixed")
        else:
            argv = shlex.split(line)
            self.assertEqual(argv[0], Path(sys.executable).as_posix())
            self.assert_tool_argv(argv[1:], ["--fixed"])

    def test_json_lists_a_command_as_its_argv(self):
        m = self.standard()
        data = json.loads(self.ok("product", "--manifest", m, "--json"))
        self.assertEqual(data["py"], Path(sys.executable).as_posix())
        self.assertEqual(data["tool"], [data["py"], data["tool.py"], "--fixed"])
        self.assert_tool_argv(data["tool"][1:], ["--fixed"])
        self.assertEqual(data["wrapped"], data["tool"] + ["--extra"])

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

    def test_malformed_commands_are_refused_at_load(self):
        cases = {
            '{ tool = { value = "x", command = { "x" } } }':
                "PRODUCTS entry 'tool' has both 'value' and 'command'",
            '{ tool = { command = "x" } }':
                "PRODUCTS entry 'tool' command must be a list of strings",
            "{ tool = { command = {} } }":
                "PRODUCTS entry 'tool' command must be a non-empty list starting with a "
                "program",
            '{ tool = { command = { "{{py" } } }':
                "PRODUCTS entry 'tool' command unterminated '{{' in '{{py'",
        }
        for products, needle in cases.items():
            with self.subTest(products=products):
                m = self.manifest(("local.py@v1", self.py_spec),
                                  ("local.tool@v1", self.tool_spec(products)))
                self.fails(m, needle)

    def test_user_managed_command_takes_arguments(self):
        """A user-managed product, its own interpreter included, gains fixed args."""
        py = json.dumps(Path(sys.executable).as_posix())
        um = self.write_spec("um.lua", self.user_spec(
            "local.um@v1",
            f'{{ py = {py}, hi = {{ command = {{ "{{{{py}}}}", "-c", "print(42)" }} }} }}'))
        m = self.manifest(("local.um@v1", um))
        self.assertEqual(self.ok("product", "--manifest", m, "--run", "hi").strip(), "42")
