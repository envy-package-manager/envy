"""What each exception boundary reports for an error envy throws and one libc++ throws.

envy.test_throw (functional tester only) throws through sol's trampoline, as every envy.*
function does; envy.test_throw_past_lua skips it, so the exception unwinds to the C++
caller: main for `envy lua`, a task_engine worker for a phase, a validation thread for a
bundle. "libcxx" throws from inside libc++'s dylib, which macOS -fno-rtti code can't catch
as std::exception. Every boundary must report the message and fail; none may abort.
"""

from __future__ import annotations

import unittest

from . import test_config
from .env import EnvyTestCase

MESSAGES = {"envy": "test_throw: thrown by envy", "libcxx": "create_directory"}


class TestExceptionBoundaries(EnvyTestCase):
    def assert_reported(self, run, message: str, returncode: int = 1):
        self.assertEqual(returncode, run.returncode, test_config.describe_exit(run.result))
        self.assertIn(message, run.stderr)

    def run_lua(self, body: str):
        script = self.work / "throw.lua"
        script.write_text(body, encoding="utf-8")
        return self.run_envy("lua", script)

    def install_spec_that(self, kind: str, body: str):
        identity = f"local.throw-{kind}@r0"
        spec = self.write_spec(
            f"throw-{kind}.lua",
            f'IDENTITY = "{identity}"\nFETCH = function() end\n'
            f"INSTALL = function()\n  {body}\nend\n",
        )
        return self.install(test_config.write_spec_manifest(self.work, [(identity, spec.name)]))

    def test_lua_call(self):
        for kind, message in MESSAGES.items():
            with self.subTest(kind=kind):
                self.assert_reported(self.run_lua(f'envy.test_throw("{kind}")\n'), message)

    def test_lua_pcall(self):
        for kind, message in MESSAGES.items():
            with self.subTest(kind=kind):
                run = self.run_lua(
                    f'local ok, err = pcall(envy.test_throw, "{kind}")\n'
                    "assert(not ok)\nio.stderr:write(tostring(err))\n"
                )
                self.assert_reported(run, message, returncode=0)

    def test_phase(self):
        for kind, message in MESSAGES.items():
            with self.subTest(kind=kind):
                run = self.install_spec_that(kind, f'envy.test_throw("{kind}")')
                self.assert_reported(run, message)

    def test_past_lua_to_main(self):
        for kind, message in MESSAGES.items():
            with self.subTest(kind=kind):
                run = self.run_lua(f'envy.test_throw_past_lua("{kind}")\n')
                self.assert_reported(run, message)

    def test_past_lua_to_an_engine_worker(self):
        for kind, message in MESSAGES.items():
            with self.subTest(kind=kind):
                run = self.install_spec_that(kind, f'envy.test_throw_past_lua("{kind}")')
                self.assert_reported(run, message)

    def test_past_lua_to_a_bundle_validation_thread(self):
        for kind, message in MESSAGES.items():
            with self.subTest(kind=kind):
                bundle = self.make_temp_dir(f"bundle-{kind}")
                (bundle / "envy-bundle.lua").write_text(
                    'BUNDLE = "test.b@r0"\nSPECS = { ["test.p@r0"] = "p.lua" }\n',
                    encoding="utf-8",
                )
                (bundle / "p.lua").write_text(
                    f'envy.test_throw_past_lua("{kind}")\nIDENTITY = "test.p@r0"\n',
                    encoding="utf-8",
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
