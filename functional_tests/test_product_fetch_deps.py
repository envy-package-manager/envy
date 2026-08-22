"""envy.product inside a source.fetch function, and the edge that makes it safe.

A fetch function runs at spec_fetch, before its own spec exists, so it cannot rely
on the weak-resolution pass -- that runs only at a resolution barrier, after every
spec_fetch has finished. What it can rely on is its declared fetch dependencies:
`process_fetch_dependencies` wires them with needed_by=spec_fetch in the task's
on_start, strictly before step 0 queries its edges, and a dependency edge is
satisfied only at pkg_export. So by the time the fetch function body runs, every
declared fetch dependency is fully installed.

That is the whole safety argument, and it is one invariant:

    a product's provider is readable  <=>  the consumer holds a dependency edge on
    it whose needed_by has already been reached

The provider *name* comes from the project-wide registry (published eagerly, at
each package's own spec_fetch completion). The registry alone is never enough --
it answers "who provides this" with no happens-before at all, so every lookup
still has to clear the edge check. These tests pin both halves: the lookups that
must succeed, and the near-misses that must fail deterministically rather than
racily.
"""

from pathlib import Path

from .env import EnvyTestCase


# A cache-managed provider: its product resolves to pkg_path/jf, which only exists
# once install has run. Asserting on that path is how these tests prove the edge
# drove the provider all the way through install, not merely through spec_fetch.
TOOL_SPEC = """IDENTITY = "local.tool@v1"
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

# Same product name, different provider identity -- for the collision case. Sorts
# before local.tool@v1, so a sorted message names it first regardless of which
# worker won the race to register.
OTHER_TOOL_SPEC = """IDENTITY = "local.other@v1"
USER_MANAGED = true
PRODUCTS = { jf = "/opt/other/jf" }
SETUP = { m = { CHECK = function() return true end, INSTALL = function() end } }
"""

# Tail every bundle fetch function needs: emit a bundle manifest plus the one spec
# it advertises, then commit. Raw string -- the \n escapes belong to Lua.
BUNDLE_TAIL = r"""
        local b = io.open(tmp_dir .. "/envy-bundle.lua", "w")
        b:write('BUNDLE = "corp.specs@r1"\nSPECS = { ["corp.thing@r1"] = "thing.lua" }\n')
        b:close()
        local t = io.open(tmp_dir .. "/thing.lua", "w")
        t:write('IDENTITY = "corp.thing@r1"\nUSER_MANAGED = true\n' ..
                'SETUP = { m = { CHECK = function() return true end, ' ..
                'INSTALL = function() end } }\n')
        t:close()
        envy.commit_fetch({ "envy-bundle.lua", "thing.lua" })
"""


class TestProductInFetchFunctions(EnvyTestCase):
    def setUp(self):
        super().setUp()
        self.tool = self.write_spec("tool.lua", TOOL_SPEC)
        self.other = self.write_spec("other.lua", OTHER_TOOL_SPEC)

    # -- fixtures -----------------------------------------------------------

    def bundle_manifest(self, dep_entry: str, fetch_body: str) -> Path:
        """A manifest whose bundle has `dep_entry` as a fetch dep and runs `fetch_body`.

        The manifest-declared bundle is the case where the fetch function's phase
        context is the bundle package itself, so a fetch dependency declared here
        is owned by the same package that runs the fetch.
        """
        return self.write_manifest(
            f"""
BUNDLES = {{
  corp = {{ identity = "corp.specs@r1",
    source = {{
      dependencies = {{ {dep_entry} }},
      fetch = function(tmp_dir)
{fetch_body}
{BUNDLE_TAIL}
      end }} }},
}}
PACKAGES = {{ {{ spec = "corp.thing@r1", bundle = "corp" }} }}
"""
        )

    def strong_dep(self, spec: Path = None, product: str = None) -> str:
        entry = f'spec = "local.tool@v1", source = "{self.lua_path(spec or self.tool)}"'
        return "{ " + entry + (f', product = "{product}"' if product else "") + " }"

    @staticmethod
    def probe(product: str, label: str = "PROBE") -> str:
        """Lua that calls envy.product and reports outcome without failing the run."""
        return f"""
        local ok, v = pcall(function() return envy.product("{product}") end)
        envy.info("{label} ok=" .. tostring(ok) .. " -> " .. tostring(v))
