"""Functional tests for engine dependency resolution.

Tests the dependency graph construction phase: building dependency graphs,
cycle detection, memoization, and local/remote security constraints.
"""

import hashlib
import io
import subprocess
import tarfile
from pathlib import Path
import unittest

from . import test_config
from .env import EnvyTestCase
from .trace_parser import TraceParser

# Test archive contents
TEST_ARCHIVE_FILES = {
    "root/file1.txt": "Root file content\n",
    "root/file2.txt": "Another root file\n",
}


def create_test_archive(output_path: Path) -> str:
    """Create test.tar.gz archive and return its SHA256 hash."""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz") as tar:
        for name, content in TEST_ARCHIVE_FILES.items():
            data = content.encode("utf-8")
            info = tarfile.TarInfo(name=name)
            info.size = len(data)
            tar.addfile(info, io.BytesIO(data))
    archive_data = buf.getvalue()
    output_path.write_bytes(archive_data)
    return hashlib.sha256(archive_data).hexdigest()


class TestEngineDependencyResolution(EnvyTestCase):
    """Tests for dependency graph construction and validation."""

    def setUp(self):
        super().setUp()
        self.specs_dir = self.make_temp_dir("specs_dir")

        # Create test archive and get its hash
        self.archive_path = self.specs_dir / "test.tar.gz"
        self.archive_hash = create_test_archive(self.archive_path)

    def write_spec(self, name: str, content: str) -> Path:
        """Write a spec file with placeholder substitution."""
        content = content.format(
            ARCHIVE_PATH=self.archive_path.as_posix(),
            ARCHIVE_HASH=self.archive_hash,
        )
        path = self.specs_dir / name
        path.write_text(content, encoding="utf-8")
        return path

    def get_file_hash(self, filepath):
        """Get SHA256 hash of file using envy hash command."""
        result = test_config.run(
            [str(self.envy), "hash", str(filepath)],
            capture_output=True,
            text=True,
            check=True,
        )
        return result.stdout.strip().split("  ", 1)[0]

    def install_spec(self, identity: str, spec_name: str, **kwargs):
        """Install one spec through a generated manifest.

        Returns (result, registered) where registered is the set of package keys
        the engine resolved -- the dependency graph these tests assert on.
        """
        trace_file = self.cache_root / "trace.jsonl"
        manifest = test_config.write_spec_manifest(
            self.specs_dir, [(identity, self.specs_dir / spec_name)]
        )
        result = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                f"--trace=file:{trace_file}",
                "install",
                "--manifest",
                str(manifest),
            ],
            capture_output=True,
            text=True,
            **kwargs,
        )
        return result, TraceParser(trace_file).registered_specs()

    def test_recipe_with_one_dependency(self):
        """Engine loads spec and its dependency."""
        # Minimal test spec - no dependencies
        self.write_spec(
            "simple.lua",
            """-- Minimal test spec - no dependencies
IDENTITY = "local.simple@v1"
DEPENDENCIES = {{}}

USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic package - no cache interaction
    end,
  }},
}}

""",
        )

        # Spec with a dependency
        self.write_spec(
            "with_dep.lua",
            """-- Spec with a dependency
IDENTITY = "local.withdep@v1"
DEPENDENCIES = {{
  {{ spec = "local.simple@v1", source = "simple.lua" }}
}}

USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic package - no cache interaction
    end,
  }},
}}

""",
        )

        result, output = self.install_spec("local.withdep@v1", "with_dep.lua")

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        self.assertEqual(len(output), 2, f"Expected 2 recipes, got: {output}")

        # Check both identities present
        self.assertIn("local.withdep@v1", output)
        self.assertIn("local.simple@v1", output)

    def test_cycle_detection(self):
        """Engine detects and rejects dependency cycles."""
        # Spec A in a cycle: A -> B
        self.write_spec(
            "cycle_a.lua",
            """-- Spec A in a cycle: A -> B
IDENTITY = "local.cycle_a@v1"
DEPENDENCIES = {{
  {{ spec = "local.cycle_b@v1", source = "cycle_b.lua" }}
}}

USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic package
    end,
  }},
}}

""",
        )

        # Spec B in a cycle: B -> A
        self.write_spec(
            "cycle_b.lua",
            """-- Spec B in a cycle: B -> A
IDENTITY = "local.cycle_b@v1"
DEPENDENCIES = {{
  {{ spec = "local.cycle_a@v1", source = "cycle_a.lua" }}
}}

USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic package
    end,
  }},
}}

""",
        )

        result, _ = self.install_spec("local.cycle_a@v1", "cycle_a.lua")

        self.assertNotEqual(result.returncode, 0, "Expected cycle to cause failure")
        self.assertIn(
            "cycle",
            result.stderr.lower(),
            f"Expected cycle error, got: {result.stderr}",
        )

    def test_self_dependency_detection(self):
        """Engine detects and rejects spec depending on itself."""
        # Spec that depends on itself (self-loop)
        self.write_spec(
            "self_dep.lua",
            """-- Spec that depends on itself (self-loop)
IDENTITY = "local.self_dep@v1"
DEPENDENCIES = {{
  {{ spec = "local.self_dep@v1", source = "self_dep.lua" }}
}}

USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic package
    end,
  }},
}}

""",
        )

        result, _ = self.install_spec("local.self_dep@v1", "self_dep.lua")

        self.assertNotEqual(
            result.returncode, 0, "Expected self-dependency to cause failure"
        )
        self.assertIn(
            "cycle",
            result.stderr.lower(),
            f"Expected cycle error, got: {result.stderr}",
        )

    def test_diamond_dependency_memoization(self):
        """Engine memoizes shared dependencies (diamond: A->B,C; B,C->D)."""
        # Top of diamond: A depends on B and C (which both depend on D)
        self.write_spec(
            "diamond_a.lua",
            """-- Top of diamond: A depends on B and C (which both depend on D)
IDENTITY = "local.diamond_a@v1"
DEPENDENCIES = {{
  {{ spec = "local.diamond_b@v1", source = "diamond_b.lua" }},
  {{ spec = "local.diamond_c@v1", source = "diamond_c.lua" }}
}}

USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic package
    end,
  }},
}}

""",
        )

        # Left side of diamond: B depends on D
        self.write_spec(
            "diamond_b.lua",
            """-- Left side of diamond: B depends on D
IDENTITY = "local.diamond_b@v1"
DEPENDENCIES = {{
  {{ spec = "local.diamond_d@v1", source = "diamond_d.lua" }}
}}

USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic package
    end,
  }},
}}

""",
        )

        # Right side of diamond: C depends on D
        self.write_spec(
            "diamond_c.lua",
            """-- Right side of diamond: C depends on D
IDENTITY = "local.diamond_c@v1"
DEPENDENCIES = {{
  {{ spec = "local.diamond_d@v1", source = "diamond_d.lua" }}
}}

USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic package
    end,
  }},
}}

""",
        )

        # Base of diamond dependency
        self.write_spec(
            "diamond_d.lua",
            """-- Base of diamond dependency
IDENTITY = "local.diamond_d@v1"
DEPENDENCIES = {{}}

USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic package
    end,
  }},
}}

""",
        )

        result, output = self.install_spec("local.diamond_a@v1", "diamond_a.lua")

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        self.assertEqual(
            len(output), 4, f"Expected 4 specs (A,B,C,D once), got: {output}"
        )

        # Verify all present
        self.assertIn("local.diamond_a@v1", output)
        self.assertIn("local.diamond_b@v1", output)
        self.assertIn("local.diamond_c@v1", output)
        self.assertIn("local.diamond_d@v1", output)

    def test_dependency_completion_blocks_parent_check(self):
        """Dependency marked needed_by=check runs to completion and graph fully resolves."""
        # Helper spec with FETCH and STAGE
        self.write_spec(
            "fetch_dep_helper.lua",
            """IDENTITY = "local.fetch_dep_helper@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}",
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end
""",
        )

        # Parent spec whose recipe_fetch depends on a helper finishing all phases
        self.write_spec(
            "fetch_dep_blocked.lua",
            """-- Parent spec whose recipe_fetch depends on a helper finishing all phases.
IDENTITY = "local.fetch_dep_blocked@v1"

DEPENDENCIES = {{
  {{
    spec = "local.fetch_dep_helper@v1",
    source = "fetch_dep_helper.lua",
    needed_by = "check",
  }},
}}

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}",
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end
""",
        )

        result, output = self.install_spec(
            "local.fetch_dep_blocked@v1", "fetch_dep_blocked.lua"
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        self.assertEqual(
            len(output),
            2,
            f"Expected 2 specs (parent + helper), got: {output}",
        )

        self.assertIn("local.fetch_dep_blocked@v1", output)
        self.assertIn("local.fetch_dep_helper@v1", output)

    def test_multiple_independent_roots(self):
        """Engine resolves multiple independent dependency trees."""
        # Spec with two independent dependencies
        self.write_spec(
            "multiple_roots.lua",
            """-- Spec with two independent dependencies
IDENTITY = "local.multiple_roots@v1"
DEPENDENCIES = {{
  {{ spec = "local.independent_left@v1", source = "independent_left.lua" }},
  {{ spec = "local.independent_right@v1", source = "independent_right.lua" }}
}}

USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic package
    end,
  }},
}}

""",
        )

        # Independent tree, no shared deps
        self.write_spec(
            "independent_left.lua",
            """-- Independent tree, no shared deps
IDENTITY = "local.independent_left@v1"
DEPENDENCIES = {{}}

USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic package
    end,
  }},
}}

""",
        )

        # Independent tree, no shared deps
        self.write_spec(
            "independent_right.lua",
            """-- Independent tree, no shared deps
IDENTITY = "local.independent_right@v1"
DEPENDENCIES = {{}}

USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic package
    end,
  }},
}}

""",
        )

        result, output = self.install_spec(
            "local.multiple_roots@v1", "multiple_roots.lua"
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        self.assertEqual(len(output), 3, f"Expected 3 recipes, got: {output}")

        # Verify all present
        self.assertIn("local.multiple_roots@v1", output)
        self.assertIn("local.independent_left@v1", output)
        self.assertIn("local.independent_right@v1", output)

    def test_options_differentiate_recipes(self):
        """Same spec identity with different options creates separate entries."""
        # Spec with same dependency but different options
        self.write_spec(
            "options_parent.lua",
            """-- Spec with same dependency but different options
IDENTITY = "local.options_parent@v1"
DEPENDENCIES = {{
  {{ spec = "local.with_options@v1", source = "with_options.lua", options = {{ variant = "foo" }} }},
  {{ spec = "local.with_options@v1", source = "with_options.lua", options = {{ variant = "bar" }} }}
}}

USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic package
    end,
  }},
}}

""",
        )

        # Spec that supports options
        self.write_spec(
            "with_options.lua",
            """-- Spec that supports options
IDENTITY = "local.with_options@v1"
DEPENDENCIES = {{}}

USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic package
    end,
  }},
}}

""",
        )

        result, output = self.install_spec(
            "local.options_parent@v1", "options_parent.lua"
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        self.assertEqual(
            len(output),
            3,
            f"Expected 3 specs (parent + 2 variants), got: {output}",
        )

        # Verify all present with options in keys (strings are quoted)
        self.assertIn("local.options_parent@v1", output)
        self.assertIn('local.with_options@v1{variant="bar"}', output)
        self.assertIn('local.with_options@v1{variant="foo"}', output)

    def test_deep_chain_dependency(self):
        """Engine resolves deep dependency chain (A->B->C->D->E)."""
        # Deep chain: A -> B
        self.write_spec(
            "chain_a.lua",
            """-- Deep chain: A -> B
IDENTITY = "local.chain_a@v1"
DEPENDENCIES = {{
  {{ spec = "local.chain_b@v1", source = "chain_b.lua" }}
}}

USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic package
    end,
  }},
}}

""",
        )

        # Deep chain: B -> C
        self.write_spec(
            "chain_b.lua",
            """-- Deep chain: B -> C
IDENTITY = "local.chain_b@v1"
DEPENDENCIES = {{
  {{ spec = "local.chain_c@v1", source = "chain_c.lua" }}
}}

USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic package
    end,
  }},
}}

""",
        )

        # Deep chain: C -> D
        self.write_spec(
            "chain_c.lua",
            """-- Deep chain: C -> D
IDENTITY = "local.chain_c@v1"
DEPENDENCIES = {{
  {{ spec = "local.chain_d@v1", source = "chain_d.lua" }}
}}

USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic package
    end,
  }},
}}

""",
        )

        # Deep chain: D -> E
        self.write_spec(
            "chain_d.lua",
            """-- Deep chain: D -> E
IDENTITY = "local.chain_d@v1"
DEPENDENCIES = {{
  {{ spec = "local.chain_e@v1", source = "chain_e.lua" }}
}}

USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic package
    end,
  }},
}}

""",
        )

        # End of deep chain
        self.write_spec(
            "chain_e.lua",
            """-- End of deep chain
IDENTITY = "local.chain_e@v1"
DEPENDENCIES = {{}}

USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic package
    end,
  }},
}}

""",
        )

        result, output = self.install_spec("local.chain_a@v1", "chain_a.lua")

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        self.assertEqual(
            len(output), 5, f"Expected 5 specs (A,B,C,D,E), got: {output}"
        )

        # Verify all present
        self.assertIn("local.chain_a@v1", output)
        self.assertIn("local.chain_b@v1", output)
        self.assertIn("local.chain_c@v1", output)
        self.assertIn("local.chain_d@v1", output)
        self.assertIn("local.chain_e@v1", output)

    def test_wide_fanout_dependency(self):
        """Engine resolves wide fan-out (root->child1,2,3,4)."""
        # Wide fan-out: root depends on many children
        self.write_spec(
            "fanout_root.lua",
            """-- Wide fan-out: root depends on many children
IDENTITY = "local.fanout_root@v1"
DEPENDENCIES = {{
  {{ spec = "local.fanout_child1@v1", source = "fanout_child1.lua" }},
  {{ spec = "local.fanout_child2@v1", source = "fanout_child2.lua" }},
  {{ spec = "local.fanout_child3@v1", source = "fanout_child3.lua" }},
  {{ spec = "local.fanout_child4@v1", source = "fanout_child4.lua" }}
}}

USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic package
    end,
  }},
}}

""",
        )

        # Fan-out child 1
        self.write_spec(
            "fanout_child1.lua",
            """-- Fan-out child 1
IDENTITY = "local.fanout_child1@v1"
DEPENDENCIES = {{}}

USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic package
    end,
  }},
}}

""",
        )

        # Fan-out child 2
        self.write_spec(
            "fanout_child2.lua",
            """-- Fan-out child 2
IDENTITY = "local.fanout_child2@v1"
DEPENDENCIES = {{}}

USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic package
    end,
  }},
}}

""",
        )

        # Fan-out child 3
        self.write_spec(
            "fanout_child3.lua",
            """-- Fan-out child 3
IDENTITY = "local.fanout_child3@v1"
DEPENDENCIES = {{}}

USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic package
    end,
  }},
}}

""",
        )

        # Fan-out child 4
        self.write_spec(
            "fanout_child4.lua",
            """-- Fan-out child 4
IDENTITY = "local.fanout_child4@v1"
DEPENDENCIES = {{}}

USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic package
    end,
  }},
}}

""",
        )

        result, output = self.install_spec("local.fanout_root@v1", "fanout_root.lua")

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        self.assertEqual(len(output), 5, f"Expected 5 recipes, got: {output}")

        # Verify all present
        self.assertIn("local.fanout_root@v1", output)
        self.assertIn("local.fanout_child1@v1", output)
        self.assertIn("local.fanout_child2@v1", output)
        self.assertIn("local.fanout_child3@v1", output)
        self.assertIn("local.fanout_child4@v1", output)

    def test_nonlocal_cannot_depend_on_local(self):
        """Engine rejects non-local spec depending on local.*."""
        # Minimal test spec - no dependencies (used as target)
        self.write_spec(
            "simple.lua",
            """-- Minimal test spec - no dependencies
IDENTITY = "local.simple@v1"
DEPENDENCIES = {{}}

USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic package - no cache interaction
    end,
  }},
}}

""",
        )

        # Security test: non-local spec trying to depend on local.* recipe
        self.write_spec(
            "nonlocal_bad.lua",
            """-- remote.badrecipe@v1
-- Security test: non-local spec trying to depend on local.* recipe

IDENTITY = "remote.badrecipe@v1"
DEPENDENCIES = {{
  {{
    spec = "local.simple@v1",
    source = "simple.lua"
  }}
}}

USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      envy.info("Installing bad recipe")
    end,
  }},
}}

""",
        )

        result, _ = self.install_spec("remote.badrecipe@v1", "nonlocal_bad.lua")

        self.assertNotEqual(
            result.returncode, 0, "Expected validation to cause failure"
        )
        stderr_lower = result.stderr.lower()
        self.assertTrue(
            "security" in stderr_lower or "local" in stderr_lower,
            f"Expected security/local dep error, got: {result.stderr}",
        )

    def test_remote_file_uri_no_dependencies(self):
        """Remote spec with file:// source and no dependencies succeeds."""
        # Remote spec with no dependencies
        self.write_spec(
            "remote_fileuri.lua",
            """-- remote.fileuri@v1
-- Remote spec with no dependencies

IDENTITY = "remote.fileuri@v1"

USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      envy.info("Installing remote fileuri recipe")
    end,
  }},
}}

""",
        )

        result, output = self.install_spec("remote.fileuri@v1", "remote_fileuri.lua")

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        self.assertEqual(output, {"remote.fileuri@v1"})

    def test_remote_depends_on_remote(self):
        """Remote spec depending on another remote spec succeeds."""
        # Remote spec with no dependencies
        self.write_spec(
            "remote_child.lua",
            """-- remote.child@v1
-- Remote spec with no dependencies

IDENTITY = "remote.child@v1"

USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      envy.info("Installing remote child recipe")
    end,
  }},
}}

""",
        )

        # Remote spec depending on another remote recipe
        self.write_spec(
            "remote_parent.lua",
            """-- remote.parent@v1
-- Remote spec depending on another remote recipe

IDENTITY = "remote.parent@v1"
DEPENDENCIES = {{
  {{
    spec = "remote.child@v1",
    source = "remote_child.lua"
    -- No SHA256 (permissive mode - for testing)
  }}
}}

USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      envy.info("Installing remote parent recipe")
    end,
  }},
}}

""",
        )

        result, output = self.install_spec("remote.parent@v1", "remote_parent.lua")

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        self.assertEqual(len(output), 2, f"Expected 2 recipes, got: {output}")

        self.assertIn("remote.parent@v1", output)
        self.assertIn("remote.child@v1", output)

    def test_local_depends_on_remote(self):
        """Local spec depending on remote spec succeeds."""
        # Remote spec with no dependencies
        self.write_spec(
            "remote_base.lua",
            """-- remote.base@v1
-- Remote spec with no dependencies

IDENTITY = "remote.base@v1"

USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      envy.info("Installing remote base recipe")
    end,
  }},
}}

""",
        )

        # Local spec depending on remote recipe
        self.write_spec(
            "local_wrapper.lua",
            """-- local.wrapper@v1
-- Local spec depending on remote recipe

IDENTITY = "local.wrapper@v1"
DEPENDENCIES = {{
  {{
    spec = "remote.base@v1",
    source = "remote_base.lua"
    -- No SHA256 (permissive mode - for testing)
  }}
}}

USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      envy.info("Installing local wrapper recipe")
    end,
  }},
}}

""",
        )

        result, output = self.install_spec("local.wrapper@v1", "local_wrapper.lua")

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        self.assertEqual(len(output), 2, f"Expected 2 recipes, got: {output}")

        self.assertIn("local.wrapper@v1", output)
        self.assertIn("remote.base@v1", output)

    def test_local_depends_on_local(self):
        """Local spec depending on another local spec succeeds."""
        # Local spec with no dependencies
        self.write_spec(
            "local_child.lua",
            """-- local.child@v1
-- Local spec with no dependencies

IDENTITY = "local.child@v1"

USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      envy.info("Installing local child recipe")
    end,
  }},
}}

""",
        )

        # Local spec depending on another local recipe
        self.write_spec(
            "local_parent.lua",
            """-- local.parent@v1
-- Local spec depending on another local recipe

IDENTITY = "local.parent@v1"
DEPENDENCIES = {{
  {{
    spec = "local.child@v1",
    source = "local_child.lua"
  }}
}}

USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      envy.info("Installing local parent recipe")
    end,
  }},
}}

""",
        )

        result, output = self.install_spec("local.parent@v1", "local_parent.lua")

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        self.assertEqual(len(output), 2, f"Expected 2 recipes, got: {output}")

        self.assertIn("local.parent@v1", output)
        self.assertIn("local.child@v1", output)

    def test_transitive_local_dependency_rejected(self):
        """Remote spec transitively depending on local.* fails."""
        # Local spec with no dependencies
        self.write_spec(
            "local_c.lua",
            """-- local.c@v1
-- Local spec with no dependencies

IDENTITY = "local.c@v1"

USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      envy.info("Installing local c recipe")
    end,
  }},
}}

""",
        )

        # Remote spec that depends on local.* (transitively violates security)
        self.write_spec(
            "remote_transitive_b.lua",
            """-- remote.b@v1
-- Remote spec that depends on local.* (transitively violates security)

IDENTITY = "remote.b@v1"
DEPENDENCIES = {{
  {{
    spec = "local.c@v1",
    source = "local_c.lua"
  }}
}}

USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      envy.info("Installing remote transitive b recipe")
    end,
  }},
}}

""",
        )

        # Remote spec that transitively depends on local.* through remote.b
        self.write_spec(
            "remote_transitive_a.lua",
            """-- remote.a@v1
-- Remote spec that transitively depends on local.* through remote.b

IDENTITY = "remote.a@v1"
DEPENDENCIES = {{
  {{
    spec = "remote.b@v1",
    source = "remote_transitive_b.lua"
    -- No SHA256 (permissive mode - for testing)
  }}
}}

USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      envy.info("Installing remote transitive a recipe")
    end,
  }},
}}

""",
        )

        result, _ = self.install_spec("remote.a@v1", "remote_transitive_a.lua")

        self.assertNotEqual(
            result.returncode, 0, "Expected transitive violation to cause failure"
        )
        stderr_lower = result.stderr.lower()
        self.assertTrue(
            "security" in stderr_lower or "local" in stderr_lower,
            f"Expected security/local dep error, got: {result.stderr}",
        )

    def test_fetch_dependency_cycle(self):
        """Engine detects and rejects fetch dependency cycles."""
        # Spec A in a fetch dependency cycle: A fetch needs B
        self.write_spec(
            "fetch_cycle_a.lua",
            """-- Spec A in a fetch dependency cycle: A fetch needs B
IDENTITY = "local.fetch_cycle_a@v1"
DEPENDENCIES = {{
  {{
    spec = "local.fetch_cycle_b@v1",
    source = "fetch_cycle_b.lua",  -- Will be fetched by custom fetch function
    fetch = function(tmp_dir, options)
      -- Custom fetch that needs fetch_cycle_b to be available
      -- This creates a cycle since fetch_cycle_b also fetch-depends on A
      error("Should not reach here - cycle should be detected first")
    end
  }}
}}

function FETCH(tmp_dir, options)
  -- Simple fetch function for this recipe
end

function INSTALL(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  -- Programmatic package
end
""",
        )

        # Spec B in a fetch dependency cycle: B fetch needs A
        self.write_spec(
            "fetch_cycle_b.lua",
            """-- Spec B in a fetch dependency cycle: B fetch needs A
IDENTITY = "local.fetch_cycle_b@v1"
DEPENDENCIES = {{
  {{
    spec = "local.fetch_cycle_a@v1",
    source = "fetch_cycle_a.lua",  -- Will be fetched by custom fetch function
    fetch = function(tmp_dir, options)
      -- Custom fetch that needs fetch_cycle_a to be available
      -- This completes the cycle: A fetch needs B, B fetch needs A
      error("Should not reach here - cycle should be detected first")
    end
  }}
}}

function FETCH(tmp_dir, options)
  -- Simple fetch function for this recipe
end

function INSTALL(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  -- Programmatic package
end
""",
        )

        result, _ = self.install_spec("local.fetch_cycle_a@v1", "fetch_cycle_a.lua")

        self.assertNotEqual(
            result.returncode, 0, "Expected fetch dependency cycle to cause failure"
        )
        stderr_lower = result.stderr.lower()
        # Accept either "Fetch dependency cycle" or "Dependency cycle" as both indicate detection
        self.assertIn(
            "cycle",
            stderr_lower,
            f"Expected cycle error, got: {result.stderr}",
        )
        # Verify the cycle path includes both recipes
        self.assertIn(
            "fetch_cycle_a",
            stderr_lower,
            f"Expected fetch_cycle_a in error, got: {result.stderr}",
        )
        self.assertIn(
            "fetch_cycle_b",
            stderr_lower,
            f"Expected fetch_cycle_b in error, got: {result.stderr}",
        )

    def test_simple_fetch_dependency(self):
        """Simple fetch dependency: A fetch needs B - validates basic flow and blocking."""
        # Base spec that will be a fetch dependency for another recipe
        self.write_spec(
            "simple_fetch_dep_base.lua",
            """-- Base spec that will be a fetch dependency for another recipe
IDENTITY = "local.simple_fetch_dep_base@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}",
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end
""",
        )

        # Spec with a dependency that has fetch prerequisites
        self.write_spec(
            "simple_fetch_dep_parent.lua",
            """-- Spec with a dependency that has fetch prerequisites
IDENTITY = "local.simple_fetch_dep_parent@v1"

DEPENDENCIES = {{
  {{
    spec = "local.simple_fetch_dep_child@v1",
    source = {{
      dependencies = {{
        {{ spec = "local.simple_fetch_dep_base@v1", source = "simple_fetch_dep_base.lua" }}
      }},
      fetch = function(tmp_dir, options)
        -- Base spec is guaranteed to be installed before this runs
        -- Write the spec.lua for the child recipe
        local recipe_content = [[
IDENTITY = "local.simple_fetch_dep_child@v1"
DEPENDENCIES = {{}}

USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic package
    end,
  }},
}}
]]
        local recipe_path = tmp_dir .. "/spec.lua"
        local f = io.open(recipe_path, "w")
        f:write(recipe_content)
        f:close()

        -- Commit the spec file to the fetch_dir
        envy.commit_fetch("spec.lua")
      end
    }}
  }}
}}

USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic package
    end,
  }},
}}
""",
        )

        result, output = self.install_spec(
            "local.simple_fetch_dep_parent@v1", "simple_fetch_dep_parent.lua"
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        self.assertEqual(
            len(output),
            3,
            f"Expected 3 specs (parent + child + base), got: {output}",
        )

        self.assertIn("local.simple_fetch_dep_parent@v1", output)
        self.assertIn("local.simple_fetch_dep_child@v1", output)
        self.assertIn("local.simple_fetch_dep_base@v1", output)

    def test_multi_level_nesting(self):
        """Multi-level nesting: A fetch needs B, B fetch needs C, C fetch needs base."""
        # Base spec that will be a fetch dependency for another recipe
        self.write_spec(
            "simple_fetch_dep_base.lua",
            """-- Base spec that will be a fetch dependency for another recipe
IDENTITY = "local.simple_fetch_dep_base@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}",
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end
""",
        )

        # Multi-level nesting spec
        self.write_spec(
            "multi_level_a.lua",
            """IDENTITY = "local.multi_level_a@v1"

DEPENDENCIES = {{
  {{
    spec = "local.multi_level_b@v1",
    source = {{
      dependencies = {{
        {{ spec = "local.simple_fetch_dep_base@v1", source = "simple_fetch_dep_base.lua" }}
      }},
      fetch = function(tmp_dir, options)
        local recipe_content = [=[
IDENTITY = "local.multi_level_b@v1"
DEPENDENCIES = {{
  {{
    spec = "local.multi_level_c@v1",
    source = {{
      dependencies = {{
        {{ spec = "local.simple_fetch_dep_base@v1", source = "simple_fetch_dep_base.lua" }}
      }},
      fetch = function(tmp_dir, options)
        local recipe_content_c = [[
IDENTITY = "local.multi_level_c@v1"
DEPENDENCIES = {{}}
USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic package
    end,
  }},
}}
]]
        local recipe_path_c = tmp_dir .. "/spec.lua"
        local f_c = io.open(recipe_path_c, "w")
        f_c:write(recipe_content_c)
        f_c:close()
        envy.commit_fetch("spec.lua")
      end
    }}
  }}
}}
USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic package
    end,
  }},
}}
]=]
        local recipe_path = tmp_dir .. "/spec.lua"
        local f = io.open(recipe_path, "w")
        f:write(recipe_content)
        f:close()
        envy.commit_fetch("spec.lua")
      end
    }}
  }}
}}

USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic package
    end,
  }},
}}
""",
        )

        result, output = self.install_spec(
            "local.multi_level_a@v1", "multi_level_a.lua"
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        self.assertEqual(
            len(output),
            4,
            f"Expected 4 specs (A + B + C + base), got: {output}",
        )

        self.assertIn("local.multi_level_a@v1", output)
        self.assertIn("local.multi_level_b@v1", output)
        self.assertIn("local.multi_level_c@v1", output)
        self.assertIn("local.simple_fetch_dep_base@v1", output)

    def test_multiple_fetch_dependencies(self):
        """Multiple fetch dependencies: A fetch needs [B, C] - parallel installation."""
        # Base spec that will be a fetch dependency for another recipe
        self.write_spec(
            "simple_fetch_dep_base.lua",
            """-- Base spec that will be a fetch dependency for another recipe
IDENTITY = "local.simple_fetch_dep_base@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}",
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end
""",
        )

        # Helper spec with FETCH and STAGE
        self.write_spec(
            "fetch_dep_helper.lua",
            """IDENTITY = "local.fetch_dep_helper@v1"

FETCH = {{
  source = "{ARCHIVE_PATH}",
  sha256 = "{ARCHIVE_HASH}",
}}

STAGE = function(fetch_dir, stage_dir, tmp_dir, options)
  envy.extract_all(fetch_dir, stage_dir, {{strip = 1}})
end
""",
        )

        # Spec with multiple fetch dependencies
        self.write_spec(
            "multiple_fetch_deps_parent.lua",
            """IDENTITY = "local.multiple_fetch_deps_parent@v1"

DEPENDENCIES = {{
  {{
    spec = "local.multiple_fetch_deps_child@v1",
    source = {{
      dependencies = {{
        {{ spec = "local.simple_fetch_dep_base@v1", source = "simple_fetch_dep_base.lua" }},
        {{ spec = "local.fetch_dep_helper@v1", source = "fetch_dep_helper.lua" }}
      }},
      fetch = function(tmp_dir, options)
        local recipe_content = [[
IDENTITY = "local.multiple_fetch_deps_child@v1"
DEPENDENCIES = {{}}
USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic package
    end,
  }},
}}
]]
        local recipe_path = tmp_dir .. "/spec.lua"
        local f = io.open(recipe_path, "w")
        f:write(recipe_content)
        f:close()
        envy.commit_fetch("spec.lua")
      end
    }}
  }}
}}

USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic package
    end,
  }},
}}
""",
        )

        result, output = self.install_spec(
            "local.multiple_fetch_deps_parent@v1", "multiple_fetch_deps_parent.lua"
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        self.assertEqual(
            len(output),
            4,
            f"Expected 4 specs (parent + child + base + helper), got: {output}",
        )

        self.assertIn("local.multiple_fetch_deps_parent@v1", output)
        self.assertIn("local.multiple_fetch_deps_child@v1", output)
        self.assertIn("local.simple_fetch_dep_base@v1", output)
        self.assertIn("local.fetch_dep_helper@v1", output)


if __name__ == "__main__":
    unittest.main()
