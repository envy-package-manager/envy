"""`envy state` lists and cleans .envy/state/<ns.name> entries, read-only trees included;
a whole-manifest install or sync names the unused ones."""

from __future__ import annotations

import os
import stat
import sys
import unittest
from pathlib import Path

from . import test_config
from .env import EnvyTestCase

WINDOWS = sys.platform == "win32"
RUNNING_AS_ROOT = not WINDOWS and os.geteuid() == 0
LAUNCHER = r".\envy-bin\envy.bat" if WINDOWS else "./envy-bin/envy"

# `toolbin` differs from its package's name, so a query reaches it through the product
# registry.
_TOOL = """IDENTITY = "local.tool@v1"
FETCH = function() end
INSTALL = function(install_dir) envy.write(install_dir .. "/tool.txt", "x") end
PRODUCTS = { toolbin = { value = "tool.txt", script = false } }
ENV = function(pkg_dir, pkg_state_dir) return { TOOL_HOME = pkg_state_dir } end
"""

_RO_FILE = stat.S_IRUSR | stat.S_IRGRP | stat.S_IROTH
_RO_DIR = _RO_FILE | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH


def _spec(identity: str, deps: str = "") -> str:
    return (
        f'IDENTITY = "{identity}"\n'
        + (f"DEPENDENCIES = {{ {deps} }}\n" if deps else "")
        + "FETCH = function() end\nINSTALL = function() end\n"
    )


def _non_host_platform() -> str:
    return "windows" if not WINDOWS else "linux"


def _link_dir(link: Path, target: Path) -> None:
    """A directory link: a junction on Windows, which any user may create."""
    if WINDOWS:
        import _winapi

        _winapi.CreateJunction(str(target), str(link))
    else:
        link.symlink_to(target, target_is_directory=True)


def _writable(root: Path) -> None:
    """Restores owner rwx below `root` so the scratch dir's own cleanup takes it."""
    for dirpath, dirnames, filenames in os.walk(root):
        for name in dirnames + filenames:
            p = Path(dirpath) / name
            if not p.is_symlink():
                os.chmod(p, stat.S_IRWXU)


