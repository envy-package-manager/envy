"""A fetch that produces no usable spec must not finalize its cache entry.

An entry carrying `envy-complete` is trusted forever after -- envy takes the fast
path and never revalidates what is on disk. Finalizing before the fetched bytes are
known to be a loadable spec therefore turns one bad fetch (a 404 body served as
200, a malformed bundle manifest, a fetch function that writes the wrong spec) into
a permanent failure: every later run re-reads the same broken entry and fails
identically, even after the source itself is repaired.

Each test fails, repairs the source, and requires the next run to succeed. That
last run is the regression: with the entry finalized too early it still fails,
because envy never looks at the repaired source again.
"""

import shutil
import unittest

from .env import EnvyRun, EnvyTestCase

# CHECK succeeds, so nothing is ever installed: these tests are about spec_fetch.
USER_MANAGED_SPEC = """IDENTITY = "{identity}"
DEPENDENCIES = {{}}
USER_MANAGED = true
SETUP = {{
  main = {{ CHECK = "exit 0", INSTALL = "exit 0" }},
}}
"""


class CachePoisoningTestBase(EnvyTestCase):
    """Assertions about whether a spec cache entry survived a run."""

    def assert_not_finalized(self, identity: str, run: EnvyRun):
        """The run failed and left every entry cold, so a later run refetches."""
        self.assertNotEqual(
            0, run.returncode, f"expected failure, got success: {run.stdout}"
        )
        self.assertFalse(
            self.spec_complete(identity),
            f"{identity}: a failed fetch finalized a cache entry "
            f"({self.spec_entries(identity)})",
        )
        dispositions = [
            e.raw["disposition"]
            for e in run.events("cache_entry_finalized", spec=identity)
        ]
        self.assertNotIn(
            "completed", dispositions, f"{identity}: entry finalized as complete"
        )

    def assert_finalized(self, identity: str, run: EnvyRun):
        self.assertEqual(0, run.returncode, f"stderr: {run.stderr}")
        self.assertTrue(
            self.spec_complete(identity), f"{identity}: no finalized cache entry"
        )


class TestBundleCachePoisoning(CachePoisoningTestBase):
    """A bundle copied into the cache from a local directory.

    The identity does not start with `local.`, so the directory is copied into
    `specs/<identity>/pkg` rather than used in situ -- the path that made a
    malformed envy-bundle.lua a permanent cache entry.
    """

    IDENTITY = "test.poison-bundle@v1"

    def setUp(self):
        super().setUp()
        self.bundle_dir = self.work / "bundle-src"
        (self.bundle_dir / "specs").mkdir(parents=True)
        self.manifest = self.write_manifest(
            f"""
BUNDLES = {{
    tc = {{
        identity = "{self.IDENTITY}",
        source = "{self.lua_path(self.bundle_dir)}",
    }},
}}

PACKAGES = {{
    {{ spec = "test.poison-a@v1", bundle = "tc", setup = {{ "main" }} }},
}}
"""
        )

    def write_bundle(self, manifest_body: str, with_spec: bool = True):
        (self.bundle_dir / "envy-bundle.lua").write_text(manifest_body, encoding="utf-8")
        spec = self.bundle_dir / "specs" / "a.lua"
        spec.unlink(missing_ok=True)
        if with_spec:
            spec.write_text(
                USER_MANAGED_SPEC.format(identity="test.poison-a@v1"), encoding="utf-8"
            )

    def valid_bundle(self) -> str:
        return (
            f'BUNDLE = "{self.IDENTITY}"\n'
            'SPECS = { ["test.poison-a@v1"] = "specs/a.lua" }\n'
        )

    def test_unparseable_bundle_manifest_is_not_cached(self):
        self.write_bundle(f'BUNDLE = "{self.IDENTITY}"\nnot lua at all(((\n')

        self.assert_not_finalized(self.IDENTITY, self.install(self.manifest))
        self.assert_not_finalized(self.IDENTITY, self.install(self.manifest))

        self.write_bundle(self.valid_bundle())
        self.assert_finalized(self.IDENTITY, self.install(self.manifest))

    def test_bundle_identity_mismatch_is_not_cached(self):
        self.write_bundle(
            'BUNDLE = "test.some-other-bundle@v9"\n'
            'SPECS = { ["test.poison-a@v1"] = "specs/a.lua" }\n'
        )

        self.assert_not_finalized(self.IDENTITY, self.install(self.manifest))

        self.write_bundle(self.valid_bundle())
        self.assert_finalized(self.IDENTITY, self.install(self.manifest))

    def test_bundle_with_missing_spec_file_is_not_cached(self):
        self.write_bundle(self.valid_bundle(), with_spec=False)

        self.assert_not_finalized(self.IDENTITY, self.install(self.manifest))

        self.write_bundle(self.valid_bundle())
        self.assert_finalized(self.IDENTITY, self.install(self.manifest))


