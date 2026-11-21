"""Advanced > Version (the release list, the switch's file moves and backups) and the log folder, with temporary
folders: no window, no download, no game."""
import importlib.util
import io
import os
import shutil
import tempfile
import time
import unittest
import zipfile
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

    def test_odd_entries_are_left_out(self):
        odd = [1, None, {'tag_name': None}, {'tag_name': 'v0.3.6.64', 'assets': None},
               {'tag_name': 'v0.3.6.64', 'assets': [{'name': None}, 'x']}, release('v0.3.6.62')]
        self.assertEqual([n for n, *_r in LAUNCHER.release_choices(odd)], ['0.3.6.62'])

    def test_a_cut_answer_from_github_is_an_oserror(self):
        class Cut:
            def read(self, *_a):
                raise LAUNCHER.http.client.IncompleteRead(b'[', 5000)

            def __enter__(self):
                return self

            def __exit__(self, *_exc):
                return False
        with patch.object(LAUNCHER.urllib.request, 'urlopen', lambda *_a, **_k: Cut()):
            with self.assertRaises(OSError):
                LAUNCHER.github_json('https://example/releases')

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
        real_rename, calls = os.rename, []

        def failing(src, dst):
            calls.append(src)
            if len(calls) == 12:  # partway through moving the new release in
                raise PermissionError('in use')
            return real_rename(src, dst)
        with patch.object(LAUNCHER.os, 'rename', failing):
            with self.assertRaises(OSError):
                LAUNCHER.switch_files(self.install, old, self.install / 'versions' / '0.3.6.65')
        self.assertEqual(tree(self.install), original)
        self.assertEqual(tree(old), incoming)
        self.assertFalse((self.install / 'versions' / '0.3.6.65').exists())

    def test_a_folder_with_a_file_in_use_is_never_copied_and_deleted(self):
        # A file open in _internal (the old launcher or the game still running): the folder cannot be renamed. The
        # switch fails and the installation is whole; shutil.move copied the folder instead and then deleted most
        # of _internal before it reached the open file.
        original = tree(self.install)
        old = make_release(self.temp / 'download', '0.3.6.64')
        with open(self.install / '_internal' / '_internal.txt', 'rb'):
            with self.assertRaises(PermissionError):
                LAUNCHER.switch_files(self.install, old, self.install / 'versions' / '0.3.6.65')
        self.assertEqual(tree(self.install), original)
        self.assertFalse((self.install / 'versions' / '0.3.6.65').exists())

    def test_a_download_on_another_drive_is_staged_first(self):
        old = make_release(self.temp / 'download', '0.3.6.64')
        (old / 'user').mkdir()
        (old / 'user' / 'stray.txt').write_text('from the zip')
        versions = self.install / 'versions'
        with patch.object(LAUNCHER, 'same_drive', lambda a, b: False):
            staged = LAUNCHER.stage_release(old, versions)
        self.assertEqual(staged, versions / LAUNCHER.INCOMING_DIR)
        self.assertEqual(tree(staged), {k: v for k, v in tree(old).items() if not k.startswith('user/')})
        self.assertEqual(LAUNCHER.version_backups(versions), [])  # a staged release is no backup
        LAUNCHER.switch_files(self.install, staged, versions / '0.3.6.65')
        self.assertEqual((self.install / 'commit.txt').read_text(), '0.3.6.64')
        self.assertFalse(staged.exists())
        with patch.object(LAUNCHER, 'same_drive', lambda a, b: True):
            self.assertEqual(LAUNCHER.stage_release(old, versions), old)

    def test_a_failed_copy_to_the_drive_changes_nothing(self):
        original = tree(self.install)
        old = make_release(self.temp / 'download', '0.3.6.64')
        real_copy2, calls = shutil.copy2, []

        def full_disk(src, dst, *args, **kwargs):
            calls.append(src)
            if len(calls) == 4:
                raise OSError(28, 'No space left on device')
            return real_copy2(src, dst, *args, **kwargs)
        real_copytree = shutil.copytree
        with patch.object(LAUNCHER, 'same_drive', lambda a, b: False),                 patch.object(LAUNCHER.shutil, 'copy2', full_disk),                 patch.object(LAUNCHER.shutil, 'copytree',
                             lambda s, d, **k: real_copytree(s, d, copy_function=full_disk, **k)):
            with self.assertRaises(OSError):
                LAUNCHER.stage_release(old, self.install / 'versions')
        self.assertEqual({k: v for k, v in tree(self.install).items()}, original)
        self.assertFalse((self.install / 'versions' / LAUNCHER.INCOMING_DIR).exists())

    def test_a_half_deleted_backup_is_no_backup_and_is_refused(self):
        backup = make_release(self.install / 'versions' / '0.3.6.64', '0.3.6.64')
        shutil.rmtree(backup / '_internal')  # Uncharted.exe is there, but it cannot start
        self.assertEqual(LAUNCHER.version_backups(self.install / 'versions'), [])
        original = tree(self.install)
        with self.assertRaises(OSError):
            LAUNCHER.switch_files(self.install, backup, self.install / 'versions' / '0.3.6.65')
        self.assertEqual(tree(self.install), original)

    def test_a_switch_stopped_partway_leaves_no_backup_that_counts(self):
        old = make_release(self.temp / 'download', '0.3.6.64')
        real_rename, calls = os.rename, []

        class Stopped(BaseException):  # the process ended: no rollback
            pass

        def stopping(src, dst):
            calls.append(src)
            if Path(src).name == 'Uncharted.exe':
                raise Stopped
            return real_rename(src, dst)
        with patch.object(LAUNCHER.os, 'rename', stopping), self.assertRaises(Stopped):
            LAUNCHER.switch_files(self.install, old, self.install / 'versions' / '0.3.6.65')
        self.assertEqual(Path(calls[-1]).name, 'Uncharted.exe')
        self.assertTrue((self.install / 'versions' / '0.3.6.65' / '_internal').is_dir())  # all else moved first
        self.assertEqual(LAUNCHER.version_backups(self.install / 'versions'), [])

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

    def test_prune_keeps_the_most_recent(self):
        for n, version in enumerate(('0.3.6.62', '0.3.6.64', '0.3.6.63')):
            make_release(self.versions / version, version)
            stamp = time.time() - 100 + n
            os.utime(self.versions / version, (stamp, stamp))
        (self.versions / 'half').mkdir()
        LAUNCHER.prune_backups(self.versions)
        self.assertEqual(sorted(p.name for p in self.versions.iterdir()), ['0.3.6.63'])

    def test_size_and_removal(self):
        make_release(self.versions / '0.3.6.64', '0.3.6.64')
        (self.versions / '0.3.6.64' / 'bin' / 'big.dll').write_bytes(b'x' * 3 * (1 << 20))
        size = LAUNCHER.folder_size(self.versions)
        self.assertEqual(size, sum(p.stat().st_size for p in self.versions.rglob('*') if p.is_file()))
        self.assertEqual(round(size / (1 << 20)), 3)
        LAUNCHER.remove_backups(self.versions)
        self.assertFalse(self.versions.exists())
        self.assertEqual(LAUNCHER.folder_size(self.versions), 0)
        LAUNCHER.remove_backups(self.versions)  # nothing left: no error

    def test_read_only_files_are_removed_too(self):
        make_release(self.versions / '0.3.6.64', '0.3.6.64')
        make_release(self.versions / '0.3.6.62', '0.3.6.62')
        os.utime(self.versions / '0.3.6.62', (time.time() - 100, time.time() - 100))
        for version in ('0.3.6.64', '0.3.6.62'):
            os.chmod(self.versions / version / 'bin' / 'bin.txt', 0o444)
        LAUNCHER.prune_backups(self.versions)
        self.assertEqual(sorted(p.name for p in self.versions.iterdir()), ['0.3.6.64'])
        LAUNCHER.remove_backups(self.versions)
        self.assertFalse(self.versions.exists())

    def test_folder_empty(self):
        self.assertTrue(LAUNCHER.folder_empty(self.versions))
        self.assertTrue(LAUNCHER.folder_empty(self.versions / 'missing'))
        (self.versions / 'half').mkdir()
        self.assertFalse(LAUNCHER.folder_empty(self.versions))

    def test_no_versions_folder(self):
        self.assertEqual(LAUNCHER.version_backups(self.versions / 'missing'), [])
        LAUNCHER.prune_backups(self.versions / 'missing')

    def test_backup_folder_names(self):
        self.assertEqual(LAUNCHER.backup_folder_name('0.3.6.58.1-test-reflex'), '0.3.6.58.1-test-reflex')
        self.assertEqual(LAUNCHER.backup_folder_name('0.3.6.65 (local)'), '0.3.6.65__local_')


