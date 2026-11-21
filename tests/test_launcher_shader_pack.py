"""The launcher's shader preparation status with the one-file shader cache (shaders.pack): the prepared marker counts
only for the pack it names. Temporary folders: no window, no game."""
import importlib.util
import struct
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from paths import ROOT


SPEC = importlib.util.spec_from_file_location(
    'uncharted_launcher_shader_pack', ROOT / 'launcher' / 'uncharted_launcher.py')
LAUNCHER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(LAUNCHER)


def write_pack(folder, pack_id, version=1):
    """A pack header as gpu/shadps4/video_core/cache_pack.cpp writes it (the launcher does not check its checksum)."""
    head = b'BBSHPACK' + struct.pack('<IIQQ', version, 64, pack_id, 0) + bytes(24) + bytes(8)
    (folder / 'shaders.pack').write_bytes(head)


class ShaderPackTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.folder = root / 'cache' / 'CUSA02320-nan1-perf'
        self.folder.mkdir(parents=True)
        self.list = root / 'u3_variants.txt'
        self.list.write_text('x' * 1234)
        patches = [patch.object(LAUNCHER, 'shader_list', lambda key: (self.list, self.list)),
                   patch.object(LAUNCHER, 'shader_cache_dirs', lambda s, key: [self.folder])]
        for p in patches:
            p.start()
            self.addCleanup(p.stop)

    def tearDown(self):
        self.tmp.cleanup()

    def marker(self, text):
        (self.folder / 'prepared_u3_variants').write_text(text)

    def test_pack_id_read_from_header(self):
        write_pack(self.folder, 0x0123456789ABCDEF)
        self.assertEqual(LAUNCHER.shader_pack_id(self.folder), '0123456789abcdef')

    def test_other_format_or_no_pack_has_no_id(self):
        self.assertIsNone(LAUNCHER.shader_pack_id(self.folder))
        write_pack(self.folder, 5, version=2)
        self.assertIsNone(LAUNCHER.shader_pack_id(self.folder))
        (self.folder / 'shaders.pack').write_bytes(b'BBSH')
        self.assertIsNone(LAUNCHER.shader_pack_id(self.folder))

    def test_marker_counts_for_its_pack(self):
        write_pack(self.folder, 0xABC)
        self.marker(f'1234 cache {LAUNCHER.SHADER_CACHE_VERSIONS}\npack 0000000000000abc\n')
        self.assertTrue(LAUNCHER.shaders_prepared({}, 'u3'))

    def test_marker_with_clip_jitter_words(self):
        write_pack(self.folder, 0xABC)
        self.marker(f'1234 clip-jitter 3 cache {LAUNCHER.SHADER_CACHE_VERSIONS}\npack 0000000000000abc\n')
        self.assertTrue(LAUNCHER.shaders_prepared({}, 'u3'))

    def test_marker_of_another_pack_does_not_count(self):
        write_pack(self.folder, 0xDEF)  # cleared, or damaged records dropped: a new id
        self.marker(f'1234 cache {LAUNCHER.SHADER_CACHE_VERSIONS}\npack 0000000000000abc\n')
        self.assertFalse(LAUNCHER.shaders_prepared({}, 'u3'))

    def test_marker_of_another_list_does_not_count(self):
        write_pack(self.folder, 0xABC)
        self.marker(f'999 cache {LAUNCHER.SHADER_CACHE_VERSIONS}\npack 0000000000000abc\n')
        self.assertFalse(LAUNCHER.shaders_prepared({}, 'u3'))

    def test_folder_cache_before_the_move_counts(self):
        (self.folder / 'profile.bin').write_bytes(b'p')
        self.marker('1234\n')
        self.assertTrue(LAUNCHER.shaders_prepared({}, 'u3'))

    def test_untied_marker_beside_a_pack_does_not_count(self):
        (self.folder / 'profile.bin').write_bytes(b'p')  # left from the move, not yet removed
        write_pack(self.folder, 0xABC)
        self.marker('1234\n')
        self.assertFalse(LAUNCHER.shaders_prepared({}, 'u3'))

    def test_marker_without_cache_versions_does_not_count(self):
        write_pack(self.folder, 0xABC)
        self.marker('1234\npack 0000000000000abc\n')
        self.assertFalse(LAUNCHER.shaders_prepared({}, 'u3'))

    def test_no_marker(self):
        write_pack(self.folder, 0xABC)
        self.assertFalse(LAUNCHER.shaders_prepared({}, 'u3'))


if __name__ == '__main__':
    unittest.main()
