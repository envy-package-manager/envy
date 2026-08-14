"""Scenario-level tests for the logging/trace observability behaviors.

Complements test_trace_schema.py (registry contract) and test_trace_parser.py
(parser unit tests) by driving real runs and asserting:
  - per-package outcome: log line (INFO, non-TTY) + pkg_outcome trace event,
    across installed / cache_hit
  - download_start/complete on a successful fetch, download_failed on a bad source
  - product_resolved when a product dependency resolves
  - deploy_script on `sync` with product deployment enabled
  - -q/--quiet suppresses the INFO outcome line; --verbose surfaces the DEBUG
    decision narrative
  - undecorated warnings/errors are compiler-style tagged
"""

import sys
import unittest
from pathlib import Path

from . import test_config
from .env import EnvyTestCase
from .trace_parser import TraceParser


def lua_path(p: Path) -> str:
    return p.as_posix()


class TestObservability(EnvyTestCase):
    def setUp(self):
        super().setUp()
        self.test_dir = self.make_temp_dir("test_dir")

        self.payload = self.test_dir / "payload.txt"
        self.payload.write_text("hello\n", encoding="utf-8")

        self.spec = self.test_dir / "obs.lua"
        self.spec.write_text(
            f'IDENTITY = "local.obs@v1"\n'
            f'FETCH = {{ source = "file://{lua_path(self.payload)}" }}\n',
            encoding="utf-8",
        )
        self.manifest = self._manifest("obs", "local.obs@v1", self.spec)

    def _manifest(self, name: str, identity: str, spec: Path) -> Path:
        """One manifest per subdir; write_spec_manifest always names it envy.lua."""
        d = self.test_dir / name
        d.mkdir(exist_ok=True)
        return test_config.write_spec_manifest(d, [(identity, spec)])

    def _install(self, *extra, trace_file: Path | None = None):
        cmd = [str(self.envy), f"--cache-root={self.cache_root}"]
        if trace_file is not None:
            cmd.append(f"--trace=file:{trace_file}")
        cmd.extend(extra)
        cmd.extend(["install", "--manifest", str(self.manifest)])
        return test_config.run(cmd, capture_output=True, text=True)

    # --- outcomes -----------------------------------------------------------

    def test_outcome_installed_then_cache_hit(self):
        """pkg_outcome + INFO line report installed on build, cache_hit on rerun."""
        trace1 = self.cache_root / "t1.jsonl"
        r1 = self._install(trace_file=trace1)
        self.assertEqual(r1.returncode, 0, r1.stderr)

        # Non-TTY INFO outcome line (capture_output pipes stderr, so no TTY).
        self.assertRegex(r1.stderr, r"\[local\.obs@v1\] installed \(\d")

        outcomes1 = TraceParser(trace1).filter_by_event("pkg_outcome")
        self.assertEqual(len(outcomes1), 1)
        self.assertEqual(outcomes1[0].spec, "local.obs@v1")
        self.assertEqual(outcomes1[0].raw["outcome"], "installed")
        self.assertGreaterEqual(outcomes1[0].raw["duration_ms"], 0)

        trace2 = self.cache_root / "t2.jsonl"
        r2 = self._install(trace_file=trace2)
        self.assertEqual(r2.returncode, 0, r2.stderr)
        self.assertIn("[local.obs@v1] cache hit", r2.stderr)

        outcomes2 = TraceParser(trace2).filter_by_event("pkg_outcome")
        self.assertEqual(len(outcomes2), 1)
        self.assertEqual(outcomes2[0].raw["outcome"], "cache_hit")

    def test_quiet_suppresses_outcome_line(self):
        """-q keeps warnings/errors only; the INFO outcome line is gone."""
        r = self._install("-q")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertNotIn("installed", r.stderr)
        self.assertNotIn("cache hit", r.stderr)

    def test_verbose_surfaces_decision_narrative(self):
        """--verbose shows the per-package DEBUG narrative, [identity]-prefixed."""
        r = self._install("--verbose")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("[local.obs@v1] check: miss", r.stderr)
        self.assertIn("[local.obs@v1] fetch: downloading", r.stderr)

    # --- download events ----------------------------------------------------

    def test_download_start_and_complete(self):
        trace = self.cache_root / "t.jsonl"
        r = self._install(trace_file=trace)
        self.assertEqual(r.returncode, 0, r.stderr)

        parser = TraceParser(trace)
        starts = parser.filter_by_event("download_start")
        completes = parser.filter_by_event("download_complete")
        self.assertEqual(len(starts), 1)
        self.assertEqual(len(completes), 1)
        self.assertEqual(completes[0].spec, "local.obs@v1")
        self.assertGreaterEqual(completes[0].raw["bytes"], 0)

    def test_download_failed_on_bad_source(self):
        """A missing fetch source emits download_failed and an error: line."""
        bad_spec = self.test_dir / "bad.lua"
        missing = self.test_dir / "does-not-exist.txt"
        bad_spec.write_text(
            f'IDENTITY = "local.obsbad@v1"\n'
            f'FETCH = {{ source = "file://{lua_path(missing)}" }}\n',
            encoding="utf-8",
        )
        manifest = self._manifest("bad", "local.obsbad@v1", bad_spec)
        trace = self.cache_root / "bad.jsonl"
        r = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                f"--trace=file:{trace}",
                "install",
                "--manifest",
                str(manifest),
            ],
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(r.returncode, 0)
        # Undecorated error lines are compiler-style tagged.
        self.assertIn("error:", r.stderr)

        failed = TraceParser(trace).filter_by_event("download_failed")
        self.assertGreaterEqual(len(failed), 1)
        self.assertTrue(all(e.raw.get("error") for e in failed))

    # --- product resolution -------------------------------------------------

    @unittest.skipIf(sys.platform == "win32", "shell INSTALL uses POSIX mkdir/echo")
    def test_product_resolved_event(self):
        """A weak product reference resolves from the registry → product_resolved.

        The weak form (`{ product = "tool" }` with no spec) is resolved during
        graph resolution against the product registry, which is what emits the
        event; the explicit `{ product, spec, source }` form wires directly and
        does not. Both packages live in one manifest and install together.
        """
        provider = self.test_dir / "provider.lua"
        provider.write_text(
            'IDENTITY = "local.provider@v1"\n'
            'PRODUCTS = { tool = "bin/tool" }\n'
            "function FETCH(tmp_dir, options) end\n"
            "function INSTALL(install_dir, stage_dir, fetch_dir, tmp_dir, options)\n"
            "  envy.run(\"mkdir -p '\" .. install_dir .. \"/bin' && echo x > '\"\n"
            "          .. install_dir .. \"/bin/tool'\", { quiet = true })\n"
            "end\n",
            encoding="utf-8",
        )
        consumer = self.test_dir / "consumer.lua"
        consumer.write_text(
            'IDENTITY = "local.consumer@v1"\n'
            "DEPENDENCIES = {\n"
            '  { product = "tool", needed_by = "install" },\n'  # weak: no spec
            "}\n"
            "function FETCH(tmp_dir, options) end\n"
            "function INSTALL(install_dir, stage_dir, fetch_dir, tmp_dir, options)\n"
            '  envy.product("tool")\n'
            "end\n",
            encoding="utf-8",
        )
        manifest = self.test_dir / "envy.lua"
        manifest.write_text(
            '-- @envy bin "envy-bin"\n'
            "PACKAGES = {\n"
            f'  {{ spec = "local.provider@v1", source = "{lua_path(provider)}" }},\n'
            f'  {{ spec = "local.consumer@v1", source = "{lua_path(consumer)}" }},\n'
            "}\n",
            encoding="utf-8",
        )
        # sync resolves every manifest package, so the provider registers its
        # products before the consumer's weak "tool" reference resolves.
        trace = self.cache_root / "prod.jsonl"
        r = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                f"--trace=file:{trace}",
                "sync",
                "--manifest",
                str(manifest),
            ],
            capture_output=True,
            text=True,
            cwd=self.test_dir,
        )
        self.assertEqual(r.returncode, 0, r.stderr)

        resolved = TraceParser(trace).filter_by_event("product_resolved")
        tool = [e for e in resolved if e.raw.get("product") == "tool"]
        self.assertTrue(tool, f"no product_resolved for 'tool': {resolved}")
        self.assertEqual(tool[0].raw["provider"], "local.provider@v1")
        self.assertIn(tool[0].raw["via"], ("identity", "registry", "fallback"))


