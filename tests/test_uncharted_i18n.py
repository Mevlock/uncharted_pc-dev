import ast
import re
import sys
import unittest
from types import SimpleNamespace
from unittest.mock import patch

from paths import ROOT

sys.path.insert(0, str(ROOT / 'launcher'))
import uncharted_i18n


class LauncherTranslationTests(unittest.TestCase):
    def test_each_language_translates_play(self):
        expected = {
            'es': 'Jugar', 'pt': 'Jogar', 'fr': 'Jouer', 'de': 'Spielen',
            'it': 'Gioca', 'ja': 'プレイ',
        }
        for language, translation in expected.items():
            with self.subTest(language=language):
                self.assertEqual(uncharted_i18n.translate(language, 'Play'), translation)

    def test_missing_text_falls_back_to_english(self):
        self.assertEqual(uncharted_i18n.translate('es', 'Untranslated text'), 'Untranslated text')
        self.assertEqual(uncharted_i18n.translate('en', 'Play'), 'Play')

    def test_automatic_language_uses_windows_ui_language(self):
        # LANGIDs as GetUserDefaultUILanguage() returns them; locale.getlocale() on Windows gives
        # names such as 'Spanish_Spain', which must not decide the language.
        langids = {0x0c0a: 'es', 0x0416: 'pt', 0x0816: 'pt', 0x040c: 'fr', 0x0407: 'de', 0x0410: 'it',
                   0x0411: 'ja', 0x0809: 'en', 0x0412: 'en', 0x0419: 'en'}
        for langid, language in langids.items():
            windows = SimpleNamespace(windll=SimpleNamespace(kernel32=SimpleNamespace(
                GetUserDefaultUILanguage=lambda langid=langid: langid)))
            with self.subTest(langid=hex(langid)), patch.object(uncharted_i18n, 'ctypes', windows),                     patch.object(uncharted_i18n.locale, 'getlocale', return_value=('Spanish_Spain', '1252')):
                self.assertEqual(uncharted_i18n.language_code(''), language)

    def test_automatic_language_uses_locale_off_windows(self):
        no_windll = SimpleNamespace()
        for language in ('es', 'pt', 'fr', 'de', 'it', 'ja'):
            with self.subTest(language=language), patch.object(uncharted_i18n, 'ctypes', no_windll),                     patch.object(uncharted_i18n.locale, 'getlocale', return_value=(f'{language}_XX', 'UTF-8')):
                self.assertEqual(uncharted_i18n.language_code(''), language)
        with patch.object(uncharted_i18n, 'ctypes', no_windll),                 patch.object(uncharted_i18n.locale, 'getlocale', return_value=('ko_KR', 'UTF-8')):
            self.assertEqual(uncharted_i18n.language_code(''), 'en')

    def test_launcher_offers_selected_languages(self):
        self.assertEqual(
            [code for code, _name in uncharted_i18n.LANGUAGES],
            ['', 'en', 'es', 'pt', 'fr', 'de', 'it', 'ja'],
        )

    def test_interface_language_help_is_translated(self):
        text = 'This changes only the launcher. The game language is selected on the Game page.'
        self.assertNotEqual(uncharted_i18n.translate('es', text), text)

    def test_each_translation_covers_every_launcher_text(self):
        tree = ast.parse((ROOT / 'launcher' / 'uncharted_launcher.py').read_text(encoding='utf-8'))
        texts = {node.args[0].value for node in ast.walk(tree)
                 if isinstance(node, ast.Call) and isinstance(node.func, ast.Name)
                 and node.func.id == '_' and node.args
                 and isinstance(node.args[0], ast.Constant) and isinstance(node.args[0].value, str)}
        for language in ('es', 'pt', 'fr', 'de', 'it', 'ja'):
            with self.subTest(language=language):
                self.assertEqual(texts - set(uncharted_i18n.TRANSLATIONS[language]), set())

    def test_each_translation_covers_every_drop_down_label(self):
        labels = drop_down_labels()
        self.assertIn('Automatic (follows the device you used last)', labels)
        self.assertIn('Intel XeSS (Native AA, any GPU)', labels)  # added to the list at run time
        self.assertIn('Before playing (recommended)', labels)  # inline list
        self.assertIn('Not assigned', labels)  # the controller page's own drop-downs
        self.assertIn('Portuguese (Brazil)', labels)
        self.assert_translated(labels)

    def test_each_translation_covers_texts_shown_through_variables(self):
        tree = ast.parse((ROOT / 'launcher' / 'uncharted_launcher.py').read_text(encoding='utf-8'))
        sites = {ast.unparse(node.args[0]) for node in ast.walk(tree)
                 if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id == '_'
                 and node.args and not isinstance(node.args[0], ast.Constant)}
        # A new _(variable) needs its source list added to variable_texts() (and here).
        self.assertEqual(sites, {'t', 'title', 'subtitle', "subtitle.split(' · ')[0]", 'RESERVED_KEYS[sym]',
                                 'trophies.GRADE_NAMES[g]', "trophies.GRADE_NAMES.get(grade, 'Bronze')"})
        texts = variable_texts()
        self.assertIn('Cross ✕', texts)
        self.assertIn('Look left', texts)
        self.assertIn('Platinum', texts)
        self.assert_translated(texts)

    def assert_translated(self, labels):
        texts = {t for t in labels if t not in KEEP_ENGLISH and re.search('[a-z]{2}', t)}
        for language in ('es', 'pt', 'fr', 'de', 'it', 'ja'):
            table = uncharted_i18n.TRANSLATIONS[language]
            with self.subTest(language=language):
                self.assertEqual(texts - set(table), set())
                for text in texts:
                    self.assertEqual(re.findall(r'\{[^}]*\}', table[text]), re.findall(r'\{[^}]*\}', text), text)


