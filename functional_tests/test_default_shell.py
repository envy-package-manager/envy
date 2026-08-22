"""Functional coverage for a manifest DEFAULT_SHELL that names an envy-managed
interpreter.

The interpreter here is a shim the provider package installs at bin/myshell: it sets
ENVY_SHIM and re-execs /bin/sh. Any string verb that observes ENVY_SHIM therefore ran
through the manifest shell, and any that does not ran through the platform built-in --
which is what lets one fixture prove both the wiring and its carve-out.

The shim tests are POSIX-only: a Windows custom shell would have to name a system
interpreter (cmd.exe, powershell.exe) as argv[0], so the run could no longer
distinguish the envy-managed path from the built-in one. The declaration and error
paths below are exercised on every platform.
"""

import hashlib
import io
import sys
import tarfile
import unittest
from pathlib import Path

from .env import EnvyTestCase

posix_only = unittest.skipIf(
    sys.platform == "win32", "shim interpreter requires a POSIX shell"
)

# Provider spec. Its own STAGE is a string verb, so it runs under whatever shell the
# manifest hands it -- and it is the package supplying that shell, so the carve-out
# must hand it the built-in. It also records what it saw, for the carve-out test.
SPEC_INTERPRETER = """IDENTITY = "local.ds_interp@v1"
PRODUCTS = {{ myshell = "bin/myshell" }}

FETCH = {{ source = "{ARCHIVE}", sha256 = "{HASH}" }}

STAGE = [[
mkdir -p bin
printf '#!/bin/sh\\nENVY_SHIM=1 exec /bin/sh "$@"\\n' > bin/myshell
chmod +x bin/myshell
echo "${{ENVY_SHIM:-none}}" > shim-seen.txt
]]
"""

# Consumer spec. Its STAGE fails outright unless the manifest shell ran it.
SPEC_CONSUMER = """IDENTITY = "local.ds_consumer@v1"

FETCH = {{ source = "{ARCHIVE}", sha256 = "{HASH}" }}

STAGE = [[
test -n "$ENVY_SHIM" || exit 1
echo ok > marker.txt
]]
"""


