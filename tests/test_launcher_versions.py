"""Advanced > Version (the release list, the switch's file moves and backups), with temporary folders: no window,
no download, no game."""
import importlib.util
import os
import shutil
import tempfile
import time
import unittest
from pathlib import Path
from unittest.mock import patch

from paths import ROOT


SPEC = importlib.util.spec_from_file_location(
    'uncharted_launcher_versions', ROOT / 'launcher' / 'uncharted_launcher.py')
LAUNCHER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(LAUNCHER)


def release(tag, prerelease=False, draft=False, zip_file=True):
    assets = [{'name': f'uncharted-windows-{tag}.zip', 'browser_download_url': f'https://example/{tag}.zip'}] \
        if zip_file else [{'name': 'notes.txt', 'browser_download_url': 'https://example/notes.txt'}]
    return {'tag_name': tag, 'prerelease': prerelease, 'draft': draft, 'assets': assets,
            'html_url': f'https://example/releases/{tag}'}


RELEASES = [release('v0.3.6.64'), release('v0.3.6.58.1-test-reflex', prerelease=True), release('v0.3.6.58'),
            release('v0.3.6.62'), release('v0.3.6.57'), release('v0.3.6.56'), release('v0.3.6.29-pre'),
            release('v0.3.6.66', draft=True), release('v0.3.6.63', zip_file=False),
            release('v0.3.6.58.1'), release('v0.3.6.65')]


class ReleaseChoiceTests(unittest.TestCase):
    def test_releases_newest_first_none_older_than_057(self):
        names = [name for name, *_rest in LAUNCHER.release_choices(RELEASES)]
        self.assertEqual(names, ['0.3.6.65', '0.3.6.64', '0.3.6.62', '0.3.6.58.1', '0.3.6.58', '0.3.6.57'])

    def test_test_builds_only_with_the_switch(self):
        names = [name for name, *_rest in LAUNCHER.release_choices(RELEASES, test_builds=True)]
        self.assertEqual(names, ['0.3.6.65', '0.3.6.64', '0.3.6.62', '0.3.6.58.1', '0.3.6.58.1-test-reflex',
                                 '0.3.6.58', '0.3.6.57'])
        test = {name: flag for name, _url, _page, flag in LAUNCHER.release_choices(RELEASES, test_builds=True)}
        self.assertTrue(test['0.3.6.58.1-test-reflex'])
        self.assertFalse(test['0.3.6.58.1'])

    def test_drafts_and_releases_without_a_zip_are_left_out(self):
        names = [name for name, *_rest in LAUNCHER.release_choices(RELEASES, test_builds=True)]
        self.assertNotIn('0.3.6.66', names)
        self.assertNotIn('0.3.6.63', names)

    def test_entries_carry_the_zip_and_the_page(self):
        name, url, page, test = LAUNCHER.release_choices([release('v0.3.6.64')])[0]
        self.assertEqual((name, url, page, test),
                         ('0.3.6.64', 'https://example/v0.3.6.64.zip', 'https://example/releases/v0.3.6.64', False))

    def test_version_ordering_is_numeric(self):
        names = [n for n, *_r in LAUNCHER.release_choices([release('v0.3.6.9'), release('v0.3.6.100'),
                                                            release('v0.3.6.57')], oldest=(0, 3, 6))]
        self.assertEqual(names, ['0.3.6.100', '0.3.6.57', '0.3.6.9'])


def make_release(folder, version):
    """A small stand-in for an unpacked Windows package of version."""
    folder.mkdir(parents=True, exist_ok=True)
    (folder / 'Uncharted.exe').write_text(f'launcher {version}')
    (folder / 'Play Uncharted.exe').write_text(f'play {version}')
    (folder / 'run.py').write_text(f'run {version}')
    (folder / 'commit.txt').write_text(version)
    for sub in ('_internal', 'bin', 'shaders', 'notes', 'scripts'):
        (folder / sub).mkdir(exist_ok=True)
        (folder / sub / f'{sub}.txt').write_text(f'{sub} {version}')
    return folder


def tree(folder):
    """{relative path: content} of every file under folder."""
    return {p.relative_to(folder).as_posix(): p.read_text() for p in sorted(folder.rglob('*')) if p.is_file()}