class FakeResponse:
    """urlopen's answer: body, with a Content-Length of length (the whole body by default)."""
    def __init__(self, body, length=None):
        self.body, self.headers = io.BytesIO(body), {'Content-Length': str(len(body) if length is None else length)}

    def read(self, size=-1):
        return self.body.read(size)

    def __enter__(self):
        return self

    def __exit__(self, *_exc):
        return False


def release_zip_bytes():
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, 'w', zipfile.ZIP_DEFLATED) as package:
        package.writestr('uncharted-windows/Uncharted.exe', b'launcher')
        package.writestr('uncharted-windows/bin/big.dll', os.urandom(1 << 16) + b'x' * (1 << 18))
    return buffer.getvalue()


class DownloadTests(unittest.TestCase):
    def setUp(self):
        self.temp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.temp, True)
        self.update_dir = self.temp / 'uncharted-update'
        patcher = patch.object(LAUNCHER, 'UPDATE_DIR', self.update_dir)
        patcher.start()
        self.addCleanup(patcher.stop)

    def download(self, response):
        with patch.object(LAUNCHER.urllib.request, 'urlopen', lambda *_a, **_k: response):
            return LAUNCHER.download_release('https://example/release.zip', lambda _percent: None)

    def test_a_whole_download(self):
        new = self.download(FakeResponse(release_zip_bytes()))
        self.assertEqual((new / 'Uncharted.exe').read_bytes(), b'launcher')
        self.assertFalse((self.update_dir / 'update.zip').exists())

    def test_a_damaged_zip_is_an_oserror_and_leaves_nothing(self):
        data = bytearray(release_zip_bytes())
        start = data.find(b'big.dll') + len(b'big.dll') + 2000
        data[start:start + 200] = bytes(200)  # deflate data of a member overwritten (zlib.error, not BadZipFile)
        with self.assertRaises(OSError):
            self.download(FakeResponse(bytes(data)))
        self.assertFalse(self.update_dir.exists())

    def test_a_cut_download_says_so_and_leaves_nothing(self):
        data = release_zip_bytes()
        with self.assertRaises(OSError) as raised:
            self.download(FakeResponse(data[:len(data) // 2], length=len(data)))
        self.assertIn('stopped', str(raised.exception))
        self.assertFalse(self.update_dir.exists())

    def test_not_a_zip(self):
        with self.assertRaises(OSError):
            self.download(FakeResponse(b'<html>rate limited</html>'))
        self.assertFalse(self.update_dir.exists())


class LogLocationTests(unittest.TestCase):
    def setUp(self):
        self.temp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.temp, True)

    def test_the_saves_folder_holds_the_log(self):
        saves = self.temp / 'saves'
        saves.mkdir()
        self.assertEqual(LAUNCHER.log_location({'user_dir': str(saves)}), (saves, None))
        (saves / 'last_run.log').write_text('log')
        self.assertEqual(LAUNCHER.log_location({'user_dir': str(saves)}), (saves, saves / 'last_run.log'))

    def test_without_a_saves_folder_user_next_to_the_launcher(self):
        with patch.object(LAUNCHER, 'DATA_DIR', self.temp):
            self.assertEqual(LAUNCHER.log_location({'user_dir': ''}), (self.temp / 'user', None))
            (self.temp / 'user').mkdir()
            (self.temp / 'user' / 'last_run.log').write_text('log')
            self.assertEqual(LAUNCHER.log_location({}), (self.temp / 'user', self.temp / 'user' / 'last_run.log'))


if __name__ == '__main__':
    unittest.main()