class TestDefaultShell(EnvyTestCase):
    envy_watchdog_timeout = 60

    def setUp(self):
        super().setUp()
        self.archive = self.work / "payload.tar.gz"
        self.archive_hash = self._make_archive(self.archive)

    @staticmethod
    def _make_archive(path: Path) -> str:
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode="w:gz") as tar:
            data = b"payload\n"
            info = tarfile.TarInfo("root/file1.txt")
            info.size = len(data)
            tar.addfile(info, io.BytesIO(data))
        blob = buf.getvalue()
        path.write_bytes(blob)
        return hashlib.sha256(blob).hexdigest()

    def _spec(self, name: str, template: str) -> Path:
        return self.write_spec(
            f"{name}.lua",
            template.format(ARCHIVE=self.lua_path(self.archive), HASH=self.archive_hash),
        )

    def _manifest(self, default_shell: str) -> Path:
        interp = self._spec("interp", SPEC_INTERPRETER)
        consumer = self._spec("consumer", SPEC_CONSUMER)
        return self.write_manifest(
            f"""
PACKAGES = {{
  {{ spec = "local.ds_interp@v1", source = "{self.lua_path(interp)}" }},
  {{ spec = "local.ds_consumer@v1", source = "{self.lua_path(consumer)}" }},
}}

{default_shell}
"""
        )

    def _find_one(self, name: str) -> Path:
        hits = list(self.cache_root.rglob(name))
        self.assertEqual(1, len(hits), f"expected exactly one {name}, got {hits}")
        return hits[0]

    # -- the wiring ---------------------------------------------------------

    @posix_only
    def test_product_resolves_managed_interpreter(self):
        """envy.product() in DEFAULT_SHELL names a package installed by DEPENDS."""
        manifest = self._manifest(
            """
DEFAULT_SHELL = {
  DEPENDS = { "local.ds_interp@v1" },
  SHELL = function()
    return { file = { envy.product("myshell") }, ext = ".sh" }
  end,
}
"""
        )
        run = self.install(manifest)
        self.assertEqual(0, run.returncode, run.stderr)

        # The consumer's string verb only gets this far under the shim.
        self.assertEqual("ok\n", self._find_one("marker.txt").read_text())

        access = run.events("lua_ctx_product_access")
        self.assertEqual(1, len(access), access)
        self.assertEqual("envy.DEFAULT_SHELL@v1", access[0].spec)
        self.assertEqual("local.ds_interp@v1", access[0].raw["provider"])
        self.assertTrue(access[0].raw["allowed"])

    @posix_only
    def test_package_resolves_managed_interpreter(self):
        """envy.package() works from DEFAULT_SHELL for the same DEPENDS entry."""
        manifest = self._manifest(
            """
DEFAULT_SHELL = {
  DEPENDS = { "local.ds_interp@v1" },
  SHELL = function()
    return { file = { envy.package("local.ds_interp@v1") .. "/bin/myshell" }, ext = ".sh" }
  end,
}
"""
        )
        run = self.install(manifest)
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertEqual("ok\n", self._find_one("marker.txt").read_text())

    @posix_only
    def test_interpreter_runs_under_builtin_shell(self):
        """The DEPENDS closure supplies the shell, so it cannot consume it."""
        manifest = self._manifest(
            """
DEFAULT_SHELL = {
  DEPENDS = { "local.ds_interp@v1" },
  SHELL = function()
    return { file = { envy.product("myshell") }, ext = ".sh" }
  end,
}
"""
        )
        run = self.install(manifest)
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertEqual("none\n", self._find_one("shim-seen.txt").read_text())

    @posix_only
    def test_depends_completes_before_any_other_string_verb(self):
        """The interpreter is installed before a consumer's first string verb."""
        manifest = self._manifest(
            """
DEFAULT_SHELL = {
  DEPENDS = { "local.ds_interp@v1" },
  SHELL = function()
    return { file = { envy.product("myshell") }, ext = ".sh" }
  end,
}
"""
        )
        run = self.install(manifest)
        self.assertEqual(0, run.returncode, run.stderr)

        def seq(spec, event, phase):
            matching = [
                e for e in run.events(event, spec=spec) if e.raw["phase"] == phase
            ]
            self.assertEqual(1, len(matching), f"{spec} {event} {phase}: {matching}")
            return matching[0].seq

        # The shell is resolved once, and only after DEPENDS lands: the consumer's
        # stage script cannot start before that, and cannot finish after it.
        access = run.events("lua_ctx_product_access")
        self.assertEqual(1, len(access), access)

        interp_installed = seq("local.ds_interp@v1", "phase_complete", "install")
        consumer_staged = seq("local.ds_consumer@v1", "phase_complete", "stage")

        self.assertLess(
            interp_installed,
            access[0].seq,
            "the shell must resolve only after its interpreter is installed",
        )
        self.assertLess(
            access[0].seq,
            consumer_staged,
            "the consumer's stage script must run through the resolved shell",
        )

    @posix_only
    def test_carve_out_reaches_transitive_members(self):
        """A package the interpreter depends on is carved out too, and is here also
        a root in its own right — so it is reached by propagation, not by the
        DEPENDS entry that seeded the closure.

        This does not reproduce the stale-bit race engine::is_default_shell_member
        guards (the interpreter's spec_fetch reliably wires this edge before the
        dependency reaches a string verb); it pins the propagation the guard
        backstops.
        """
        base = self.write_spec(
            "base.lua",
            f"""IDENTITY = "local.ds_base@v1"
PRODUCTS = {{ base_marker = "base-ran.txt" }}

FETCH = {{ source = "{self.lua_path(self.archive)}", sha256 = "{self.archive_hash}" }}

STAGE = [[
echo "${{ENVY_SHIM:-none}}" > base-ran.txt
]]
""",
        )
        interp = self.write_spec(
            "interp_dep.lua",
            f"""IDENTITY = "local.ds_interp@v1"
PRODUCTS = {{ myshell = "bin/myshell" }}

DEPENDENCIES = {{
  {{ spec = "local.ds_base@v1", source = "{self.lua_path(base)}" }},
}}

FETCH = {{ source = "{self.lua_path(self.archive)}", sha256 = "{self.archive_hash}" }}

STAGE = [[
mkdir -p bin
printf '#!/bin/sh\\nENVY_SHIM=1 exec /bin/sh "$@"\\n' > bin/myshell
chmod +x bin/myshell
]]
""",
        )
        consumer = self._spec("consumer", SPEC_CONSUMER)

        manifest = self.write_manifest(
            f"""
PACKAGES = {{
  {{ spec = "local.ds_base@v1", source = "{self.lua_path(base)}" }},
  {{ spec = "local.ds_interp@v1", source = "{self.lua_path(interp)}" }},
  {{ spec = "local.ds_consumer@v1", source = "{self.lua_path(consumer)}" }},
}}

DEFAULT_SHELL = {{
  DEPENDS = {{ "local.ds_interp@v1" }},
  SHELL = function()
    return {{ file = {{ envy.product("myshell") }}, ext = ".sh" }}
  end,
}}
"""
        )

        run = self.install(manifest)
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertEqual("ok\n", self._find_one("marker.txt").read_text())
        # The transitive member is carved out too, whichever way it was reached.
        self.assertEqual("none\n", self._find_one("base-ran.txt").read_text())

    # -- declaration errors -------------------------------------------------

    def test_product_without_depends_is_refused(self):
        """No DEPENDS means no edge to authorize against; say so, do not guess."""
        manifest = self._manifest(
            """
DEFAULT_SHELL = {
  SHELL = function()
    return { file = { envy.product("myshell") }, ext = ".sh" }
  end,
}
"""
        )
        run = self.install(manifest)
        self.assertNotEqual(0, run.returncode)
        self.assertIn("DEFAULT_SHELL function failed", run.stderr)
        self.assertIn("envy.DEFAULT_SHELL@v1", run.stderr)

    def test_unknown_depends_is_refused(self):
        """DEPENDS names packages the manifest declares, as PACKAGE_DEPOTS does."""
        manifest = self._manifest(
            """
DEFAULT_SHELL = {
  DEPENDS = { "local.nonexistent@v9" },
  SHELL = function() return ENVY_SHELL.BASH end,
}
"""
        )
        run = self.install(manifest)
        self.assertNotEqual(0, run.returncode)
        self.assertIn("local.nonexistent@v9", run.stderr)

    def test_weak_reference_in_depends_closure_is_refused(self):
        """DEPENDS is a strong-only closure, on the same grounds as the others.

        Its members are started at target=completion before any worker exists, so
        they reach their own string verbs long before resolve_weak_references() runs
        -- an unresolved reference there is a needed_by violation, or worse a silent
        one, not something the barrier could still satisfy.
        """
        interp = self.write_spec(
            "interp_weak.lua",
            f"""IDENTITY = "local.ds_interp@v1"
PRODUCTS = {{ myshell = "bin/myshell" }}

DEPENDENCIES = {{
  {{ product = "wk", needed_by = "stage" }},
}}

FETCH = {{ source = "{self.lua_path(self.archive)}", sha256 = "{self.archive_hash}" }}
""",
        )
        provider = self.write_spec(
            "wk_provider.lua",
            f"""IDENTITY = "local.ds_wk@v1"
PRODUCTS = {{ wk = "bin/wk" }}

FETCH = {{ source = "{self.lua_path(self.archive)}", sha256 = "{self.archive_hash}" }}
""",
        )
        manifest = self.write_manifest(
            f"""
PACKAGES = {{
  {{ spec = "local.ds_wk@v1", source = "{self.lua_path(provider)}" }},
  {{ spec = "local.ds_interp@v1", source = "{self.lua_path(interp)}" }},
}}

DEFAULT_SHELL = {{
  DEPENDS = {{ "local.ds_interp@v1" }},
  SHELL = function()
    return {{ file = {{ envy.product("myshell") }}, ext = ".sh" }}
  end,
}}
"""
        )

        run = self.install(manifest)
        self.assertNotEqual(0, run.returncode, run.stdout)
        self.assertIn("must use strong dependencies", run.stderr)
        self.assertIn("DEFAULT_SHELL dependency closure", run.stderr)
        self.assertIn("wk", run.stderr)

    @posix_only
    def test_envy_run_inside_the_shell_function(self):
        """The SHELL function may call envy.run(): it gets the platform built-in.

        It is deciding what the manifest shell is, so it cannot be handed the answer
        it has not produced yet -- and asking for one would re-enter the evaluation
        this very call is inside.
        """
        manifest = self._manifest(
            """
DEFAULT_SHELL = {
  DEPENDS = { "local.ds_interp@v1" },
  SHELL = function()
    local shell = envy.product("myshell")
    local r = envy.run("echo probing", { capture = true })
    assert(r.stdout:match("probing"), "expected captured output")
    return { file = { shell }, ext = ".sh" }
  end,
}
"""
        )
        run = self.install(manifest)
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertEqual("ok\n", self._find_one("marker.txt").read_text())

    def test_depends_with_value_shell_is_refused(self):
        """DEPENDS only matters to a function; a value form is read before any
        package exists, so pairing the two is an authoring mistake."""
        manifest = self._manifest(
            """
DEFAULT_SHELL = {
  DEPENDS = { "local.ds_interp@v1" },
  SHELL = ENVY_SHELL.BASH,
}
"""
        )
        run = self.install(manifest)
        self.assertNotEqual(0, run.returncode)
        self.assertIn("DEPENDS requires SHELL to be a function", run.stderr)