class SwitchFileTests(unittest.TestCase):
    def setUp(self):
        self.temp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.temp, True)
        self.install = make_release(self.temp / 'install', '0.3.6.65')
        # What a player has in the installation: saves, shader cache, logs, settings, the FSR 4 download.
        (self.install / 'user' / 'cache').mkdir(parents=True)
        (self.install / 'user' / 'cache' / 'pipelines.bin').write_text('cache')
        (self.install / 'user' / 'last_run.log').write_text('log')
        (self.install / 'launcher_settings.json').write_text('{"frame_pacing": "off"}')
        (self.install / 'bbport.ini').write_text('show_fps=1')
        (self.install / 'input.ini').write_text('keys')
        (self.install / 'fsr4_shaders').mkdir()
        (self.install / 'fsr4_shaders' / 'model.bin').write_text('fsr4')
        self.player_files = {k: v for k, v in tree(self.install).items()
                             if k.split('/')[0] in LAUNCHER.SWITCH_KEEP}

    def test_switch_keeps_player_files_and_backs_up_the_release(self):
        old = make_release(self.temp / 'download' / 'uncharted-windows', '0.3.6.64')
        (old / 'only_in_064.txt').write_text('x')
        backup = self.install / 'versions' / '0.3.6.65'
        before = {k: v for k, v in tree(self.install).items() if k not in self.player_files}
        LAUNCHER.switch_files(self.install, old, backup)
        self.assertEqual(tree(backup), before)
        self.assertEqual((self.install / 'commit.txt').read_text(), '0.3.6.64')
        self.assertTrue((self.install / 'only_in_064.txt').is_file())
        self.assertEqual({k: v for k, v in tree(self.install).items() if k.split('/')[0] in LAUNCHER.SWITCH_KEEP
                          and not k.startswith('versions/')}, self.player_files)
        self.assertFalse(old.exists())  # moved in, the emptied folder removed

    def test_back_restores_exactly_and_newer_only_files_leave(self):
        (self.install / 'bin' / 'new_in_065.dll').write_text('new')
        original = tree(self.install)
        old = make_release(self.temp / 'download', '0.3.6.64')
        LAUNCHER.switch_files(self.install, old, self.install / 'versions' / '0.3.6.65')
        self.assertFalse((self.install / 'bin' / 'new_in_065.dll').exists())  # no mix of two releases
        LAUNCHER.switch_files(self.install, self.install / 'versions' / '0.3.6.65',
                              self.install / 'versions' / '0.3.6.64')
        now = tree(self.install)
        self.assertEqual({k: v for k, v in now.items() if not k.startswith('versions/')}, original)
        self.assertEqual(tree(self.install / 'versions' / '0.3.6.64')['commit.txt'], '0.3.6.64')
        self.assertFalse((self.install / 'versions' / '0.3.6.65').exists())

    def test_a_failed_move_puts_everything_back(self):
        original = tree(self.install)
        old = make_release(self.temp / 'download', '0.3.6.64')
        incoming = tree(old)
        real_move, calls = shutil.move, []

        def failing(src, dst):
            calls.append(src)
            if len(calls) == 12:  # partway through moving the new release in
                raise PermissionError('in use')
            return real_move(src, dst)
        with patch.object(LAUNCHER.shutil, 'move', failing):
            with self.assertRaises(OSError):
                LAUNCHER.switch_files(self.install, old, self.install / 'versions' / '0.3.6.65')
        self.assertEqual(tree(self.install), original)
        self.assertEqual(tree(old), incoming)
        self.assertFalse((self.install / 'versions' / '0.3.6.65').exists())

    def test_a_source_without_the_launcher_is_refused(self):
        empty = self.temp / 'empty'
        (empty / 'bin').mkdir(parents=True)
        original = tree(self.install)
        with self.assertRaises(OSError):
            LAUNCHER.switch_files(self.install, empty, self.install / 'versions' / '0.3.6.65')
        self.assertEqual(tree(self.install), original)

    def test_player_files_in_a_download_are_not_moved_in(self):
        old = make_release(self.temp / 'download', '0.3.6.64')
        (old / 'user').mkdir()
        (old / 'user' / 'stray.txt').write_text('from the zip')
        (old / 'bbport.ini').write_text('from the zip')
        LAUNCHER.switch_files(self.install, old, self.install / 'versions' / '0.3.6.65')
        self.assertEqual((self.install / 'bbport.ini').read_text(), 'show_fps=1')
        self.assertFalse((self.install / 'user' / 'stray.txt').exists())


class BackupTests(unittest.TestCase):
    def setUp(self):
        self.versions = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.versions, True)

    def test_backups_newest_first_and_only_whole_ones(self):
        for n, version in enumerate(('0.3.6.62', '0.3.6.64', '0.3.6.63')):
            make_release(self.versions / version, version)
            stamp = time.time() - 100 + n
            os.utime(self.versions / version, (stamp, stamp))
        (self.versions / 'half').mkdir()
        self.assertEqual([v for v, _p in LAUNCHER.version_backups(self.versions)], ['0.3.6.63', '0.3.6.64', '0.3.6.62'])

    def test_prune_keeps_the_two_most_recent(self):
        for n, version in enumerate(('0.3.6.62', '0.3.6.64', '0.3.6.63')):
            make_release(self.versions / version, version)
            stamp = time.time() - 100 + n
            os.utime(self.versions / version, (stamp, stamp))
        (self.versions / 'half').mkdir()
        LAUNCHER.prune_backups(self.versions)
        self.assertEqual(sorted(p.name for p in self.versions.iterdir()), ['0.3.6.63', '0.3.6.64'])

    def test_no_versions_folder(self):
        self.assertEqual(LAUNCHER.version_backups(self.versions / 'missing'), [])
        LAUNCHER.prune_backups(self.versions / 'missing')

    def test_backup_folder_names(self):
        self.assertEqual(LAUNCHER.backup_folder_name('0.3.6.58.1-test-reflex'), '0.3.6.58.1-test-reflex')
        self.assertEqual(LAUNCHER.backup_folder_name('0.3.6.65 (local)'), '0.3.6.65__local_')


if __name__ == '__main__':
    unittest.main()