"""

    def product_events(self, run, target: str):
        return [e.raw for e in run.events("lua_ctx_product_access") if e.raw["target"] == target]

    # -- the lookups that must succeed --------------------------------------

    def test_fetch_function_resolves_product_of_declared_fetch_dep(self):
        """The feature: a fetch dep declared by identity, its product asked by name.

        No `product =` on the entry. The name comes from the registry, the edge
        comes from the dependency, and the resolved value is under the provider's
        installed pkg dir -- which is the observable proof that the edge ran the
        provider through install before this fetch function started.
        """
        manifest = self.bundle_manifest(self.strong_dep(), self.probe("jf"))
        run = self.sync(manifest)

        self.assertEqual(0, run.returncode, run.stderr)
        self.assertIn("PROBE ok=true", run.stderr)

        events = self.product_events(run, "jf")
        self.assertEqual(1, len(events), events)
        event = events[0]
        self.assertTrue(event["allowed"], event)
        self.assertEqual("local.tool@v1", event["provider"])
        # needed_by is spec_fetch: the only value whose edge applies at step 0.
        self.assertEqual("spec_fetch", event["needed_by"])
        self.assertEqual("spec_fetch", event["current_phase"])
        # reason carries the resolved value; /pkg/ means install completed.
        self.assertPathEndsWith(event["reason"], "/pkg/jf")

    def test_explicit_product_pin_resolves(self):
        """`product =` on the entry pins the name to that provider, and still resolves."""
        manifest = self.bundle_manifest(
            self.strong_dep(product="jf"), self.probe("jf")
        )
        run = self.sync(manifest)

        self.assertEqual(0, run.returncode, run.stderr)
        self.assertIn("PROBE ok=true", run.stderr)
        event = self.product_events(run, "jf")[0]
        self.assertTrue(event["allowed"], event)
        self.assertEqual("local.tool@v1", event["provider"])

    def test_package_lookup_still_works_alongside_product(self):
        """envy.package in a fetch function is unchanged; both APIs agree on the dir."""
        body = self.probe("jf", "PRODUCT") + """
        local ok, v = pcall(function() return envy.package("local.tool@v1") end)
        envy.info("PACKAGE ok=" .. tostring(ok) .. " -> " .. tostring(v))
"""
        run = self.sync(self.bundle_manifest(self.strong_dep(), body))

        self.assertEqual(0, run.returncode, run.stderr)
        self.assertIn("PRODUCT ok=true", run.stderr)
        self.assertIn("PACKAGE ok=true", run.stderr)

        product_value = self.product_events(run, "jf")[0]["reason"]
        package_events = [e.raw for e in run.events("lua_ctx_package_access")]
        package_value = next(
            e["reason"] for e in package_events if e["target"] == "local.tool@v1"
        )
        # envy.product is envy.package plus the PRODUCTS-relative suffix, in the same
        # spelling: both go through util_normalized_path, so this is exact equality
        # rather than a separator-insensitive comparison. Asserted strictly on purpose
        # — the two used to disagree on Windows, where one returned generic separators
        # and the other native ones.
        sep = "\\" if package_value[1:3] == ":\\" else "/"
        self.assertEqual(product_value, package_value.rstrip(sep) + sep + "jf")

    # -- the near-misses that must fail deterministically --------------------

    def test_transitive_provider_is_rejected(self):
        """Registry hit, no edge: the provider is a dep of a dep, so access is refused.

        Deterministic by construction. local.b@v1 is reached only through
        local.a@v1, whose own edge runs it through pkg_export before local.a@v1
        finishes -- so local.b@v1 is registered well before this fetch function
        runs. The registry therefore *does* answer, and the refusal comes from the
        edge check alone. Allowing it would mean reading a package this one never
        declared, which is exactly the reachable-by-accident coupling the edge
        check exists to prevent.
        """
        self.write_spec(
            "b.lua",
            """IDENTITY = "local.b@v1"
