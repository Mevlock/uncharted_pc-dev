"""Launcher input.ini generation tests."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

from paths import ROOT


SPEC = importlib.util.spec_from_file_location(
    'uncharted_launcher', ROOT / 'launcher' / 'uncharted_launcher.py')
LAUNCHER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(LAUNCHER)


class InputConfigTests(unittest.TestCase):
    def test_controller_vibration_defaults_on_and_can_be_disabled(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'input.ini'
            original = LAUNCHER.input_ini_path
            LAUNCHER.input_ini_path = lambda: path
            try:
                LAUNCHER.write_input_ini({})
                text = path.read_text(encoding='utf-8')
                self.assertIn('pad_vibration=1\n', text)
                self.assertIn('pad_vibration_strength=100\n', text)

                LAUNCHER.write_input_ini({'pad_vibration': False, 'pad_vibration_strength': '45'})
                text = path.read_text(encoding='utf-8')
                self.assertIn('pad_vibration=0\n', text)
                self.assertIn('pad_vibration_strength=45\n', text)
            finally:
                LAUNCHER.input_ini_path = original

    def test_vibration_strength_is_clamped(self):
        self.assertEqual(LAUNCHER.vibration_strength({'pad_vibration_strength': '-5'}), 0)
        self.assertEqual(LAUNCHER.vibration_strength({'pad_vibration_strength': '250'}), 100)
        self.assertEqual(LAUNCHER.vibration_strength({'pad_vibration_strength': 'invalid'}), 100)

    def test_walk_and_rotate_keys(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'input.ini'
            original = LAUNCHER.input_ini_path
            LAUNCHER.input_ini_path = lambda: path
            try:
                LAUNCHER.write_input_ini({})
                text = path.read_text(encoding='utf-8')
                self.assertIn('key.rotate_ccw=R\n', text)
                self.assertIn('key.rotate_cw=T\n', text)
                self.assertIn('key.walk=Left Ctrl\n', text)
                self.assertIn('walk_speed=65\n', text)
                # a default key the player already gave to something else stays theirs
                LAUNCHER.write_input_ini({'key_bindings': {'cross': 'R', 'square': 'Left Ctrl'}, 'walk_speed': '70'})
                text = path.read_text(encoding='utf-8')
                self.assertIn('key.rotate_ccw=\n', text)
                self.assertIn('key.walk=\n', text)
                self.assertIn('key.rotate_cw=T\n', text)
                self.assertIn('walk_speed=70\n', text)
                # bound on purpose: kept
                LAUNCHER.write_input_ini({'key_bindings': {'cross': 'R', 'rotate_ccw': 'R'}})
                self.assertIn('key.rotate_ccw=R\n', path.read_text(encoding='utf-8'))
            finally:
                LAUNCHER.input_ini_path = original
        self.assertEqual(LAUNCHER.walk_speed({'walk_speed': '5'}), 50)
        self.assertEqual(LAUNCHER.walk_speed({'walk_speed': 'x'}), 65)

    def test_controller_in_the_background_is_an_option(self):
        self.assertFalse(LAUNCHER.APP_DEFAULTS['pad_background'])

    def test_reset_all_settings_keeps_folders_languages_and_names(self):
        s = {**LAUNCHER.APP_DEFAULTS, 'game_dir': 'D:/Uncharted', 'game_dir_u2': 'D:/U2', 'user_dir': 'D:/saves',
             'ui_language': 'ru', 'language': '8', 'player_name': 'Nate', 'art_u1': 'cover.png',
             'fullscreen': True, 'mouse_sensitivity': '3.0', 'key_bindings': {'cross': 'X'},
             'pad_bindings': {'cross': 'b'}, 'u1_infinite_health': True, 'extra_env': 'BB_X=1',
             'pad_background': True, 'walk_speed': '80', 'internal_state': 'kept'}
        reset = LAUNCHER.reset_settings(s)
        for key in ('game_dir', 'game_dir_u2', 'user_dir', 'ui_language', 'language', 'player_name', 'art_u1',
                    'internal_state'):
            self.assertEqual(reset[key], s[key], key)
        for key in ('fullscreen', 'mouse_sensitivity', 'key_bindings', 'pad_bindings', 'u1_infinite_health',
                    'extra_env', 'pad_background', 'walk_speed'):
            self.assertEqual(reset[key], LAUNCHER.APP_DEFAULTS[key], key)
        reset['key_bindings']['cross'] = 'Z'  # a copy, not the defaults' own dict
        self.assertEqual(LAUNCHER.APP_DEFAULTS['key_bindings'], {})

    def test_reset_all_writes_the_defaults_and_rebuilds(self):
        from types import SimpleNamespace
        app = {**LAUNCHER.APP_DEFAULTS, 'game_dir': 'D:/Uncharted', 'fullscreen': True}
        saved, built = [], []
        fake = SimpleNamespace(app=app, vars={'fullscreen': None}, pad_vars={'cross': None},
                               current=lambda: {**app, 'fov': '120'},
                               save_settings=lambda: saved.append(dict(fake.app)),
                               rebuild=built.append, status=SimpleNamespace(configure=lambda **_k: None))
        LAUNCHER.Launcher.reset_all(fake)
        self.assertIs(fake.app, app)
        self.assertEqual(fake.vars, {})
        self.assertIsNone(fake.pad_vars)
        self.assertEqual(saved[0]['game_dir'], 'D:/Uncharted')
        self.assertFalse(saved[0]['fullscreen'])
        self.assertEqual(saved[0]['fov'], LAUNCHER.APP_DEFAULTS['fov'])
        self.assertEqual(built, ['advanced'])

    def test_controller_probe_response(self):
        info = LAUNCHER.parse_controller_probe(
            'connected=1\nname=DualSense Wireless Controller\nrumble=1\ntested=1\n')
        self.assertEqual(info, {'connected': True, 'name': 'DualSense Wireless Controller',
                                'rumble': True, 'tested': True})


if __name__ == '__main__':
    unittest.main()
