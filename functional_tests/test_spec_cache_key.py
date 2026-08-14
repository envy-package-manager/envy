"""A spec cache entry is keyed on its source, not on its identity alone.

Nothing revalidates a complete cache entry, so if identity were the whole key,
repointing a spec at a new URL, a new git ref, or a new directory would keep
serving yesterday's bytes forever -- the declaration would say one thing and the
cache would answer with another. Each test here redeclares one identity against a
second source and requires envy to fetch it rather than reuse the first entry.
"""

import shutil
import unittest

from .env import EnvyTestCase

SPEC = """IDENTITY = "{identity}"
DEPENDENCIES = {{}}
USER_MANAGED = true
MARKER = "{marker}"
SETUP = {{
  main = {{ CHECK = "exit 0", INSTALL = "exit 0" }},
}}
"""


class TestRemoteSpecCacheKey(EnvyTestCase):
    """Two URLs, one identity."""

    IDENTITY = "test.keyed-remote@v1"

    def setUp(self):
        super().setUp()
        self.serve_dir = self.make_temp_dir("www")
        self.base_url = self.serve_directory(self.serve_dir)
        for name in ("a.lua", "b.lua"):
            (self.serve_dir / name).write_text(
                SPEC.format(identity=self.IDENTITY, marker=name), encoding="utf-8"
            )

    def manifest_for(self, filename: str):
        return self.write_manifest(
            f"""
PACKAGES = {{
    {{ spec = "{self.IDENTITY}", source = "{self.base_url}/{filename}",
       setup = {{ "main" }} }},
}}
"""
        )

    def test_changing_the_url_fetches_again(self):
        first = self.install(self.manifest_for("a.lua"))
        self.assertEqual(0, first.returncode, first.stderr)
        self.assertEqual(1, len(self.spec_entries(self.IDENTITY)))

        second = self.install(self.manifest_for("b.lua"))
        self.assertEqual(0, second.returncode, second.stderr)
        self.assertTrue(
            second.events("cache_miss", spec=self.IDENTITY),
            "a new URL must miss the cache, not reuse the old entry",
        )
        self.assertEqual(2, len(self.spec_entries(self.IDENTITY)))

    def test_returning_to_the_first_url_hits_its_own_entry(self):
        self.install(self.manifest_for("a.lua"))
        self.install(self.manifest_for("b.lua"))

        again = self.install(self.manifest_for("a.lua"))
        self.assertEqual(0, again.returncode, again.stderr)
        self.assertFalse(
            again.events("cache_miss", spec=self.IDENTITY),
            "the original URL still has its own complete entry",
        )
        self.assertEqual(2, len(self.spec_entries(self.IDENTITY)))


class TestBundleSourceCacheKey(EnvyTestCase):
    """Two directories, one bundle identity."""

    IDENTITY = "test.keyed-bundle@v1"
    MEMBER = "test.keyed-member@v1"

    def setUp(self):
        super().setUp()
        self.first = self.make_bundle("first")
        self.second = self.make_bundle("second")

    def make_bundle(self, label: str):
        root = self.make_temp_dir(label)
        (root / "specs").mkdir()
        (root / "envy-bundle.lua").write_text(
            f'BUNDLE = "{self.IDENTITY}"\n'
            f'SPECS = {{ ["{self.MEMBER}"] = "specs/m.lua" }}\n',
            encoding="utf-8",
        )
        (root / "specs" / "m.lua").write_text(
            SPEC.format(identity=self.MEMBER, marker=label), encoding="utf-8"
        )
        return root

    def manifest_for(self, bundle_dir):
        return self.write_manifest(
            f"""
BUNDLES = {{
    tc = {{ identity = "{self.IDENTITY}", source = "{self.lua_path(bundle_dir)}" }},
}}

PACKAGES = {{
    {{ spec = "{self.MEMBER}", bundle = "tc", setup = {{ "main" }} }},
}}
"""
        )

    def test_changing_the_bundle_directory_fetches_again(self):
        first = self.install(self.manifest_for(self.first))
        self.assertEqual(0, first.returncode, first.stderr)
        self.assertEqual(1, len(self.spec_entries(self.IDENTITY)))

        second = self.install(self.manifest_for(self.second))
        self.assertEqual(0, second.returncode, second.stderr)
        self.assertTrue(
            second.events("cache_miss", spec=self.IDENTITY),
            "a new bundle directory must miss the cache",
        )
        self.assertEqual(2, len(self.spec_entries(self.IDENTITY)))


@unittest.skipUnless(shutil.which("git"), "git is not on PATH")
class TestGitSpecCacheKey(EnvyTestCase):
    """Two refs of one repo, one identity.

    A filesystem path ending in `.git` is a git source (uri_classify routes any
    `.git` suffix to the git scheme), so this needs no server. The ref is part of
    the cache key: pinning a new ref must fetch it, not hand back the old checkout.
    """

    IDENTITY = "test.keyed-git@v1"

    def setUp(self):
        super().setUp()
        self.repo = self.make_git_repo(
            {"spec.lua": SPEC.format(identity=self.IDENTITY, marker="v1")}, "gitspec"
        )
        self.git("tag", "v1", cwd=self.repo)
        self.commit_files(
            self.repo,
            {"spec.lua": SPEC.format(identity=self.IDENTITY, marker="v2")},
            "second",
        )
        self.git("tag", "v2", cwd=self.repo)

    def manifest_for(self, ref: str):
        return self.write_manifest(
            f"""
PACKAGES = {{
    {{ spec = "{self.IDENTITY}", source = "{self.lua_path(self.repo)}",
       ref = "{ref}", setup = {{ "main" }} }},
}}
"""
        )

    def test_changing_the_ref_fetches_again(self):
        first = self.install(self.manifest_for("v1"))
        self.assertEqual(0, first.returncode, first.stderr)
        self.assertEqual(1, len(self.spec_entries(self.IDENTITY)))

        second = self.install(self.manifest_for("v2"))
        self.assertEqual(0, second.returncode, second.stderr)
        self.assertTrue(
            second.events("cache_miss", spec=self.IDENTITY),
            "a new ref must miss the cache, not reuse the old checkout",
        )
        self.assertEqual(2, len(self.spec_entries(self.IDENTITY)))

    def test_same_ref_hits_the_cache(self):
        self.install(self.manifest_for("v1"))

        again = self.install(self.manifest_for("v1"))
        self.assertEqual(0, again.returncode, again.stderr)
        self.assertFalse(again.events("cache_miss", spec=self.IDENTITY))
        self.assertEqual(1, len(self.spec_entries(self.IDENTITY)))


if __name__ == "__main__":
    unittest.main()
