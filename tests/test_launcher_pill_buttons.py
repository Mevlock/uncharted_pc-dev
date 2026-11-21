"""Every method the launcher calls on a pill button exists on PillButton (0.3.6.65: Play did nothing, because
play() hid the Open log folder pill with pack_forget, which PillButton did not have)."""
import re
import sys
import unittest

from paths import ROOT

sys.path.insert(0, str(ROOT / 'launcher'))
try:
    import uncharted_widgets
except ImportError:  # no Pillow
    uncharted_widgets = None

SOURCE = (ROOT / 'launcher' / 'uncharted_launcher.py').read_text(encoding='utf-8')


def pill_attributes():
    """The self.<name> attributes made by self.pill(...), and the methods called on them."""
    names = set(re.findall(r'self\.(\w+)\s*=\s*self\.pill\(', SOURCE))
    calls = {}
    for name in names:
        for method in re.findall(r'self\.' + name + r'\.(\w+)\(', SOURCE):
            calls.setdefault(method, set()).add(name)
    return names, calls


@unittest.skipIf(uncharted_widgets is None, 'Pillow is not installed')
class PillButtonMethodTests(unittest.TestCase):
    def test_found_the_buttons(self):
        names, _calls = pill_attributes()
        self.assertIn('play_button', names)
        self.assertIn('crash_log_button', names)

    def test_every_called_method_exists(self):
        _names, calls = pill_attributes()
        missing = {method: sorted(users) for method, users in calls.items()
                   if not hasattr(uncharted_widgets.PillButton, method)}
        self.assertEqual(missing, {})

    def test_hide_and_show_again(self):
        try:
            import tkinter as tk
            root = tk.Tk()
        except Exception as error:  # no display
            self.skipTest(f'no Tk: {error}')
        try:
            root.withdraw()
            colors = {key: '#202020' for key in ('card', 'accent', 'accent_hover', 'text', 'muted', 'line',
                                                  'panel', 'button', 'button_hover', 'disabled')}
            try:
                button = uncharted_widgets.PillButton(tk, root, 'Open log folder', lambda: None, colors,
                                                      lambda v: v, ('Segoe UI', 10))
            except KeyError as error:
                self.skipTest(f'colour table changed: {error}')
            button.pack(side='left')
            root.update_idletasks()
            self.assertEqual(button.canvas.winfo_manager(), 'pack')
            button.pack_forget()
            self.assertEqual(button.canvas.winfo_manager(), '')
            button.pack(side='left')
            self.assertEqual(button.canvas.winfo_manager(), 'pack')
        finally:
            root.destroy()


if __name__ == '__main__':
    unittest.main()
