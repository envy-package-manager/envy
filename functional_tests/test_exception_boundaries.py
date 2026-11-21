"""What each boundary reports for an error envy throws and one the C++ runtime throws.

envy.test_throw (functional tester only) is an ordinary envy.* function: "stdlib" throws
from inside the C++ runtime's compiled code (libc++.dylib on macOS, which -fno-rtti code
can't catch as std::exception), anything else from envy. Each boundary a spec can reach --
`envy lua`, pcall, a phase on an engine worker, bundle validation on its own thread -- must
report the message and fail; none may abort. Messages are each runtime's common wording.
"""

from __future__ import annotations

import unittest

from . import test_config
from .env import EnvyTestCase

MESSAGES = {"envy": "test_throw: thrown by envy", "stdlib": "stoi"}


class TestExceptionBoundaries(EnvyTestCase):
    def assert_reported(self, run, message: str):
        self.assertEqual(1, run.returncode, test_config.describe_exit(run.result))
        self.assertIn(message, run.stderr)

    def test_lua_call(self):
        for kind, message in MESSAGES.items():
            with self.subTest(kind=kind):
                self.assert_reported(self.run_lua(f'envy.test_throw("{kind}")\n'), message)

    def test_lua_pcall(self):
        for kind, message in MESSAGES.items():
            with self.subTest(kind=kind):
                self.assertIn(message, self.lua_error(f'envy.test_throw("{kind}")'))

    def test_phase_on_an_engine_worker(self):
        for kind, message in MESSAGES.items():
            with self.subTest(kind=kind):
                identity = f"local.throw-{kind}@r0"
                spec = self.write_spec(
                    f"throw-{kind}.lua",
                    f'IDENTITY = "{identity}"\nFETCH = function() end\n'
                    f'INSTALL = function() envy.test_throw("{kind}") end\n',
                )
                manifest = test_config.write_spec_manifest(self.work, [(identity, spec.name)])
                self.assert_reported(self.install(manifest), message)

    def test_bundle_validation_thread(self):
        for kind, message in MESSAGES.items():
            with self.subTest(kind=kind):
                bundle = self.make_temp_dir(f"bundle-{kind}")
                (bundle / "envy-bundle.lua").write_text(
                    'BUNDLE = "test.b@r0"\nSPECS = { ["test.p@r0"] = "p.lua" }\n',
                    encoding="utf-8",
                )
                (bundle / "p.lua").write_text(
                    f'envy.test_throw("{kind}")\nIDENTITY = "test.p@r0"\n', encoding="utf-8"
                )
                manifest = self.write_manifest(
                    f'BUNDLES = {{ b = {{ identity = "test.b@r0", '
                    f'source = "{self.lua_path(bundle)}" }} }}\n'
                    'PACKAGES = { { spec = "test.p@r0", bundle = "b" } }\n',
                    directory=self.make_temp_dir(f"project-{kind}"),
                )
                self.assert_reported(self.install(manifest), message)


if __name__ == "__main__":
    unittest.main()
