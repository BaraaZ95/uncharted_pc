"""The launcher's hi-res output setting reaches the game as BB_HIRES_OUTPUT only with DLSS / FSR 3."""
import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'launcher'))
import uncharted_launcher as launcher


class HiResEnvironmentTests(unittest.TestCase):
    def environment(self, **changes):
        with tempfile.TemporaryDirectory() as tmp:
            settings = {**launcher.APP_DEFAULTS, 'game_dir': tmp, **changes}
            saved = {key: os.environ.get(key) for key in ('BB_INPUT_CONFIG', 'BB_HIRES_OUTPUT')}
            os.environ['BB_INPUT_CONFIG'] = str(Path(tmp) / 'input.ini')
            os.environ.pop('BB_HIRES_OUTPUT', None)
            try:
                return launcher.game_environment(settings)
            finally:
                for key, value in saved.items():
                    if value is None:
                        os.environ.pop(key, None)
                    else:
                        os.environ[key] = value

    def test_default_is_the_games_own_size(self):
        self.assertEqual(launcher.APP_DEFAULTS['output_resolution'], 'native')
        self.assertNotIn('BB_HIRES_OUTPUT', self.environment())

    def test_dlss_and_fsr3_pass_the_size(self):
        for mode in ('dlss', 'fsr3'):
            env = self.environment(anti_aliasing=mode, output_resolution='2560x1440')
            self.assertEqual(env.get('BB_HIRES_OUTPUT'), '2560x1440', mode)
            self.assertEqual(env['BB_UPSCALER'], mode)

    def test_other_anti_aliasing_modes_stay_at_1080p(self):
        for mode in ('off', 'taa', 'fsr4', 'fsr411'):
            env = self.environment(anti_aliasing=mode, output_resolution='2560x1440')
            self.assertNotIn('BB_HIRES_OUTPUT', env, mode)

    def test_native_size_stays_off(self):
        env = self.environment(anti_aliasing='dlss', output_resolution='native')
        self.assertNotIn('BB_HIRES_OUTPUT', env)
        env = self.environment(anti_aliasing='dlss', output_resolution='')
        self.assertNotIn('BB_HIRES_OUTPUT', env)

    def test_extra_environment_can_still_set_it(self):
        env = self.environment(extra_env='BB_HIRES_OUTPUT=2560x1440 BB_HIRES_PROBE=1')
        self.assertEqual(env.get('BB_HIRES_OUTPUT'), '2560x1440')
        self.assertEqual(env.get('BB_HIRES_PROBE'), '1')

    def test_choices(self):
        values = [value for value, _text in launcher.OUTPUT_RESOLUTIONS]
        self.assertEqual(values, ['native', '2560x1440'])  # 3840x2160 comes with FSR 4 support
        self.assertEqual(launcher.HIRES_ANTI_ALIASING, ('dlss', 'fsr3'))


if __name__ == '__main__':
    unittest.main()