USER_MANAGED = true
PRODUCTS = { jf = "/opt/b/jf" }
SETUP = { m = { CHECK = function() return true end, INSTALL = function() end } }
""",
        )
        a = self.write_spec(
            "a.lua",
            """IDENTITY = "local.a@v1"
USER_MANAGED = true
DEPENDENCIES = { { spec = "local.b@v1", source = "b.lua", needed_by = "build" } }
SETUP = { m = { CHECK = function() return true end, INSTALL = function() end } }
""",
        )
        dep = '{ spec = "local.a@v1", source = "' + self.lua_path(a) + '" }'
        run = self.sync(self.bundle_manifest(dep, self.probe("jf")))

        self.assertIn("PROBE ok=false", run.stderr)
        # Names the provider it found and what is missing -- not "unknown product".
        self.assertIn("'local.b@v1' provides product 'jf'", run.stderr)
        self.assertIn("does not depend on it", run.stderr)

        event = self.product_events(run, "jf")[0]
        self.assertFalse(event["allowed"], event)
        self.assertEqual("local.b@v1", event["provider"])

    def test_unknown_product_is_rejected(self):
        """Registry miss and no declaration: the pre-existing error, unchanged."""
        run = self.sync(self.bundle_manifest(self.strong_dep(), self.probe("nosuch")))

        self.assertIn("PROBE ok=false", run.stderr)
        self.assertIn("does not declare product dependency on 'nosuch'", run.stderr)
        event = self.product_events(run, "nosuch")[0]
        self.assertFalse(event["allowed"], event)
        self.assertEqual("", event["provider"])

    def test_pinned_product_absent_from_provider_names_the_provider(self):
        """A pin to a provider that does not declare the product fails on the provider."""
        manifest = self.bundle_manifest(
            self.strong_dep(product="nope"), self.probe("nope")
        )
        run = self.sync(manifest)

        self.assertIn("PROBE ok=false", run.stderr)
        self.assertIn("Product 'nope' not found in provider 'local.tool@v1'", run.stderr)

    def test_needed_by_gate_applies_to_registry_lookups(self):
        """The gate is edge-applicability, so a later needed_by denies an earlier phase.

        One run, two probes. The dependency is needed_by=build, so its edge is
        absent at pkg_fetch (3 < 5) and present at pkg_build. The registry answers
        identically at both points -- only the edge differs, and only the edge
        decides. This is the case the doc's cause-2 hit: the error is telling the
        truth, because at fetch nothing has forced the provider anywhere.
        """
        self.write_spec(
            "prov.lua",
            """IDENTITY = "local.prov@v1"
USER_MANAGED = true
PRODUCTS = { jf = "/opt/prov/jf" }
SETUP = { m = { CHECK = function() return true end, INSTALL = function() end } }
""",
        )
        consumer = self.write_spec(
            "consumer.lua",
            """IDENTITY = "local.consumer@v1"
DEPENDENCIES = {
  { spec = "local.prov@v1", source = "prov.lua", needed_by = "build" },
}
FETCH = function(tmp_dir)
  local ok, v = pcall(function() return envy.product("jf") end)
  envy.info("AT_FETCH ok=" .. tostring(ok) .. " -> " .. tostring(v))
  local f = io.open(tmp_dir .. "/x", "w") f:write("x") f:close()
  envy.commit_fetch("x")
end
STAGE = function() end
BUILD = function(build_dir, stage_dir, fetch_dir)
  local ok, v = pcall(function() return envy.product("jf") end)
  envy.info("AT_BUILD ok=" .. tostring(ok) .. " -> " .. tostring(v))
end
INSTALL = function(install_dir, stage_dir, fetch_dir)
  envy.copy(fetch_dir .. "x", install_dir .. "x")
