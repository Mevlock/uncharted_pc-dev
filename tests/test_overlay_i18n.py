"""The in-game menu's translations: every text marked T(), TI(), N_() or tr() in bbport_overlay.cpp and
bbport_saves.cpp has es/pt/fr/de/it/ja with the same placeholders, the generated .inc is current, the Japanese font
has every character, and the launcher hands its language to the game (BB_UI_LANGUAGE)."""
from paths import ROOT
import importlib.util
import os
from pathlib import Path
import sys
import tempfile
import unittest


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


class OverlayTranslationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tool = load('make_overlay_i18n', 'tools/make_overlay_i18n.py')
        cls.data = cls.tool.load_data()
        cls.found = cls.tool.marked_texts()

    def test_texts_are_found(self):
        # a sample from each file, so a broken scan does not pass as "nothing to check"
        for text in ('Save slots', 'Now: %s', 'Saved in slot {}.', 'Chapter {}', 'Platinum trophy earned'):
            self.assertIn(text, self.found)
        self.assertGreater(len(self.found), 100)

    def test_every_marked_text_has_six_languages(self):
        self.assertEqual(self.tool.problems(self.found, self.data), [])

    def test_generated_table_is_current(self):
        self.assertEqual(self.tool.OUTPUT.read_text(encoding='utf-8'), self.tool.render(self.data))

    def test_placeholder_check_catches_a_change(self):
        broken = {'Saved in slot {}.': {**self.data['Saved in slot {}.'], 'de': 'Gespeichert.'}}
        self.assertTrue(self.tool.problems({'Saved in slot {}.': 'x'}, broken))
        broken = {'Now: %s': {**self.data['Now: %s'], 'fr': 'Actuellement : %d'}}
        self.assertTrue(self.tool.problems({'Now: %s': 'x'}, broken))

    def test_japanese_font_has_every_character(self):
        try:
            from fontTools.ttLib import TTFont
        except ImportError:
            self.skipTest('fontTools not installed')
        cmap = TTFont(ROOT / 'gpu/third_party/fonts/NotoSansJP-overlay.ttf').getBestCmap()
        missing = sorted({ch for row in self.data.values() for ch in row['ja'] if ord(ch) > 0x7F and ord(ch) not in cmap})
        self.assertEqual(missing, [], 'run tools/make_overlay_font_ja.py')


class LauncherLanguageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.input_dir = tempfile.TemporaryDirectory()
        os.environ['BB_INPUT_CONFIG'] = str(Path(cls.input_dir.name) / 'input.ini')
        sys.path.insert(0, str(ROOT / 'launcher'))
        cls.launcher = load('uncharted_launcher', 'launcher/uncharted_launcher.py')

    @classmethod
    def tearDownClass(cls):
        os.environ.pop('BB_INPUT_CONFIG', None)
        cls.input_dir.cleanup()

    def test_game_gets_the_launcher_language(self):
        for choice in ('es', 'pt', 'fr', 'de', 'it', 'ja', 'en'):
            with self.subTest(choice=choice):
                env = self.launcher.game_environment({**self.launcher.APP_DEFAULTS, 'ui_language': choice})
                self.assertEqual(env['BB_UI_LANGUAGE'], choice)


if __name__ == '__main__':
    unittest.main()