class TestRemoteSpecCachePoisoning(CachePoisoningTestBase):
    """A spec downloaded over HTTP.

    The URL never changes; only what the server returns does. That is the shape of
    the real failure -- an origin that 200s an error page, or serves a truncated
    file, and is fixed later.
    """

    IDENTITY = "test.poison-remote@v1"

    def setUp(self):
        super().setUp()
        self.serve_dir = self.work / "www"
        self.serve_dir.mkdir()
        url = self.serve_directory(self.serve_dir) + "/spec.lua"
        self.manifest = self.write_manifest(
            f"""
PACKAGES = {{
    {{ spec = "{self.IDENTITY}", source = "{url}", setup = {{ "main" }} }},
}}
"""
        )

    def serve(self, body: str):
        (self.serve_dir / "spec.lua").write_text(body, encoding="utf-8")

    def test_error_page_served_as_spec_is_not_cached(self):
        self.serve("<html><body>404 Not Found</body></html>\n")

        self.assert_not_finalized(self.IDENTITY, self.install(self.manifest))
        self.assert_not_finalized(self.IDENTITY, self.install(self.manifest))

        self.serve(USER_MANAGED_SPEC.format(identity=self.IDENTITY))
        self.assert_finalized(self.IDENTITY, self.install(self.manifest))

    def test_spec_with_wrong_identity_is_not_cached(self):
        self.serve(USER_MANAGED_SPEC.format(identity="test.someone-else@v9"))

        self.assert_not_finalized(self.IDENTITY, self.install(self.manifest))

        self.serve(USER_MANAGED_SPEC.format(identity=self.IDENTITY))
        self.assert_finalized(self.IDENTITY, self.install(self.manifest))


@unittest.skipUnless(shutil.which("git"), "git is not on PATH")
class TestGitSpecCachePoisoning(CachePoisoningTestBase):
    """A spec cloned from a git repo, pinned to a branch.

    The ref is part of the cache key, so committing a fix to the same branch leaves
    the key unchanged -- exactly the case where a finalized bad entry would be
    permanent. Repairing the branch must be enough.
    """

    IDENTITY = "test.poison-git@v1"

    def setUp(self):
        super().setUp()
        self.repo = self.make_git_repo(
            {"spec.lua": USER_MANAGED_SPEC.format(identity="test.someone-else@v9")},
            "gitpoison",
        )
        self.manifest = self.write_manifest(
            f"""
PACKAGES = {{
    {{ spec = "{self.IDENTITY}", source = "{self.lua_path(self.repo)}",
       ref = "main", setup = {{ "main" }} }},
}}
"""
        )

    def test_repo_with_the_wrong_spec_is_not_cached(self):
        self.assert_not_finalized(self.IDENTITY, self.install(self.manifest))
        self.assert_not_finalized(self.IDENTITY, self.install(self.manifest))

        self.commit_files(
            self.repo,
            {"spec.lua": USER_MANAGED_SPEC.format(identity=self.IDENTITY)},
            "fix identity",
        )
        self.assert_finalized(self.IDENTITY, self.install(self.manifest))

    def test_repo_without_a_spec_is_not_cached(self):
        repo = self.make_git_repo({"readme.txt": "no spec here\n"}, "gitempty")
        manifest = self.write_manifest(
            f"""
PACKAGES = {{
    {{ spec = "{self.IDENTITY}", source = "{self.lua_path(repo)}",
       ref = "main", setup = {{ "main" }} }},
}}
"""
        )

        self.assert_not_finalized(self.IDENTITY, self.install(manifest))

        self.commit_files(
            repo,
            {"spec.lua": USER_MANAGED_SPEC.format(identity=self.IDENTITY)},
            "add spec",
        )
        self.assert_finalized(self.IDENTITY, self.install(manifest))


class TestCustomFetchSpecCachePoisoning(CachePoisoningTestBase):
    """A spec produced by a parent spec's `source = { fetch = ... }` function.

    The fetch function is the source here, and the parent holding it is local, so
    rewriting the parent is the repair.
    """

    IDENTITY = "test.poison-custom@v1"

    def setUp(self):
        super().setUp()
        self.parent_spec = self.work / "parent.lua"
        self.manifest = self.write_manifest(
            f"""
PACKAGES = {{
    {{ spec = "local.poison-parent@v1", source = "{self.lua_path(self.parent_spec)}",
       setup = {{ "main" }} }},
}}
"""
        )

    def write_parent(self, fetched_identity: str):
        """Parent whose fetch function writes a child spec declaring `fetched_identity`."""
        self.parent_spec.write_text(
            f"""IDENTITY = "local.poison-parent@v1"
USER_MANAGED = true
SETUP = {{
  main = {{ CHECK = "exit 0", INSTALL = "exit 0" }},
}}
DEPENDENCIES = {{
  {{ spec = "{self.IDENTITY}", source = {{ fetch = function(tmp_dir, options)
      local f = io.open(tmp_dir .. "/spec.lua", "w")
      f:write('IDENTITY = "{fetched_identity}"\\n')
      f:write('USER_MANAGED = true\\n')
      f:write('SETUP = {{ main = {{ CHECK = "exit 0", INSTALL = "exit 0" }} }}\\n')
      f:close()
      envy.commit_fetch({{"spec.lua"}})
  end }} }},
}}
""",
            encoding="utf-8",
        )

    def test_fetch_function_writing_the_wrong_spec_is_not_cached(self):
        self.write_parent(fetched_identity="test.someone-else@v9")

        self.assert_not_finalized(self.IDENTITY, self.install(self.manifest))
        self.assert_not_finalized(self.IDENTITY, self.install(self.manifest))

        self.write_parent(fetched_identity=self.IDENTITY)
        self.assert_finalized(self.IDENTITY, self.install(self.manifest))


if __name__ == "__main__":
    unittest.main()
