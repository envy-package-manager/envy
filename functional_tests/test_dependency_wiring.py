"""One wiring path for every dependency edge.

Every edge in `pkg::dependencies` is added by `engine::wire_dependency`, which
refuses an edge whose target already reaches the parent. The old ancestor-chain
check only saw the spawn path that first started a package, so any edge reaching a
node by a different route -- a second manifest root, a weak fallback, a
`source.dependencies` prerequisite -- went in unchecked and the run wedged.

RED for the cycle cases is the task engine's deadlock watchdog ("Deadlock: ..."),
or a subprocess timeout if even that fails; GREEN is a wire-time "Dependency cycle
detected" naming the actual path. Each run carries its own timeout so a regression
fails instead of stalling the suite.
"""

from .env import EnvyTestCase


USER_MANAGED_TAIL = """
USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options) return true end,
    INSTALL = function(pkg_dir, options) end,
  },
}
"""

CACHED_TAIL = """
FETCH = function(tmp_dir)
  local f = io.open(tmp_dir .. "/x", "w") f:write("x") f:close()
  envy.commit_fetch("x")
end
STAGE = function() end
INSTALL = function(install_dir, stage_dir, fetch_dir)
  envy.copy(fetch_dir .. "x", install_dir .. "x")
end
"""

RUN_TIMEOUT_S = 30


