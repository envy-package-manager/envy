"""A spec-declared `source = { fetch, dependencies }`: whose context runs it.

The fetch function is a closure in the *parent's* Lua state, but everything it is
allowed to reach belongs to the *child*: `source.dependencies` are wired onto the
child package, and `options` are the child entry's, not the parent's. The phase
context therefore names the child while the interpreter stays the parent's --
otherwise a fetch function's own declared prerequisites are invisible to it and it
reads the wrong options table.

Whatever the function commits is the child's spec directory, not just `spec.lua`:
a multi-file spec is the documented reason to write a fetch function at all.
"""

import unittest

from .env import EnvyTestCase


# A cache-managed provider: `jf` resolves under its installed pkg dir, so asserting
# on that path proves the edge drove it through install before the fetch ran.
TOOL_SPEC = """IDENTITY = "local.cf_tool@v1"
PRODUCTS = { jf = "jf" }
FETCH = function(tmp_dir)
  local f = io.open(tmp_dir .. "/jf", "w") f:write("x") f:close()
  envy.commit_fetch("jf")
end
STAGE = function() end
INSTALL = function(install_dir, stage_dir, fetch_dir)
  envy.copy(fetch_dir .. "jf", install_dir .. "jf")
end
"""

# The spec a fetch function writes: inert, so a test asserts on the fetch, not on it.
CHILD_SPEC_LUA = (
    "'IDENTITY = \"local.cf_child@v1\"\\n' .. "
    "'USER_MANAGED = true\\n' .. "
    "'SETUP = { m = { CHECK = function() return true end, "
    "INSTALL = function() end } }\\n'"
)