# Option labels made only of names, kept as they are in every language (labels without a lower-case
# word, such as '120 FPS' or 'LB / L1', need no translation either).
KEEP_ENGLISH = {'FIFO Relaxed', 'NVIDIA DLSS / DLAA (GeForce RTX)',
                'NVIDIA DLSS Frame Generation (GeForce RTX 40/50)', 'Start / Options', 'Back / Share',
                # PS4 button name and the games' titles.
                'Options', "Drake's Fortune", 'Among Thieves', "Drake's Deception",
                'Uncharted 1', 'Uncharted 2', 'Uncharted 3',
                'Uncharted 1 · 2007', 'Uncharted 2 · 2009', 'Uncharted 3 · 2011'}


def module_constants(path):
    tree = ast.parse(path.read_text(encoding='utf-8'))
    values = {}
    for node in tree.body:
        if isinstance(node, ast.Assign) and isinstance(node.targets[0], ast.Name):
            try:
                values[node.targets[0].id] = ast.literal_eval(node.value)
            except ValueError:
                pass
    return values


def variable_texts():
    """The texts the launcher passes to _() as variables: control names, game titles, notes, trophy grades."""
    launcher = module_constants(ROOT / 'launcher' / 'uncharted_launcher.py')
    trophies = module_constants(ROOT / 'scripts' / 'trophies.py')
    texts = {row[1] for row in launcher['BUTTON_ACTIONS'] + launcher['STICK_ACTIONS'] + launcher['PAD_BUTTONS']}
    for _game, title, subtitle, _elf in launcher['GAMES']:
        texts |= {title, subtitle, subtitle.split(' · ')[0]}
    texts |= set(launcher['RESERVED_KEYS'].values()) | set(trophies['GRADE_NAMES'].values())
    return texts


def drop_down_labels():
    """The labels of every list the launcher shows in a drop-down (choice() or a Combobox of _(label)s)."""
    tree = ast.parse((ROOT / 'launcher' / 'uncharted_launcher.py').read_text(encoding='utf-8'))
    constants = {node.targets[0].id: node.value for node in tree.body
                 if isinstance(node, ast.Assign) and isinstance(node.targets[0], ast.Name)}
    locals_ = {node.targets[0].id: node.value for node in ast.walk(tree)
               if isinstance(node, ast.Assign) and isinstance(node.targets[0], ast.Name)
               and node not in tree.body}
    labels = set()

    def collect(node, seen=()):
        if isinstance(node, ast.Name):
            value = constants.get(node.id, locals_.get(node.id))
            if value is not None and node.id not in seen:
                collect(value, seen + (node.id,))
        elif isinstance(node, ast.Tuple) and len(node.elts) == 2 and all(
                isinstance(e, ast.Constant) and isinstance(e.value, str) for e in node.elts):
            labels.add(node.elts[1].value)
        else:
            for child in ast.iter_child_nodes(node):
                collect(child, seen)

    for node in ast.walk(tree):
        if not isinstance(node, ast.Call) or not isinstance(node.func, ast.Attribute):
            continue
        if node.func.attr == 'choice' and len(node.args) >= 3:
            collect(node.args[2])
        elif node.func.attr == 'Combobox':
            for keyword in node.keywords:
                if keyword.arg == 'values':
                    for comp in ast.walk(keyword.value):
                        if isinstance(comp, ast.comprehension):
                            collect(comp.iter)
    return labels


if __name__ == '__main__':
    unittest.main()