end
""",
        )
        manifest = self.write_manifest(
            'PACKAGES = { { spec = "local.consumer@v1", source = "'
            + self.lua_path(consumer)
            + '" } }'
        )
        run = self.sync(manifest)

        self.assertEqual(0, run.returncode, run.stderr)
        self.assertIn("AT_FETCH ok=false", run.stderr)
        self.assertIn(
            "product 'jf' needed_by 'build' but accessed during 'fetch'", run.stderr
        )
        self.assertIn("AT_BUILD ok=true -> /opt/prov/jf", run.stderr)

        phases = {
            e["current_phase"]: e["allowed"] for e in self.product_events(run, "jf")
        }
        self.assertEqual({"fetch": False, "build": True}, phases)

    # -- declarations that must not parse -----------------------------------

    def test_bare_product_fetch_dep_is_rejected(self):
        """`{ product = "jf" }` as a fetch dep: no edge is possible, so refuse it.

        Rejected at parse, before the entry (which carries an empty identity) can
        reach the graph and be pushed into the weak pass as an empty query.
        """
        run = self.sync(self.bundle_manifest('{ product = "jf" }', ""))

        self.assertNotEqual(0, run.returncode)
        self.assertIn("must be a strong reference", run.stderr)

    def test_product_fetch_dep_without_source_is_rejected(self):
        """A product entry naming a spec but no source is still a weak reference."""
        run = self.sync(
            self.bundle_manifest('{ spec = "local.tool@v1", product = "jf" }', "")
        )

        self.assertNotEqual(0, run.returncode)
        self.assertIn("must be a strong reference", run.stderr)

    # -- registry mechanics -------------------------------------------------

    def test_product_collision_message_is_stable(self):
        """Two providers of one name collide, and the message does not depend on timing.

        Registration is eager, so which worker wins is scheduling-dependent; the
        message sorts the two identities so the failure is still reproducible.
        """
        manifest = self.write_manifest(
            f"""
PACKAGES = {{
  {{ spec = "local.tool@v1", source = "{self.lua_path(self.tool)}" }},
  {{ spec = "local.other@v1", source = "{self.lua_path(self.other)}" }},
}}
"""
        )

        messages = set()
        for _ in range(3):
            run = self.sync(manifest)
            self.assertNotEqual(0, run.returncode)
            line = next(
                ln
                for ln in run.stderr.splitlines()
                if "provided by multiple specs" in ln
            )
            messages.add(line.strip())

        self.assertEqual(1, len(messages), f"collision message varied: {messages}")
        self.assertIn(
            "provided by multiple specs: local.other@v1, local.tool@v1",
            messages.pop(),
        )

    def test_parallel_fetch_deps_each_resolve_their_own_product(self):
        """Many fetch functions resolving products at once, each getting its own.

        Eager registration publishes from package workers while other workers are
        reading, so this drives the concurrent path that the barrier used to hide:
        N providers registering while N fetch functions look names up. A crossed
        wire shows up as a fetch function resolving a path with someone else's
        index in it, and a lock-order mistake shows up as a hang.
        """
        count = 6
        bundles, packages = [], []
        for i in range(count):
            # Distinct product name *and* distinct payload file per index, so a
            # crossed wire cannot coincidentally resolve to a valid-looking path.
            spec = self.write_spec(
                f"tool{i}.lua",
                f"""IDENTITY = "local.tool{i}@v1"
PRODUCTS = {{ jf{i} = "jf{i}" }}
FETCH = function(tmp_dir)
  local f = io.open(tmp_dir .. "/jf{i}", "w") f:write("x") f:close()
  envy.commit_fetch("jf{i}")
end
STAGE = function() end
INSTALL = function(install_dir, stage_dir, fetch_dir)
  envy.copy(fetch_dir .. "jf{i}", install_dir .. "jf{i}")
