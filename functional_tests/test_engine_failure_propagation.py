"""Functional tests for engine failure propagation under parallelism.

A mid-graph package failure must fail all dependents deterministically and the
engine must exit cleanly - no hangs, no stuck worker threads. Exercises the
fail_all_contexts / condition-variable paths that only trigger when many package
threads are in flight simultaneously.
"""

from pathlib import Path
import unittest

from . import test_config
from .env import EnvyTestCase

MID_COUNT = 20
FAILING_MID = 7


class TestEngineFailurePropagation(EnvyTestCase):
    """Mid-graph failure in a wide diamond graph: all dependents fail, no hang."""

    def setUp(self):
        super().setUp()
        self.specs_dir = self.make_temp_dir("specs_dir")

    def write_spec(self, name: str, content: str) -> Path:
        path = self.specs_dir / name
        path.write_text(content, encoding="utf-8")
        return path

    def build_diamond(self):
        """root -> mid_0..mid_N -> (leaf_a, leaf_b); one mid fails at INSTALL."""
        for leaf in ("a", "b"):
            self.write_spec(
                f"leaf_{leaf}.lua",
                f"""IDENTITY = "local.leaf-{leaf}@v1"
USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options) return false end,
    INSTALL = function(pkg_dir, options) end,
  }},
}}

""",
            )

        for i in range(MID_COUNT):
            fail_stmt = (
                f'error("mid-{i} deliberate failure")' if i == FAILING_MID else ""
            )
            self.write_spec(
                f"mid_{i}.lua",
                f"""IDENTITY = "local.mid-{i}@v1"
DEPENDENCIES = {{
  {{ spec = "local.leaf-a@v1", source = "leaf_a.lua", setup = {{ "main" }} }},
  {{ spec = "local.leaf-b@v1", source = "leaf_b.lua", setup = {{ "main" }} }},
}}
USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options) return false end,
    INSTALL = function(pkg_dir, options)
      local t = os.clock()
      while os.clock() - t < 0.05 do end
      {fail_stmt}
    end,
  }},
}}

""",
            )

        mid_deps = ",\n  ".join(
            f'{{ spec = "local.mid-{i}@v1", source = "mid_{i}.lua", setup = {{ "main" }} }}'
            for i in range(MID_COUNT)
        )
        self.write_spec(
            "root.lua",
            f"""IDENTITY = "local.root@v1"
DEPENDENCIES = {{
  {mid_deps}
}}
USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options) return false end,
    INSTALL = function(pkg_dir, options) end,
  }},
}}

""",
        )

    def test_mid_graph_failure_fails_dependents_without_hang(self):
        self.build_diamond()
        manifest = test_config.write_spec_manifest(
            self.specs_dir, [("local.root@v1", self.specs_dir / "root.lua")]
        )

        # Repeat to shake out scheduling-dependent races; each run spawns
        # MID_COUNT+3 package threads. Harness timeout catches hangs.
        for iteration in range(3):
            with self.subTest(iteration=iteration):
                result = test_config.run(
                    [
                        str(self.envy),
                        f"--cache-root={self.cache_root}",
                        "install",
                        "--manifest",
                        str(manifest),
                    ],
                    capture_output=True,
                    text=True,
                )

                self.assertNotEqual(
                    result.returncode,
                    0,
                    f"expected failure, got success; stderr: {result.stderr}",
                )
                self.assertIn(
                    f"mid-{FAILING_MID} deliberate failure",
                    result.stderr + result.stdout,
                    f"missing failure message; stderr: {result.stderr}",
                )


if __name__ == "__main__":
    unittest.main()