class TestDependencyWiring(EnvyTestCase):
    # The RED path for the cycle cases waits out the engine's ~1s deadlock watchdog
    # before reporting, and a sanitized binary is not fast; the per-run timeout is
    # what actually bounds these.
    envy_watchdog_timeout = 120

    def user_managed(self, name: str, identity: str, dependencies: str = "") -> str:
        """Write a payload-free spec and return its Lua-quoted path."""
        path = self.write_spec(
            name,
            f'IDENTITY = "{identity}"\nDEPENDENCIES = {{ {dependencies} }}\n'
            + USER_MANAGED_TAIL,
        )
        return self.lua_path(path)

    def cached(self, name: str, identity: str, body: str) -> str:
        path = self.write_spec(name, f'IDENTITY = "{identity}"\n{body}' + CACHED_TAIL)
        return self.lua_path(path)

    def assertCycle(self, run, *identities):
        """A wire-time cycle error naming every package on the cycle, and no hang."""
        self.assertNotEqual(0, run.returncode, f"expected a cycle error\n{run.stderr}")
        self.assertNotIn("Deadlock", run.stderr, run.stderr)
        self.assertIn("cycle detected:", run.stderr, run.stderr)
        for identity in identities:
            self.assertIn(identity, run.stderr, run.stderr)

    # -- cycles the ancestor chain could not see ----------------------------

    def test_mutual_roots_cycle(self):
        """Two manifest roots depending on each other: neither has an ancestor chain."""
        a = self.user_managed(
            "mr_a.lua", "local.mr_a@v1", '{ spec = "local.mr_b@v1", source = "mr_b.lua" }'
        )
        b = self.user_managed(
            "mr_b.lua", "local.mr_b@v1", '{ spec = "local.mr_a@v1", source = "mr_a.lua" }'
        )
        manifest = self.write_manifest(
            f"""PACKAGES = {{
  {{ spec = "local.mr_a@v1", source = "{a}" }},
  {{ spec = "local.mr_b@v1", source = "{b}" }},
}}
"""
        )
        run = self.install(manifest, timeout=RUN_TIMEOUT_S)
        self.assertCycle(run, "local.mr_a@v1", "local.mr_b@v1")

    def test_diamond_with_back_edge(self):
        """A->B,C; B,C->D; D->B. B and D are roots, so neither chain names the other."""
        b = self.user_managed("dm_b.lua", "local.dm_b@v1",
                              '{ spec = "local.dm_d@v1", source = "dm_d.lua" }')
        self.user_managed("dm_c.lua", "local.dm_c@v1",
                          '{ spec = "local.dm_d@v1", source = "dm_d.lua" }')
        d = self.user_managed("dm_d.lua", "local.dm_d@v1",
                              '{ spec = "local.dm_b@v1", source = "dm_b.lua" }')
        a = self.user_managed(
            "dm_a.lua",
            "local.dm_a@v1",
            '{ spec = "local.dm_b@v1", source = "dm_b.lua" }, '
            '{ spec = "local.dm_c@v1", source = "dm_c.lua" }',
        )
        manifest = self.write_manifest(
            f"""PACKAGES = {{
  {{ spec = "local.dm_a@v1", source = "{a}" }},
  {{ spec = "local.dm_b@v1", source = "{b}" }},
  {{ spec = "local.dm_d@v1", source = "{d}" }},
}}
"""
        )
        run = self.install(manifest, timeout=RUN_TIMEOUT_S)
        self.assertCycle(run, "local.dm_b@v1", "local.dm_d@v1")

    def test_weak_fallback_resolving_to_an_ancestor(self):
        """A fallback that lands on an existing ancestor closes a cycle at wire time."""
        self.user_managed(
            "wf_dep.lua",
            "local.wf_dep@v1",
            '{ spec = "local.needs_it", '
            'weak = { spec = "local.wf_root@v1", source = "wf_root.lua" } }',
        )
        root = self.user_managed(
            "wf_root.lua",
            "local.wf_root@v1",
            '{ spec = "local.wf_dep@v1", source = "wf_dep.lua" }',
        )
        manifest = self.write_manifest(
            f'PACKAGES = {{ {{ spec = "local.wf_root@v1", source = "{root}" }} }}\n'
        )
        run = self.install(manifest, timeout=RUN_TIMEOUT_S)
        self.assertCycle(run, "local.wf_root@v1", "local.wf_dep@v1")

    def test_source_dependencies_back_edge(self):
        """A fetch prerequisite pointing back at a root that already depends on it.

        The custom-fetch entry lives in the root's own DEPENDENCIES: a manifest entry
        has no parent Lua state, so a `source = { fetch = ... }` there is refused at
        parse and never reaches the wiring this covers.
        """
        root = self.lua_path(self.work / "sd_root.lua")
        self.user_managed(
            "sd_root.lua",
            "local.sd_root@v1",
            f"""{{ spec = "local.sd_tool@v1",
    source = {{
      dependencies = {{ {{ spec = "local.sd_root@v1", source = "{root}" }} }},
      fetch = function(tmp_dir)
        error("cycle must be detected before the fetch function runs")
      end }} }}""",
        )
        manifest = self.write_manifest(
            f'PACKAGES = {{ {{ spec = "local.sd_root@v1", source = "{root}" }} }}\n'
        )
        run = self.install(manifest, timeout=RUN_TIMEOUT_S)
        self.assertCycle(run, "local.sd_root@v1", "local.sd_tool@v1")

    # -- one edge per (parent, dependency), at the tightest phase -----------

    def test_two_products_from_one_provider_tighten_the_edge(self):
        """Two product entries on one provider collapse to the earliest needed_by."""
        provider = self.cached(
            "tp_prov.lua",
            "local.tp_prov@v1",
            'PRODUCTS = { alpha = "x", beta = "x" }\n',
        )
        # The provider must be readable at the tighter of the two phases; STAGE is
        # where the loosened edge used to refuse access.
        consumer = self.write_spec(
            "tp_consumer.lua",
            f"""IDENTITY = "local.tp_consumer@v1"
DEPENDENCIES = {{
  {{ product = "alpha", spec = "local.tp_prov@v1", source = "{provider}",
     needed_by = "stage" }},
  {{ product = "beta", spec = "local.tp_prov@v1", source = "{provider}",
     needed_by = "install" }},
}}
FETCH = function(tmp_dir)
  local f = io.open(tmp_dir .. "/x", "w") f:write("x") f:close()
  envy.commit_fetch("x")
end
STAGE = function()
  envy.info("PROVIDER_AT_STAGE " .. envy.package("local.tp_prov@v1"))
  envy.info("ALPHA_AT_STAGE " .. envy.product("alpha"))
end
INSTALL = function(install_dir, stage_dir, fetch_dir)
  envy.copy(fetch_dir .. "x", install_dir .. "x")
end
""",
        )
        manifest = self.write_manifest(
            f'PACKAGES = {{ {{ spec = "local.tp_consumer@v1", '
            f'source = "{self.lua_path(consumer)}" }} }}\n'
        )
        run = self.install(manifest, timeout=RUN_TIMEOUT_S)
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertIn("PROVIDER_AT_STAGE", run.stderr, run.stderr)
        self.assertIn("ALPHA_AT_STAGE", run.stderr, run.stderr)

        added = [
            e
            for e in run.events("dependency_added", spec="local.tp_consumer@v1")
            if e.raw["dependency"] == "local.tp_prov@v1"
        ]
        self.assertEqual(["stage"], [e.raw["needed_by"] for e in added], added)

    # -- parse-time backstop ------------------------------------------------

    def test_duplicate_identity_with_different_options_is_a_parse_error(self):
        """An identity-keyed edge map cannot hold two option variants of one identity."""
        dup = self.user_managed("di_dup.lua", "local.di_dup@v1")
        consumer = self.user_managed(
            "di_consumer.lua",
            "local.di_consumer@v1",
            f'{{ spec = "local.di_dup@v1", source = "{dup}", options = {{ v = "1" }} }}, '
            f'{{ spec = "local.di_dup@v1", source = "{dup}", options = {{ v = "2" }} }}',
        )
        manifest = self.write_manifest(
            f'PACKAGES = {{ {{ spec = "local.di_consumer@v1", source = "{consumer}" }} }}\n'
        )
        run = self.install(manifest, timeout=RUN_TIMEOUT_S)
        self.assertNotEqual(0, run.returncode, run.stderr)
        self.assertIn("local.di_dup@v1", run.stderr, run.stderr)
        self.assertIn("local.di_consumer@v1", run.stderr, run.stderr)
        self.assertIn("different options", run.stderr, run.stderr)

    def test_duplicate_identity_in_source_dependencies_is_a_parse_error(self):
        """Same backstop inside a `source.dependencies` array."""
        dup = self.user_managed("sdi_dup.lua", "local.sdi_dup@v1")
        holder = self.user_managed(
            "sdi_holder.lua",
            "local.sdi_holder@v1",
            f"""{{ spec = "local.sdi_tool@v1",
    source = {{
      dependencies = {{
        {{ spec = "local.sdi_dup@v1", source = "{dup}", options = {{ v = "1" }} }},
        {{ spec = "local.sdi_dup@v1", source = "{dup}", options = {{ v = "2" }} }},
      }},
      fetch = function(tmp_dir) error("never runs") end }} }}""",
        )
        manifest = self.write_manifest(
            f'PACKAGES = {{ {{ spec = "local.sdi_holder@v1", source = "{holder}" }} }}\n'
        )
        run = self.install(manifest, timeout=RUN_TIMEOUT_S)
        self.assertNotEqual(0, run.returncode, run.stderr)
        self.assertIn("local.sdi_dup@v1", run.stderr, run.stderr)
        self.assertIn("different options", run.stderr, run.stderr)
