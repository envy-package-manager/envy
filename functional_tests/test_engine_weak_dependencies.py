"""Functional tests for weak dependency resolution."""

from pathlib import Path
import unittest

from . import test_config
from .env import EnvyTestCase
from .trace_parser import TraceParser


class TestEngineWeakDependencies(EnvyTestCase):
    """Weak dependency resolution scenarios."""

    def setUp(self):
        super().setUp()
        self.specs_dir = self.make_temp_dir("specs_dir")

    def write_spec(self, name: str, content: str) -> None:
        """Write a spec file to the temp specs directory."""
        (self.specs_dir / name).write_text(content, encoding="utf-8")

    def run_engine(self, identity, spec_name):
        """Install the spec via a generated manifest; the trace records what resolved."""
        manifest = test_config.write_spec_manifest(
            self.specs_dir, [(identity, self.specs_dir / spec_name)]
        )
        self.trace_file = self.cache_root / "trace.jsonl"
        return test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                f"--trace=file:{self.trace_file}",
                "install",
                "--manifest",
                str(manifest),
            ],
            capture_output=True,
            text=True,
        )

    def registered(self):
        """The package set the engine resolved, per spec_registered trace events."""
        return TraceParser(self.trace_file).registered_specs()

    def test_weak_fallback_used_when_no_match(self):
        # Weak dependency with fallback when the target is absent
        weak_consumer_fallback = """-- Weak dependency with fallback when the target is absent
IDENTITY = "local.weak_consumer_fallback@v1"
DEPENDENCIES = {
  { spec = "local.missing_dep", weak = { spec = "local.weak_fallback@v1", source = "weak_fallback.lua" } },
}

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic install: no cache artifacts
    end,
  },
}

"""
        self.write_spec("weak_consumer_fallback.lua", weak_consumer_fallback)

        # Fallback spec used when no provider is found
        weak_fallback = """-- Fallback spec used when no provider is found
IDENTITY = "local.weak_fallback@v1"
DEPENDENCIES = {}

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic install: no cache artifacts
    end,
  },
}

"""
        self.write_spec("weak_fallback.lua", weak_fallback)

        result = self.run_engine(
            "local.weak_consumer_fallback@v1",
            "weak_consumer_fallback.lua",
        )
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        output = self.registered()
        self.assertIn("local.weak_consumer_fallback@v1", output)
        self.assertIn("local.weak_fallback@v1", output)

    def test_weak_prefers_existing_match(self):
        # Weak dependency with a present strong match and unused fallback
        weak_consumer_existing = """-- Weak dependency with a present strong match and unused fallback
IDENTITY = "local.weak_consumer_existing@v1"
DEPENDENCIES = {
  { spec = "local.existing_dep@v1", source = "weak_existing_dep.lua" },
  { spec = "local.existing_dep", weak = { spec = "local.unused_fallback@v1", source = "weak_unused_fallback.lua" } },
}

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic install: no cache artifacts
    end,
  },
}

"""
        self.write_spec("weak_consumer_existing.lua", weak_consumer_existing)

        # Strong dependency that should satisfy weak queries
        weak_existing_dep = """-- Strong dependency that should satisfy weak queries
IDENTITY = "local.existing_dep@v1"
DEPENDENCIES = {}

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic install: no cache artifacts
    end,
  },
}

"""
        self.write_spec("weak_existing_dep.lua", weak_existing_dep)

        # Fallback that should be ignored when a strong match exists
        weak_unused_fallback = """-- Fallback that should be ignored when a strong match exists
IDENTITY = "local.unused_fallback@v1"
DEPENDENCIES = {}

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic install: no cache artifacts
    end,
  },
}

"""
        self.write_spec("weak_unused_fallback.lua", weak_unused_fallback)

        result = self.run_engine(
            "local.weak_consumer_existing@v1",
            "weak_consumer_existing.lua",
        )
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        output = self.registered()
        self.assertIn("local.existing_dep@v1", output)
        self.assertNotIn("local.unused_fallback@v1", output)

    def test_reference_only_resolves_to_existing_recipe(self):
        # Reference-only dependency that is satisfied by an existing provider
        weak_consumer_ref_only = """-- Reference-only dependency that is satisfied by an existing provider
IDENTITY = "local.weak_consumer_ref_only@v1"
DEPENDENCIES = {
  { spec = "local.weak_provider@v1", source = "weak_provider.lua" },
  { spec = "local.weak_provider" },
}

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic install: no cache artifacts
    end,
  },
}

"""
        self.write_spec("weak_consumer_ref_only.lua", weak_consumer_ref_only)

        # Provides a concrete spec for weak/reference consumers
        weak_provider = """-- Provides a concrete spec for weak/reference consumers
IDENTITY = "local.weak_provider@v1"
DEPENDENCIES = {}

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic install: no cache artifacts
    end,
  },
}

"""
        self.write_spec("weak_provider.lua", weak_provider)

        result = self.run_engine(
            "local.weak_consumer_ref_only@v1",
            "weak_consumer_ref_only.lua",
        )
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        output = self.registered()
        self.assertIn("local.weak_provider@v1", output)

    def test_ambiguity_reports_all_candidates(self):
        # Reference-only dependency that matches multiple strong candidates
        weak_consumer_ambiguous = """-- Reference-only dependency that matches multiple strong candidates
IDENTITY = "local.weak_consumer_ambiguous@v1"
DEPENDENCIES = {
  { spec = "local.dupe@v1", source = "weak_dupe_v1.lua" },
  { spec = "local.dupe@v2", source = "weak_dupe_v2.lua" },
  { spec = "local.dupe" },
}

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic install: no cache artifacts
    end,
  },
}

"""
        self.write_spec("weak_consumer_ambiguous.lua", weak_consumer_ambiguous)

        # First candidate for ambiguity tests
        weak_dupe_v1 = """-- First candidate for ambiguity tests
IDENTITY = "local.dupe@v1"
DEPENDENCIES = {}

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic install: no cache artifacts
    end,
  },
}

"""
        self.write_spec("weak_dupe_v1.lua", weak_dupe_v1)

        # Second candidate for ambiguity tests
        weak_dupe_v2 = """-- Second candidate for ambiguity tests
IDENTITY = "local.dupe@v2"
DEPENDENCIES = {}

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic install: no cache artifacts
    end,
  },
}

"""
        self.write_spec("weak_dupe_v2.lua", weak_dupe_v2)

        result = self.run_engine(
            "local.weak_consumer_ambiguous@v1",
            "weak_consumer_ambiguous.lua",
        )
        self.assertNotEqual(result.returncode, 0, "Ambiguity should fail resolution")
        self.assertIn("ambiguous", result.stderr.lower())
        self.assertIn("local.dupe@v1", result.stderr)
        self.assertIn("local.dupe@v2", result.stderr)

    def test_missing_reference_reports_progress_error(self):
        # Reference-only dependency with no provider anywhere in the graph
        weak_missing_ref = """-- Reference-only dependency with no provider anywhere in the graph
IDENTITY = "local.weak_missing_ref@v1"
DEPENDENCIES = {
  { spec = "local.never_provided" },
}

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic install: no cache artifacts
    end,
  },
}

"""
        self.write_spec("weak_missing_ref.lua", weak_missing_ref)

        result = self.run_engine(
            "local.weak_missing_ref@v1",
            "weak_missing_ref.lua",
        )
        self.assertNotEqual(result.returncode, 0, "Missing reference should fail")
        self.assertIn("never_provided", result.stderr)
        self.assertIn("no progress", result.stderr.lower())

    def test_cascading_weak_resolution(self):
        # Weak reference whose fallback introduces another weak reference (multi-iteration)
        weak_chain_root = """-- Weak reference whose fallback introduces another weak reference (multi-iteration)
IDENTITY = "local.weak_chain_root@v1"
DEPENDENCIES = {
  { spec = "local.chain_missing", weak = { spec = "local.chain_b@v1", source = "weak_chain_b.lua" } },
}

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic install: no cache artifacts
    end,
  },
}

"""
        self.write_spec("weak_chain_root.lua", weak_chain_root)

        # Fallback that itself carries a weak reference
        weak_chain_b = """-- Fallback that itself carries a weak reference
IDENTITY = "local.chain_b@v1"
DEPENDENCIES = {
  { spec = "local.chain_c", weak = { spec = "local.chain_c@v1", source = "weak_chain_c.lua" } },
}

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic install: no cache artifacts
    end,
  },
}

"""
        self.write_spec("weak_chain_b.lua", weak_chain_b)

        # Terminal dependency for the weak chain
        weak_chain_c = """-- Terminal dependency for the weak chain
IDENTITY = "local.chain_c@v1"
DEPENDENCIES = {}

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic install: no cache artifacts
    end,
  },
}

"""
        self.write_spec("weak_chain_c.lua", weak_chain_c)

        result = self.run_engine(
            "local.weak_chain_root@v1",
            "weak_chain_root.lua",
        )
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        output = self.registered()
        self.assertIn("local.chain_b@v1", output)
        self.assertIn("local.chain_c@v1", output)

    def test_progress_with_flat_unresolved_count(self):
        # Two weak fallbacks that each introduce new weak references; unresolved count stays flat
        weak_progress_flat_root = """-- Two weak fallbacks that each introduce new weak references; unresolved count stays flat
IDENTITY = "local.weak_progress_flat_root@v1"
DEPENDENCIES = {
  { spec = "local.branch_one", weak = { spec = "local.branch_one@v1", source = "weak_branch_one.lua" } },
  { spec = "local.branch_two", weak = { spec = "local.branch_two@v1", source = "weak_branch_two.lua" } },
  { spec = "local.shared" },
}

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic install: no cache artifacts
    end,
  },
}

"""
        self.write_spec("weak_progress_flat_root.lua", weak_progress_flat_root)

        # Fallback that creates a shared dependency
        weak_branch_one = """-- Fallback that creates a shared dependency
IDENTITY = "local.branch_one@v1"
DEPENDENCIES = {
  { spec = "local.shared", weak = { spec = "local.shared@v1", source = "weak_shared.lua" } },
}

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic install: no cache artifacts
    end,
  },
}

"""
        self.write_spec("weak_branch_one.lua", weak_branch_one)

        # Second fallback that depends on the same shared target
        weak_branch_two = """-- Second fallback that depends on the same shared target
IDENTITY = "local.branch_two@v1"
DEPENDENCIES = {
  { spec = "local.shared", weak = { spec = "local.shared@v1", source = "weak_shared.lua" } },
}

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic install: no cache artifacts
    end,
  },
}

"""
        self.write_spec("weak_branch_two.lua", weak_branch_two)

        # Shared dependency produced by fallbacks
        weak_shared = """-- Shared dependency produced by fallbacks
IDENTITY = "local.shared@v1"
DEPENDENCIES = {}

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic install: no cache artifacts
    end,
  },
}

"""
        self.write_spec("weak_shared.lua", weak_shared)

        result = self.run_engine(
            "local.weak_progress_flat_root@v1",
            "weak_progress_flat_root.lua",
        )
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        output = self.registered()
        self.assertIn("local.branch_one@v1", output)
        self.assertIn("local.branch_two@v1", output)
        self.assertIn("local.shared@v1", output)

    def test_nested_weak_fetch_dep_rejected_even_when_helper_exists(self):
        """A weak source.dependencies entry is refused even if a provider is present.

        The tempting reading is that rejection is only needed when nothing could
        satisfy the entry. It is not: satisfaction happens in
        resolve_weak_references(), which runs only once every spec_fetch has
        completed -- including that of the consumer whose fetch function is waiting on
        this entry. So an available provider changes nothing about the ordering, and
        accepting the entry would mean running the fetch function without it. Measured
        before this was enforced: the fetch function ran at seq <=11 while the entry's
        provider had not started its spec_fetch until seq 14.

        Rejection is therefore unconditional, which is what this pins. Both the
        reference-only and `weak = { ... }` forms are covered in
        test_fetch_closure_weak_refs.py.
        """
        root = """IDENTITY = "local.weak_custom_fetch_root_with_helper@v1"

DEPENDENCIES = {
  { spec = "local.helper@v1", source = "weak_helper_strong.lua" },
  {
    spec = "local.custom_fetch_dep@v1",
    source = {
      dependencies = {
        { spec = "local.helper", weak = { spec = "local.helper.fallback@v1", source = "weak_helper_fallback.lua" } },
      },
      fetch = function(tmp_dir, options)
        local f = io.open(tmp_dir .. "/spec.lua", "w")
        f:write('IDENTITY = "local.custom_fetch_dep@v1"\\nUSER_MANAGED = true\\n' ..
                'SETUP = { main = { CHECK = function() return true end, INSTALL = function() end } }\\n')
        f:close()
        envy.commit_fetch("spec.lua")
      end,
    },
  },
}

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options) return true end,
    INSTALL = function(pkg_dir, options) end,
  },
}
"""
        helper = """IDENTITY = "local.helper@v1"
USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options) return true end,
    INSTALL = function(pkg_dir, options) end,
  },
}
"""
        self.write_spec("weak_custom_fetch_root_with_helper.lua", root)
        self.write_spec("weak_helper_strong.lua", helper)
        self.write_spec(
            "weak_helper_fallback.lua", helper.replace("local.helper@v1", "local.helper.fallback@v1")
        )

        result = self.run_engine(
            "local.weak_custom_fetch_root_with_helper@v1",
            "weak_custom_fetch_root_with_helper.lua",
        )

        self.assertNotEqual(result.returncode, 0, f"expected rejection\n{result.stderr}")
        self.assertIn("must be a strong reference", result.stderr)

    def test_weak_resolution_detects_cycles(self):
        """Weak reference resolution must detect cycles introduced after resolution."""
        # Root spec that loads both A and B before weak resolution
        weak_cycle_root = """-- Root spec that loads both A and B before weak resolution
IDENTITY = "local.weak_cycle_root@v1"
DEPENDENCIES = {
  { spec = "local.weak_cycle_a@v1", source = "weak_cycle_a.lua" },
  { spec = "local.foo@v1", source = "weak_cycle_b.lua" },
}

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic install
    end,
  },
}

"""
        self.write_spec("weak_cycle_root.lua", weak_cycle_root)

        # Spec A with weak reference that creates cycle after resolution
        weak_cycle_a = """-- Spec A with weak reference that creates cycle after resolution
IDENTITY = "local.weak_cycle_a@v1"
DEPENDENCIES = {
  { spec = "foo" },  -- Weak ref-only, will match weak_cycle_b
}

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic install
    end,
  },
}

"""
        self.write_spec("weak_cycle_a.lua", weak_cycle_a)

        # Spec B (matches "foo" query) that depends on A, creating cycle
        weak_cycle_b = """-- Spec B (matches "foo" query) that depends on A, creating cycle
IDENTITY = "local.foo@v1"
DEPENDENCIES = {
  { spec = "local.weak_cycle_a@v1", source = "weak_cycle_a.lua" }
}

USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options)
      return false
    end,
    INSTALL = function(pkg_dir, options)
      -- Programmatic install
    end,
  },
}

"""
        self.write_spec("weak_cycle_b.lua", weak_cycle_b)

        result = self.run_engine(
            "local.weak_cycle_root@v1",
            "weak_cycle_root.lua",
        )
        self.assertNotEqual(result.returncode, 0, "Expected cycle to cause failure")
        self.assertIn("cycle", result.stderr.lower())
        self.assertIn("local.weak_cycle_a@v1", result.stderr)
        self.assertIn("local.foo@v1", result.stderr)


if __name__ == "__main__":
    unittest.main()