end
""",
            )
            bundles.append(
                f"""
  corp{i} = {{ identity = "corp.specs{i}@r1",
    source = {{
      dependencies = {{ {{ spec = "local.tool{i}@v1",
                        source = "{self.lua_path(spec)}" }} }},
      fetch = function(tmp_dir)
        local v = envy.product("jf{i}")
        envy.info("RESOLVED{i}=" .. v)
        local b = io.open(tmp_dir .. "/envy-bundle.lua", "w")
        b:write('BUNDLE = "corp.specs{i}@r1"\\nSPECS = {{ ["corp.thing{i}@r1"] = "thing.lua" }}\\n')
        b:close()
        local t = io.open(tmp_dir .. "/thing.lua", "w")
        t:write('IDENTITY = "corp.thing{i}@r1"\\nUSER_MANAGED = true\\n' ..
                'SETUP = {{ m = {{ CHECK = function() return true end, ' ..
                'INSTALL = function() end }} }}\\n')
        t:close()
        envy.commit_fetch({{ "envy-bundle.lua", "thing.lua" }})
      end }} }},"""
            )
            packages.append(
                f'  {{ spec = "corp.thing{i}@r1", bundle = "corp{i}" }},'
            )

        manifest = self.write_manifest(
            "BUNDLES = {" + "".join(bundles) + "\n}\nPACKAGES = {\n"
            + "\n".join(packages)
            + "\n}\n"
        )
        run = self.sync(manifest)

        self.assertEqual(0, run.returncode, run.stderr)
        for i in range(count):
            # Each fetch function must see its own provider, not a neighbour's.
            events = self.product_events(run, f"jf{i}")
            self.assertEqual(1, len(events), f"jf{i}: {events}")
            self.assertTrue(events[0]["allowed"], events[0])
            self.assertEqual(f"local.tool{i}@v1", events[0]["provider"])
            self.assertPathEndsWith(events[0]["reason"], f"/pkg/jf{i}")

    def test_edge_must_belong_to_the_registry_provider(self):
        """The edge has to be *this* provider's, not merely one sharing its identity.

        pkg::dependencies is keyed by bare identity while pkg_key includes options, so
        two option variants of one spec are distinct packages under one map key. The
        closure depends on the debug variant and only the release variant declares the
        product, so an identity-keyed lookup would find the debug edge and then read
        the release package -- which the closure never drove through install.

        Deterministic by construction, like test_transitive_provider_is_rejected:
        local.gate@v1 is a fetch dep of the closure and depends on the release variant
        at needed_by=build, so that variant is through pkg_export -- hence registered
        -- before the fetch function runs. The registry therefore does answer, and the
        refusal comes from the edge check alone. Without that ordering the release
        variant is an unordered root and the refusal races between this message and
        the plain "does not declare product dependency" miss.
        """
        variant_tool = self.write_spec(
            "variant_tool.lua",
            """IDENTITY = "local.tool@v1"
PRODUCTS = function(options)
  if options.variant == "release" then return { wk = "wk" } end
  return {}
end
FETCH = function(tmp_dir)
  local f = io.open(tmp_dir .. "/wk", "w") f:write("w") f:close()
  envy.commit_fetch("wk")
end
STAGE = function() end
INSTALL = function(install_dir, stage_dir, fetch_dir)
  envy.copy(fetch_dir .. "wk", install_dir .. "wk")
end
""",
        )
        src = self.lua_path(variant_tool)
        gate = self.write_spec(
            "variant_gate.lua",
            f"""IDENTITY = "local.gate@v1"
USER_MANAGED = true
DEPENDENCIES = {{ {{ spec = "local.tool@v1", source = "{src}", needed_by = "build",
                  options = {{ variant = "release" }} }} }}
SETUP = {{ m = {{ CHECK = function() return true end, INSTALL = function() end }} }}
""",
        )
        manifest = self.write_manifest(
            f"""
BUNDLES = {{
  corp = {{ identity = "corp.specs@r1",
    source = {{
      dependencies = {{
        {{ spec = "local.tool@v1", source = "{src}",
          options = {{ variant = "debug" }} }},
        {{ spec = "local.gate@v1", source = "{self.lua_path(gate)}" }},
      }},
      fetch = function(tmp_dir)
{self.probe("wk")}
{BUNDLE_TAIL}
      end }} }},
}}
PACKAGES = {{
  {{ spec = "corp.thing@r1", bundle = "corp" }},
  {{ spec = "local.tool@v1", source = "{src}", options = {{ variant = "release" }} }},
}}
"""
        )
        run = self.sync(manifest)

        self.assertIn("PROBE ok=false", run.stderr)
        self.assertIn("'local.tool@v1' provides product 'wk'", run.stderr)
        self.assertIn("does not depend on it", run.stderr)
        event = self.product_events(run, "wk")[0]
        self.assertFalse(event["allowed"], event)
        # Non-empty provider is the registry-answered proof: a miss reports "".
        self.assertEqual("local.tool@v1", event["provider"])
