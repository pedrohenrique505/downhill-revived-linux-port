"""Linux runtime builds must retain the production VU acceleration by default."""
import os
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import link


class LinuxVuBuild(unittest.TestCase):
    def runtime_commands(self, value):
        env = dict(os.environ)
        env.pop('DHD_BUILD_VU_AOT', None)
        if value is not None:
            env['DHD_BUILD_VU_AOT'] = value
        with tempfile.TemporaryDirectory() as out, patch.dict(os.environ, env, clear=True), \
                patch.object(link, 'LINUX', True), patch.object(link, 'OUT', out), \
                patch.object(link.sdl3, 'build', return_value=(Path(out), Path(out))), \
                patch('vu1_aot.generate', return_value=1), patch('vulkan_shaders.generate'), \
                patch.object(link, 'run', return_value=SimpleNamespace(returncode=0)) as run:
            link.runtime()
            return [call.args[0] for call in run.call_args_list]

    def assert_aot(self, value, expected):
        commands = self.runtime_commands(value)
        vu = [c for c in commands if any(str(p).endswith('/runtime/vu0_interp.c') for p in c)]
        self.assertEqual(len(vu), 1)
        self.assertEqual('-DPS2_VU_AOT' in vu[0], expected)
        others = [c for c in commands if c is not vu[0]]
        self.assertTrue(all('-DPS2_VU_AOT' not in c for c in others))

    def test_linux_default_accelerates_vu(self):
        self.assert_aot(None, True)

    def test_explicit_interpreter_build_remains_available(self):
        self.assert_aot('0', False)

    def test_explicit_aot_build(self):
        self.assert_aot('1', True)


if __name__ == '__main__':
    unittest.main()