class TestSpecDeclaredCustomFetch(EnvyTestCase):
    def setUp(self):
        super().setUp()
        self.tool = self.write_spec("cf_tool.lua", TOOL_SPEC)

    # -- fixtures -----------------------------------------------------------

    def parent_spec(self, entry: str, identity: str = "local.cf_parent@v1") -> str:
        """A user-managed parent whose sole dependency is `entry`."""
        return f"""IDENTITY = "{identity}"
DEPENDENCIES = {{
{entry}
}}
USER_MANAGED = true
SETUP = {{ m = {{ CHECK = function() return true end, INSTALL = function() end }} }}
"""

    def run_parent(self, spec_text: str, identity: str = "local.cf_parent@v1",
                   manifest_options: str = ""):
        path = self.write_spec("cf_parent.lua", spec_text)
        opts = f", options = {manifest_options}" if manifest_options else ""
        manifest = self.write_manifest(
            f'PACKAGES = {{ {{ spec = "{identity}", '
            f'source = "{self.lua_path(path)}", setup = {{ "m" }}{opts} }} }}'
        )
        return self.install(manifest)

    # -- the child's own graph is what the fetch function sees ---------------

    def test_fetch_function_reads_its_own_fetch_dependency(self):
        """envy.product and envy.package inside the fetch resolve the child's deps.

        The edge lives on the child, so a parent-scoped context refuses both -- and
        refuses by naming a package the author never mentioned.
        """
        entry = f"""  {{ spec = "local.cf_child@v1", source = {{
    dependencies = {{ {{ spec = "local.cf_tool@v1",
                       source = "{self.lua_path(self.tool)}" }} }},
    fetch = function(tmp_dir, options)
      envy.info("PRODUCT=" .. envy.product("jf"))
      envy.info("PACKAGE=" .. envy.package("local.cf_tool@v1"))
      local f = io.open(tmp_dir .. "/spec.lua", "w")
      f:write({CHILD_SPEC_LUA})
      f:close()
      envy.commit_fetch("spec.lua")
    end }} }},"""
        run = self.run_parent(self.parent_spec(entry))

        self.assertEqual(0, run.returncode, run.stderr)
        product = next(
            ln.split("PRODUCT=", 1)[1].strip()
            for ln in run.stderr.splitlines()
            if "PRODUCT=" in ln
        )
        package = next(
            ln.split("PACKAGE=", 1)[1].strip()
            for ln in run.stderr.splitlines()
            if "PACKAGE=" in ln
        )
        self.assertPathEndsWith(product, "/pkg/jf")
        sep = "\\" if package[1:3] == ":\\" else "/"
        self.assertEqual(product, package.rstrip(sep) + sep + "jf")

    def test_bundle_fetch_function_reads_its_own_fetch_dependency(self):
        """Same rule for a spec-declared bundle: the bundle package owns the deps."""
        entry = f"""  {{ bundle = "corp.cf_specs@r1", source = {{
    dependencies = {{ {{ spec = "local.cf_tool@v1",
                       source = "{self.lua_path(self.tool)}" }} }},
    fetch = function(tmp_dir)
      envy.info("BUNDLE_PRODUCT=" .. envy.product("jf"))
      local b = io.open(tmp_dir .. "/envy-bundle.lua", "w")
      b:write('BUNDLE = "corp.cf_specs@r1"\\nSPECS = {{ ["corp.cf_thing@r1"] = "t.lua" }}\\n')
      b:close()
      local t = io.open(tmp_dir .. "/t.lua", "w")
      t:write('IDENTITY = "corp.cf_thing@r1"\\nUSER_MANAGED = true\\n' ..
              'SETUP = {{ m = {{ CHECK = function() return true end, ' ..
              'INSTALL = function() end }} }}\\n')
      t:close()
      envy.commit_fetch({{ "envy-bundle.lua", "t.lua" }})
    end }} }},"""
        run = self.run_parent(self.parent_spec(entry))

        self.assertEqual(0, run.returncode, run.stderr)
        value = next(
            ln.split("BUNDLE_PRODUCT=", 1)[1].strip()
            for ln in run.stderr.splitlines()
            if "BUNDLE_PRODUCT=" in ln
        )
        self.assertPathEndsWith(value, "/pkg/jf")

    # -- options -------------------------------------------------------------

    def test_options_are_the_childs_not_the_parents(self):
        """The entry's `options` reach the fetch function; the parent's do not."""
        entry = f"""  {{ spec = "local.cf_child@v1", options = {{ v = "child" }},
    source = {{ fetch = function(tmp_dir, options)
      if type(options) ~= "table" or options.v ~= "child" then
        error("BAD OPTIONS: " .. tostring(options and options.v))
      end
      local f = io.open(tmp_dir .. "/spec.lua", "w")
      f:write({CHILD_SPEC_LUA})
      f:close()
      envy.commit_fetch("spec.lua")
    end }} }},"""
        run = self.run_parent(self.parent_spec(entry),
                              manifest_options='{ v = "parent" }')

        self.assertEqual(0, run.returncode, run.stderr)
        self.assertNotIn("BAD OPTIONS", run.stderr)

    def test_option_variants_get_their_own_spec_cache_entry(self):
        """A closure has no fingerprint, so its options must be in the entry key.

        Nothing revalidates a complete spec entry. Two option sets drive the fetch
        function down two paths, so sharing one entry would serve the first run's
        bytes under the second run's declaration forever.
        """
        def entry(value: str) -> str:
            return f"""  {{ spec = "local.cf_child@v1", options = {{ v = "{value}" }},
    source = {{ fetch = function(tmp_dir, options)
      local f = io.open(tmp_dir .. "/spec.lua", "w")
      f:write({CHILD_SPEC_LUA} .. 'MARKER = "' .. options.v .. '"\\n')
      f:close()
      envy.commit_fetch("spec.lua")
    end }} }},"""

        first = self.run_parent(self.parent_spec(entry("1")))
        self.assertEqual(0, first.returncode, first.stderr)
        self.assertEqual(1, len(self.spec_entries("local.cf_child@v1")))

        second = self.run_parent(self.parent_spec(entry("2")))
        self.assertEqual(0, second.returncode, second.stderr)
        self.assertEqual(2, len(self.spec_entries("local.cf_child@v1")))

        markers = {
            (e / "pkg" / "spec.lua").read_text(encoding="utf-8").strip().splitlines()[-1]
            for e in self.spec_entries("local.cf_child@v1")
        }
        self.assertEqual({'MARKER = "1"', 'MARKER = "2"'}, markers)

    # -- everything committed is the spec directory --------------------------

    def test_multi_file_commit_survives_into_the_spec_directory(self):
        """A fetch function may commit a whole spec tree, not only spec.lua."""
        entry = f"""  {{ spec = "local.cf_child@v1", source = {{
    fetch = function(tmp_dir, options)
      local h = io.open(tmp_dir .. "/cf_helper.lua", "w")
      h:write('MARKER = "helper-loaded"\\n')
      h:close()
      local f = io.open(tmp_dir .. "/spec.lua", "w")
      f:write('local helper = envy.loadenv("cf_helper")\\n' ..
              'assert(helper.MARKER == "helper-loaded", "helper missing")\\n' ..
              {CHILD_SPEC_LUA})
      f:close()
      envy.commit_fetch({{ "spec.lua", "cf_helper.lua" }})
    end }} }},"""
        run = self.run_parent(self.parent_spec(entry))

        self.assertEqual(0, run.returncode, run.stderr)
        entries = self.spec_entries("local.cf_child@v1")
        self.assertEqual(1, len(entries), entries)
        self.assertTrue((entries[0] / "pkg" / "cf_helper.lua").exists())


if __name__ == "__main__":
    unittest.main()
