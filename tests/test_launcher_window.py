"""Launcher window plumbing without a Tk window: the UI pump and closing while the game runs."""
import importlib.util
import queue
import threading
import unittest
from types import SimpleNamespace

from paths import ROOT


SPEC = importlib.util.spec_from_file_location(
    'uncharted_launcher', ROOT / 'launcher' / 'uncharted_launcher.py')
LAUNCHER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(LAUNCHER)
Launcher = LAUNCHER.Launcher


class FakeRoot:
    def __init__(self):
        self.after_calls, self.destroyed = [], False

    def after(self, delay, callback):
        self.after_calls.append((delay, callback))

    def destroy(self):
        self.destroyed = True


class DrainOutputTests(unittest.TestCase):
    def test_a_failing_ui_call_keeps_the_pump_running(self):
        ran = []

        def broken():  # e.g. TclError from a label a language change destroyed
            raise RuntimeError('invalid command name ".!label"')
        fake = SimpleNamespace(root=FakeRoot(), ui_calls=queue.Queue(), output=queue.Queue(), drain_output=None)
        fake.drain_once = lambda: Launcher.drain_once(fake)
        fake.ui_calls.put(broken)
        fake.ui_calls.put(lambda: ran.append('next'))
        Launcher.drain_output(fake)
        self.assertEqual(ran, ['next'])
        self.assertEqual(len(fake.root.after_calls), 1)

    def test_the_pump_reschedules_when_the_output_loop_fails(self):
        def broken():
            raise RuntimeError('boom')
        fake = SimpleNamespace(root=FakeRoot(), drain_once=broken, drain_output=None)
        Launcher.drain_output(fake)
        self.assertEqual(len(fake.root.after_calls), 1)


class CloseTests(unittest.TestCase):
    def test_closing_by_hand_keeps_reading_the_game_pipe(self):
        release = threading.Event()
        reader = threading.Thread(target=release.wait, daemon=True)
        reader.start()
        fake = SimpleNamespace(root=FakeRoot(), reader=reader, closed=False, collect=lambda: None)
        Launcher.close(fake)
        self.assertTrue(fake.root.destroyed)
        self.assertTrue(fake.closed)  # read_output stops filling the Log page queue
        waiter = threading.Thread(target=Launcher.wait_for_game, args=(fake,), daemon=True)
        waiter.start()
        waiter.join(0.2)
        self.assertTrue(waiter.is_alive(), 'the process must wait for the game output to end')
        release.set()
        waiter.join(2)
        self.assertFalse(waiter.is_alive())

    def test_no_game_no_wait(self):
        Launcher.wait_for_game(SimpleNamespace(reader=None))


if __name__ == '__main__':
    unittest.main()
