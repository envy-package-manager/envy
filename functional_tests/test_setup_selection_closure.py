"""A bootstrap-closure member takes SETUP selections from the manifest only.

Its setup can run before other specs are read, so a spec's selection is refused in
either order with one message.
"""

from pathlib import Path

from .env import EnvyRun, EnvyTestCase

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

TAIL = (
    ", so its setup phase can run before every spec's DEPENDENCIES have been read. "
    "Select the pair in a manifest PACKAGES entry for '{}' instead"
)


class TestSetupSelectionOnClosureMembers(EnvyTestCase):
    def user_managed(self, name: str, deps: str = "", parse_s: float = 0) -> Path:
        """local.<name>@v1: pair `p` leaves <name>.ran; reading the spec burns `parse_s`."""
        marker = self.lua_path(self.work / f"{name}.ran")
        return self.write_spec(
            f"{name}.lua",
            f"local t0 = os.clock() while os.clock() - t0 < {parse_s} do end\n"
            f'IDENTITY = "local.{name}@v1"\n'
            "USER_MANAGED = true\n"
            f"DEPENDENCIES = {{ {deps} }}\n"
            "SETUP = { p = { CHECK = function() return false end,\n"
            f'  INSTALL = function() io.open("{marker}", "w"):close() end }} }}\n',
        )

    def entry(self, name: str, extra: str = "") -> str:
        path = self.lua_path(self.work / f"{name}.lua")
        return f'{{ spec = "local.{name}@v1", source = "{path}"{extra} }}'

    def run_graph(self, *packages: str) -> EnvyRun:
        """Install `packages` beside a bundle whose source.dependencies is local.f@v1."""
        return self.install(
            self.write_manifest(
                f"""
BUNDLES = {{
  corp = {{ identity = "corp.specs@r1",
    source = {{
      dependencies = {{ {self.entry("f")} }},
      fetch = function(tmp_dir)
{BUNDLE_TAIL}
      end }} }},
}}
PACKAGES = {{
  {{ spec = "corp.thing@r1", bundle = "corp" }},
  {",".join(packages)}
}}
"""
            )
        )

    def assertRefusedAlike(self, packages: list[str], message: str):
        """Every run fails with `message`."""
        for attempt in range(3):
            run = self.run_graph(*packages)
            self.assertNotEqual(0, run.returncode, f"run {attempt} passed\n{run.stderr}")
            self.assertIn(message, run.stderr, f"run {attempt}")
            self.assertNotIn("arrived after its setup phase ran", run.stderr)

    def test_spec_selecting_a_prerequisites_pair_is_refused(self):
        """Before the fix this passed when b parsed fast (20/20) and failed when slow
        (10/10)."""
        self.user_managed("f")
        for parse_s in (0, 0.3):
            with self.subTest(parse_s=parse_s):
                self.user_managed("b", self.entry("f", ', setup = { "p" }'), parse_s)
                self.assertRefusedAlike(
                    [self.entry("b")],
                    "'local.f@v1' cannot take a SETUP selection from spec 'local.b@v1' "
                    "(pair 'p'): it is a source.dependencies prerequisite of "
                    "'corp.specs@r1'" + TAIL.format("local.f@v1"),
                )
        self.assertFalse((self.work / "f.ran").exists())

    def test_refusal_names_the_chain_to_a_transitive_member(self):
        self.user_managed("h")
        self.user_managed("f", self.entry("h"))
        self.user_managed("b", self.entry("h", ', setup = { "p" }'))
        self.assertRefusedAlike(
            [self.entry("b")],
            "'local.h@v1' cannot take a SETUP selection from spec 'local.b@v1' (pair 'p'): "
            "it is a dependency of 'local.f@v1', a source.dependencies prerequisite of "
            "'corp.specs@r1'" + TAIL.format("local.h@v1"),
        )

    def test_manifest_selection_on_a_prerequisite_runs(self):
        """The fix the message suggests: the same selection in the manifest."""
        self.user_managed("f")
        self.user_managed("b", self.entry("f"))
        run = self.run_graph(self.entry("f", ', setup = { "p" }'), self.entry("b"))
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertTrue((self.work / "f.ran").exists())

    # -- weak references ----------------------------------------------------

    def test_weak_reference_to_a_prerequisite_resolves(self):
        """A weak entry without `setup` still resolves to a closure member."""
        self.user_managed("f")
        self.user_managed("b", '{ spec = "local.f" }')
        run = self.run_graph(self.entry("b"))
        self.assertEqual(0, run.returncode, run.stderr)
        edges = [
            (e.spec, e.raw["dependency"])
            for e in run.events()
            if e.event == "dependency_added"
        ]
        self.assertIn(("local.b@v1", "local.f@v1"), edges)  # wired by the weak pass

    def test_weak_selection_on_a_prerequisite_is_refused_alike(self):
        """A weak entry's selection gets the strong entry's refusal."""
        self.user_managed("f")
        self.user_managed("b", '{ spec = "local.f", setup = { "p" } }')
        self.assertRefusedAlike(
            [self.entry("b")],
            "'local.f@v1' cannot take a SETUP selection from spec 'local.b@v1' (pair 'p'): "
            "it is a source.dependencies prerequisite of 'corp.specs@r1'"
            + TAIL.format("local.f@v1"),
        )

