"""What each boundary reports for an error an envy.* function throws.

envy.json.decode("{") throws from envy's C++ through sol's trampoline. Each boundary a
spec can reach -- `envy lua`, pcall, a phase on an engine worker, bundle validation on its
own thread -- must report the message and fail; none may abort. A throw from inside the
C++ runtime's own compiled code is sol_util_tests.cpp's: shipped envy has no path to one.
"""

from __future__ import annotations

import unittest

from . import test_config
from .env import EnvyTestCase

THROW = 'envy.json.decode("{")'
MESSAGE = "envy.json.decode: syntax error"


class TestExceptionBoundaries(EnvyTestCase):
    def assert_reported(self, run):
        self.assertEqual(1, run.returncode, test_config.describe_exit(run.result))
        self.assertIn(MESSAGE, run.stderr)

    def test_lua_call(self):
        self.assert_reported(self.run_lua(f"{THROW}\n"))

    def test_lua_pcall(self):
        self.assertIn(MESSAGE, self.lua_error(THROW))

    def test_phase_on_an_engine_worker(self):
        identity = "local.throw@r0"
        spec = self.write_spec(
            "throw.lua",
            f'IDENTITY = "{identity}"\nFETCH = function() end\n'
            f"INSTALL = function() {THROW} end\n",
        )
        manifest = test_config.write_spec_manifest(self.work, [(identity, spec.name)])
        self.assert_reported(self.install(manifest))

    def test_bundle_validation_thread(self):
        bundle = self.make_temp_dir("bundle")
        (bundle / "envy-bundle.lua").write_text(
            'BUNDLE = "test.b@r0"\nSPECS = { ["test.p@r0"] = "p.lua" }\n',
            encoding="utf-8",
        )
        (bundle / "p.lua").write_text(
            f'{THROW}\nIDENTITY = "test.p@r0"\n', encoding="utf-8"
        )
        manifest = self.write_manifest(
            f'BUNDLES = {{ b = {{ identity = "test.b@r0", '
            f'source = "{self.lua_path(bundle)}" }} }}\n'
            'PACKAGES = { { spec = "test.p@r0", bundle = "b" } }\n',
            directory=self.make_temp_dir("project"),
        )
        self.assert_reported(self.install(manifest))


if __name__ == "__main__":
    unittest.main()