class TestDeployObservability(EnvyTestCase):
    """deploy_script events require a full `sync` (deploy is command-level)."""

    def setUp(self):
        super().setUp()
        self.project = self.make_temp_dir("project")

        spec = self.project / "tool.lua"
        spec.write_text(
            'IDENTITY = "local.deploytool@v1"\n'
            'PRODUCTS = { greet = "bin/greet" }\n'
            "function FETCH(tmp_dir, options) end\n"
            "function INSTALL(install_dir, stage_dir, fetch_dir, tmp_dir, options)\n"
            "  envy.run(\"mkdir -p '\" .. install_dir .. \"/bin' && echo x > '\"\n"
            "          .. install_dir .. \"/bin/greet'\", { quiet = true })\n"
            "end\n",
            encoding="utf-8",
        )
        (self.project / "envy.lua").write_text(
            '-- @envy bin "envy-bin"\n'
            '-- @envy deploy "true"\n'
            f'PACKAGES = {{ {{ spec = "local.deploytool@v1", '
            f'source = "{lua_path(spec)}" }} }}\n',
            encoding="utf-8",
        )

    @unittest.skipIf(sys.platform == "win32", "shell INSTALL uses POSIX mkdir/echo")
    def test_deploy_script_event(self):
        trace = self.cache_root / "deploy.jsonl"
        r = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                f"--trace=file:{trace}",
                "sync",
                "--manifest",
                str(self.project / "envy.lua"),
            ],
            capture_output=True,
            text=True,
            cwd=self.project,
        )
        self.assertEqual(r.returncode, 0, r.stderr)

        events = TraceParser(trace).filter_by_event("deploy_script")
        greet = [e for e in events if e.raw.get("product") == "greet"]
        self.assertTrue(greet, f"no deploy_script for 'greet': {events}")
        self.assertIn(
            greet[0].raw["action"], ("created", "updated", "unchanged", "removed")
        )


if __name__ == "__main__":
    unittest.main()