class TestState(EnvyTestCase):
    def setUp(self) -> None:
        super().setUp()
        self.state = self.work / ".envy" / "state"
        self.addCleanup(_writable, self.work)

    # -- fixtures -----------------------------------------------------------

    def manifest(self, *entries: str, header: str = "") -> Path:
        body = "PACKAGES = {\n" + "\n".join(entries) + "\n}\n"
        if not header:
            return self.write_manifest(body)
        path = self.work / "envy.lua"
        path.write_text(test_config.MANIFEST_HEADER + header + body, encoding="utf-8")
        return path

    def tool_manifest(self, *more: str, header: str = "") -> Path:
        tool = self.write_spec("tool.lua", _TOOL)
        return self.manifest(
            test_config.spec_entry("local.tool@v1", tool), *more, header=header
        )

    def non_host_entry(self, identity: str, deps: str = "") -> str:
        spec = self.write_spec(f"{identity}.lua", _spec(identity, deps))
        return (
            f'  {{ spec = "{identity}", source = "{spec.as_posix()}",'
            f' platforms = {{ "{_non_host_platform()}" }} }},'
        )

    def state_dir(self, key: str, size: int = 16) -> Path:
        d = self.state / key
        d.mkdir(parents=True, exist_ok=True)
        (d / "blob").write_bytes(b"\0" * size)
        return d

    def go_like(self, key: str) -> Path:
        """A state dir holding a tree as Go leaves its module cache: files 0444, dirs 0555."""
        d = self.state_dir(key)
        mod = d / "pkg" / "mod" / "x@v1"
        mod.mkdir(parents=True)
        (mod / "a.go").write_text("package x\n", encoding="utf-8")
        os.chmod(mod / "a.go", _RO_FILE)
        for p in (mod, mod.parent, mod.parent.parent):
            os.chmod(p, _RO_DIR)
        return d

    def in_project(self, *args: str | Path):
        return self.run_envy("--project", self.work, *args)

    def ok(self, *args: str | Path):
        r = self.in_project(*args)
        self.assertEqual(r.returncode, 0, f"stdout:\n{r.stdout}\nstderr:\n{r.stderr}")
        return r

    def fails(self, *args: str | Path, needle: str):
        r = self.in_project(*args)
        self.assertNotEqual(r.returncode, 0, r.stderr)
        self.assertIn(needle, r.stderr)
        return r

    def listing(self) -> tuple[str, list[tuple[str, bool]]]:
        """`envy state` as (root, [(name, unused)] in printed order)."""
        out = self.ok("state").stdout
        root, rows = "", []
        for line in out.splitlines():
            if line.startswith("State: "):
                root = line[len("State: ") :]
            elif line.startswith("  ") and line.split()[0] not in ("TOTAL", "(none)"):
                rows.append((line.split()[0], line.endswith("(unused)")))
        return root, rows

    @staticmethod
    def unused(stderr: str) -> list[str] | None:
        """Names from the unused-state line in stderr, else None."""
        for line in stderr.splitlines():
            if "Unused package state: " in line:
                names = line.split("Unused package state: ", 1)[1].split(" (", 1)[0]
                return names.split(", ")
        return None

    # -- envy state ---------------------------------------------------------

    def test_lists_largest_first_and_flags_the_unused(self):
        self.tool_manifest()
        self.state_dir("acme.b")
        self.state_dir("local.tool", 4096)
        self.state_dir("acme.a")

        root, rows = self.listing()

        self.assertEqual(os.path.realpath(root), os.path.realpath(self.state))
        self.assertEqual(rows, [("local.tool", False), ("acme.a", True), ("acme.b", True)])

    def test_lists_none_without_state(self):
        self.tool_manifest()
        self.assertIn("(none)", self.ok("state").stdout)

    def test_another_platforms_package_and_dependency_are_unused_here(self):
        dep = self.write_spec("dep.lua", _spec("local.dep@v1"))
        self.tool_manifest(
            self.non_host_entry(
                "local.other@v1", f'{{ spec = "local.dep@v1", source = "{dep.as_posix()}" }}'
            )
        )
        self.state_dir("local.other")
        self.state_dir("local.dep")

        _, rows = self.listing()

        self.assertEqual(rows, [("local.dep", True), ("local.other", True)])

    def test_a_weak_reference_binds_as_install_binds_it(self):
        """Another platform's match stays out of the graph, so the host fallback is used."""
        fb = self.write_spec("fb.lua", _spec("local.toolfb@v1"))
        consumer = self.write_spec("consumer.lua", _spec(
            "local.consumer@v1",
            f'{{ spec = "local.tool", weak = {{ spec = "local.toolfb@v1",'
            f' source = "{fb.as_posix()}" }} }}',
        ))
        self.manifest(test_config.spec_entry("local.consumer@v1", consumer),
                      self.non_host_entry("local.tool@v1"))
        self.state_dir("local.toolfb")

        _, rows = self.listing()

        self.assertEqual(rows, [("local.toolfb", False)])

    def test_links_and_files_list_by_their_own_size(self):
        self.tool_manifest()
        elsewhere = self.make_temp_dir("elsewhere")
        (elsewhere / "big").write_bytes(b"\0" * 9000)
        self.state.mkdir(parents=True)
        _link_dir(self.state / "acme.link", elsewhere)
        (self.state / "acme.file").write_bytes(b"\0" * 300)

        out = self.ok("state").stdout
        _, rows = self.listing()

        self.assertEqual(rows, [("acme.file", True), ("acme.link", True)])
        self.assertRegex(out, r"acme\.file\s+300B  \(file\)  \(unused\)")
        self.assertRegex(out, r"acme\.link\s+0B  \(link\)  \(unused\)")
        self.assertRegex(out, r"TOTAL\s+300B")

    def test_a_case_variant_dir_counts_as_used_on_a_case_blind_filesystem(self):
        (self.work / "CASE").touch()
        if not (self.work / "case").exists():
            self.skipTest("case-sensitive filesystem")
        self.tool_manifest()
        self.state_dir("local.Tool")

        _, rows = self.listing()

        self.assertEqual(rows, [("local.Tool", False)])

    def test_a_case_variant_dir_is_unused_on_a_case_sensitive_filesystem(self):
        (self.work / "CASE").touch()
        if (self.work / "case").exists():
            self.skipTest("case-insensitive filesystem")
        self.tool_manifest()
        self.state_dir("local.Tool")

        _, rows = self.listing()

        self.assertEqual(rows, [("local.Tool", True)])

    @unittest.skipIf(WINDOWS or RUNNING_AS_ROOT, "POSIX read permission")
    def test_an_unreadable_state_root_is_reported(self):
        self.tool_manifest()
        self.state_dir("acme.gone")
        os.chmod(self.state, stat.S_IWUSR | stat.S_IXUSR)

        self.fails("state", needle="cannot read")
        r = self.ok("install")

        os.chmod(self.state, stat.S_IRWXU)
        self.assertIn("cannot read", r.stderr)

    def test_state_dir_or_cache_local_inside_state_is_an_error(self):
        """The directives apply under the default root, sandboxed here."""
        env = test_config.sandbox_home_env(self.make_temp_dir("home"))
        for directive in ('-- @envy state-dir ".envy/state"\n',
                          '-- @envy cache-local ".envy/state/cache"\n'):
            with self.subTest(directive=directive):
                self.tool_manifest(header=directive)
                r = test_config.run([str(self.envy), "--project", str(self.work), "state"],
                                    capture_output=True, text=True, env=env)
                self.assertNotEqual(r.returncode, 0, r.stderr)
                self.assertIn("is inside", r.stderr)

    def test_clean_checks_the_directives_under_a_cache_root_override(self):
        self.tool_manifest(header='-- @envy cache-local ".envy/state/cache"\n')
        cache = self.state / "cache"
        cache.mkdir(parents=True)

        self.fails("state", "clean", needle="is inside")
        self.fails("state", "clean", "cache", needle="is inside")
        self.assertTrue(cache.exists())

    # -- envy state clean <query> -------------------------------------------

    @unittest.skipIf(RUNNING_AS_ROOT, "root writes through read-only modes")
    def test_clean_by_product_removes_a_read_only_tree(self):
        self.tool_manifest()
        tool = self.go_like("local.tool")
        self.assertFalse(os.access(tool / "pkg" / "mod" / "x@v1" / "a.go", os.W_OK))
        kept = self.state_dir("acme.gone")

        r = self.ok("state", "clean", "toolbin")

        self.assertFalse(tool.exists())
        self.assertTrue(kept.exists())
        self.assertPathContains(r.stderr, "Removed ")
        self.assertPathContains(r.stderr, ".envy/state/local.tool")

    def test_clean_by_identity_and_by_bare_name(self):
        self.tool_manifest()
        for query in ("local.tool@v1", "tool", "local.tool"):
            with self.subTest(query=query):
                d = self.go_like("local.tool")
                self.ok("state", "clean", query)
                self.assertFalse(d.exists())

    def test_repeated_queries_for_one_key_remove_it_once(self):
        self.tool_manifest()
        self.state_dir("local.tool")

        r = self.ok("state", "clean", "tool", "local.tool", "toolbin")

        self.assertEqual(r.stderr.count("Removed "), 1)

    def test_clean_reaches_an_orphan_by_its_entry_name(self):
        self.tool_manifest()
        gone = self.go_like("acme.gone")

        self.ok("state", "clean", "acme.gone")

        self.assertFalse(gone.exists())

    @unittest.skipIf(WINDOWS or RUNNING_AS_ROOT, "POSIX search permission")
    def test_an_unsearchable_state_root_lists_as_unreadable_and_cleans(self):
        self.tool_manifest()
        self.state_dir("local.tool")
        os.chmod(self.state, stat.S_IRUSR | stat.S_IWUSR)

        self.fails("state", needle="cannot read")
        self.ok("state", "clean", "tool")

        self.assertFalse((self.state / "local.tool").exists())

    def test_clean_without_state_says_so(self):
        self.tool_manifest()
        self.assertIn("local.tool has no state", self.ok("state", "clean", "tool").stderr)

    def test_an_unmatched_query_stops_the_whole_clean(self):
        self.tool_manifest()
        tool = self.state_dir("local.tool")

        self.fails(
            "state", "clean", "tool", "nope",
            needle="query 'nope' matches no package, product or state entry",
        )
        self.assertTrue(tool.exists(), "the queries before the typo keep their state")

    def test_queries_stay_inside_state(self):
        self.tool_manifest()
        self.state_dir("local.tool")
        victim = self.work / "keep.txt"
        victim.write_text("keep", encoding="utf-8")

        for query in ("..", "../..", "../keep.txt", "."):
            with self.subTest(query=query):
                self.fails("state", "clean", query, needle="matches no package")
        self.assertTrue(victim.exists())
        self.assertTrue((self.state / "local.tool").exists())

    def test_an_ambiguous_name_is_an_error(self):
        go_a = self.write_spec("a.lua", _spec("acme.go@v1"))
        go_b = self.write_spec("b.lua", _spec("local.go@v1"))
        self.manifest(
            test_config.spec_entry("acme.go@v1", go_a),
            test_config.spec_entry("local.go@v1", go_b),
        )
        a, b = self.state_dir("acme.go"), self.state_dir("local.go")

        self.fails("state", "clean", "go",
                   needle="query 'go' is ambiguous: acme.go, local.go")
        self.assertTrue(a.exists() and b.exists())

    def test_a_product_and_an_entry_sharing_a_name_is_ambiguous(self):
        self.tool_manifest()
        tool, orphan = self.state_dir("local.tool"), self.state_dir("toolbin")

        self.fails("state", "clean", "toolbin",
                   needle="query 'toolbin' is ambiguous: local.tool, toolbin")
        self.assertTrue(tool.exists() and orphan.exists())

    def test_a_package_key_outside_state_is_an_error(self):
        """A bundle spec's IDENTITY reaches the engine unvalidated; clean checks its key."""
        bundle = self.make_temp_dir("bundle")
        (bundle / "specs").mkdir()
        (bundle / "envy-bundle.lua").write_text(
            'BUNDLE = "test.tc@v1"\nSPECS = { ["../../victim.x@v1"] = "specs/x.lua" }\n',
            encoding="utf-8")
        (bundle / "specs" / "x.lua").write_text(_spec("../../victim.x@v1"), encoding="utf-8")
        app = self.write_spec("app.lua", _spec(
            "local.app@v1", '{ spec = "../../victim.x@v1", bundle = "tc" }'
        ).replace("DEPENDENCIES", f'BUNDLES = {{ tc = {{ identity = "test.tc@v1",'
                                  f' source = "{bundle.as_posix()}" }} }}\nDEPENDENCIES'))
        self.manifest(test_config.spec_entry("local.app@v1", app))
        self.state.mkdir(parents=True)
        victim = self.work / "victim.x"
        victim.mkdir()

        self.fails("state", "clean", "../../victim.x@v1", needle="outside .envy/state")
        self.assertTrue(victim.exists())

    def test_a_non_ascii_orphan_cleans_by_name(self):
        self.tool_manifest()
        orphan = self.state_dir("acme.caf\u00e9")

        self.ok("state", "clean", "acme.caf\u00e9")

        self.assertFalse(orphan.exists())

    def test_a_shared_dir_names_every_user(self):
        tool = self.write_spec("tool.lua", _spec("local.tool@v1"))  # two variants, one key
        self.manifest(
            test_config.spec_entry("local.tool@v1", tool, options='{ v = "1" }'),
            test_config.spec_entry("local.tool@v1", tool, options='{ v = "2" }'),
        )
        d = self.state_dir("local.tool")

        r = self.ok("state", "clean", "local.tool")

        self.assertIn(
            '(shared by local.tool@v1{["v"]="1"}, local.tool@v1{["v"]="2"})', r.stderr
        )
        self.assertFalse(d.exists())

    def test_clean_unlinks_a_linked_entry_in_place(self):
        self.tool_manifest()
        outside = self.make_temp_dir("outside")
        (outside / "precious").write_text("x", encoding="utf-8")
        self.state.mkdir(parents=True)
        _link_dir(self.state / "local.tool", outside)
        d = self.state_dir("acme.gone")
        _link_dir(d / "inner", outside)

        self.ok("state", "clean", "tool", "acme.gone")

        self.assertFalse(os.path.lexists(self.state / "local.tool"))
        self.assertFalse(d.exists())
        self.assertTrue((outside / "precious").exists())

    def test_clean_refuses_a_linked_state_root(self):
        outside = self.make_temp_dir("outside")
        (outside / "local.tool").mkdir()
        for linked in (self.work / ".envy", self.state):
            with self.subTest(linked=linked.name):
                linked.parent.mkdir(parents=True, exist_ok=True)
                _link_dir(linked, outside)
                self.tool_manifest()
                self.fails("state", "clean", needle="is a link")
                self.fails("state", "clean", "tool", needle="is a link")
                self.assertTrue((outside / "local.tool").exists())
                os.unlink(linked) if not WINDOWS else os.rmdir(linked)

    @unittest.skipIf(WINDOWS or RUNNING_AS_ROOT, "POSIX search permission")
    def test_clean_refuses_a_linked_state_root_under_a_search_only_parent(self):
        self.tool_manifest()
        outside = self.make_temp_dir("outside")
        (outside / "local.tool").mkdir()
        envy = self.work / ".envy"
        envy.mkdir()
        _link_dir(self.state, outside)
        os.chmod(envy, stat.S_IXUSR)

        self.fails("state", "clean", needle="is a link")

        os.chmod(envy, stat.S_IRWXU)
        self.assertTrue((outside / "local.tool").exists())

    def test_clean_refuses_a_linked_state_root_spelled_in_another_case(self):
        (self.work / "CASE").touch()
        if not (self.work / "case").exists():
            self.skipTest("case-sensitive filesystem")
        self.tool_manifest()
        outside = self.make_temp_dir("outside")
        (outside / "local.tool").mkdir()
        _link_dir(self.work / ".Envy", outside)

        self.fails("state", "clean", needle="is a link")
        self.assertTrue((outside / "local.tool").exists())

    @unittest.skipIf(WINDOWS or RUNNING_AS_ROOT, "POSIX write permission")
    def test_clean_empties_a_read_only_state_root(self):
        self.tool_manifest()
        for args in (("state", "clean"), ("state", "clean", "tool", "acme.gone")):
            with self.subTest(args=args):
                self.state_dir("local.tool")
                self.state_dir("acme.gone")
                os.chmod(self.state, _RO_DIR)

                self.ok(*args)

                self.assertFalse((self.state / "local.tool").exists())
                self.assertFalse((self.state / "acme.gone").exists())

    # -- envy state clean ---------------------------------------------------

    def test_bare_clean_reads_only_the_header(self):
        """The bare form reads only the @envy header, so a broken manifest still resets."""
        self.write_manifest('error("manifest is broken")\n')
        self.go_like("local.tool")
        self.state_dir("acme.gone")

        r = self.ok("state", "clean")

        self.assertFalse(self.state.exists())
        self.assertTrue((self.work / ".envy").exists(), ".envy itself stays")
        self.assertPathContains(r.stderr, ".envy/state/acme.gone")
        self.assertPathContains(r.stderr, ".envy/state/local.tool")

    def test_bare_clean_without_state_says_so(self):
        self.tool_manifest()
        self.assertIn("No package state in", self.ok("state", "clean").stderr)

    # -- the report after install and sync ----------------------------------

    def test_install_reports_unused_state_and_keeps_it(self):
        self.tool_manifest()
        used, gone = self.state_dir("local.tool"), self.state_dir("acme.gone")

        r = self.ok("install")

        self.assertEqual(self.unused(r.stderr), ["acme.gone"])
        self.assertIn(f"({LAUNCHER} state clean <name> removes one)", r.stderr)
        self.assertTrue(used.exists() and gone.exists())

    def test_install_and_sync_report_another_platforms_state_as_unused(self):
        dep = self.write_spec("dep.lua", _spec("local.dep@v1"))
        self.tool_manifest(
            self.non_host_entry(
                "local.other@v1", f'{{ spec = "local.dep@v1", source = "{dep.as_posix()}" }}'
            )
        )
        self.state_dir("local.other")
        self.state_dir("local.dep")

        for cmd in ("install", "sync"):
            with self.subTest(cmd=cmd):
                unused = self.unused(self.ok(cmd).stderr)
                self.assertEqual(unused, ["local.dep", "local.other"])

    def test_sync_reports_unused_state(self):
        self.tool_manifest()
        self.state_dir("acme.gone")

        self.assertEqual(self.unused(self.ok("sync").stderr), ["acme.gone"])

    def test_partial_runs_skip_the_report(self):
        """A query, --manifest or --subproject resolves a graph other than the top's."""
        m = self.tool_manifest()
        self.state_dir("acme.gone")

        for args in (("install", "local.tool"), ("sync", "local.tool"),
                     ("install", "--manifest", m), ("sync", "--manifest", m)):
            with self.subTest(args=args):
                self.assertIsNone(self.unused(self.ok(*args).stderr))

        r = self.run_envy("sync", "--subproject", cwd=self.work)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIsNone(self.unused(r.stderr))

    def test_quiet_install_skips_the_report(self):
        self.tool_manifest()
        self.state_dir("acme.gone")

        r = self.run_envy("-q", "--project", self.work, "install")

        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIsNone(self.unused(r.stderr))


if __name__ == "__main__":
    unittest.main()
