"""The launcher's shader preparation status with the one-file shader cache (shaders.pack): this build's prepared marker
(shaders.pack.prepared_<list>, three lines) counts only for this port's pack it names. Temporary folders: no window,
no game."""
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

PACK_LINE = 'pack 0000000000000abc'


def write_pack(folder, pack_id, version=1, port=b'Uncharted PC', magic=b'BBSHPACK'):
    """A pack header as gpu/shadps4/video_core/cache_pack.cpp writes it (the launcher does not check its checksum):
    magic, version, header size, id, time, the port's name (16 bytes, zero-padded), 8 reserved, checksum."""
    head = magic + struct.pack('<IIQQ', version, 64, pack_id, 0) + port.ljust(16, b'\0') + bytes(8) + bytes(8)
    assert len(head) == 64
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

    def marker(self, *lines, identity=True, name='shaders.pack.prepared_u3_variants'):
        """A marker file: the given lines, then (identity=True) this port's identity line."""
        lines = list(lines) + ([LAUNCHER.SHADER_MARKER_IDENTITY] if identity else [])
        (self.folder / name).write_text(''.join(line + '\n' for line in lines))

    def stamp(self, size='1234', jitter=''):
        return f'{size}{jitter} cache {LAUNCHER.SHADER_CACHE_VERSIONS}'

    def test_pack_id_read_from_header(self):
        write_pack(self.folder, 0x0123456789ABCDEF)
        self.assertEqual(LAUNCHER.shader_pack_id(self.folder), '0123456789abcdef')

    def test_other_format_or_no_pack_has_no_id(self):
        self.assertIsNone(LAUNCHER.shader_pack_id(self.folder))
        write_pack(self.folder, 5, version=2)
        self.assertIsNone(LAUNCHER.shader_pack_id(self.folder))
        (self.folder / 'shaders.pack').write_bytes(b'BBSH')
        self.assertIsNone(LAUNCHER.shader_pack_id(self.folder))

    def test_pack_of_another_port_has_no_id(self):
        write_pack(self.folder, 0xABC, port=b'Some Other Port')
        self.assertIsNone(LAUNCHER.shader_pack_id(self.folder))
        write_pack(self.folder, 0xABC, magic=b'XXSHPACK')
        self.assertIsNone(LAUNCHER.shader_pack_id(self.folder))

    def test_pack_without_port_name_is_ours(self):
        # Packs of this port's test builds before the name was added (all zero there).
        write_pack(self.folder, 0xABC, port=b'')
        self.assertEqual(LAUNCHER.shader_pack_id(self.folder), '0000000000000abc')

    def test_marker_counts_for_its_pack(self):
        write_pack(self.folder, 0xABC)
        self.marker(self.stamp(), PACK_LINE)
        self.assertTrue(LAUNCHER.shaders_prepared({}, 'u3'))

    def test_marker_with_clip_jitter_words(self):
        write_pack(self.folder, 0xABC)
        self.marker(self.stamp(jitter=' clip-jitter 3'), PACK_LINE)
        self.assertTrue(LAUNCHER.shaders_prepared({}, 'u3'))

    def test_crlf_marker_counts(self):
        write_pack(self.folder, 0xABC)
        text = '\r\n'.join([self.stamp(), PACK_LINE, LAUNCHER.SHADER_MARKER_IDENTITY, ''])
        (self.folder / 'shaders.pack.prepared_u3_variants').write_bytes(text.encode())
        self.assertTrue(LAUNCHER.shaders_prepared({}, 'u3'))

    def test_marker_of_another_pack_does_not_count(self):
        write_pack(self.folder, 0xDEF)  # cleared, or damaged records dropped: a new id
        self.marker(self.stamp(), PACK_LINE)
        self.assertFalse(LAUNCHER.shaders_prepared({}, 'u3'))

    def test_marker_of_another_list_does_not_count(self):
        write_pack(self.folder, 0xABC)
        self.marker(self.stamp(size='999'), PACK_LINE)
        self.assertFalse(LAUNCHER.shaders_prepared({}, 'u3'))

    def test_marker_of_other_cache_versions_does_not_count(self):
        write_pack(self.folder, 0xABC)
        self.marker('1234 cache 23.16.8', PACK_LINE)
        self.assertFalse(LAUNCHER.shaders_prepared({}, 'u3'))

    def test_marker_beside_another_ports_pack_does_not_count(self):
        write_pack(self.folder, 0xABC, port=b'Some Other Port')
        self.marker(self.stamp(), PACK_LINE)
        self.assertFalse(LAUNCHER.shaders_prepared({}, 'u3'))

    def test_marker_without_identity_does_not_count(self):
        write_pack(self.folder, 0xABC)
        self.marker(self.stamp(), PACK_LINE, identity=False)
        self.assertFalse(LAUNCHER.shaders_prepared({}, 'u3'))

    def test_old_marker_name_does_not_count(self):
        # An earlier build's (or another build's) prepared_<list>, even in the full format: not this build's marker.
        write_pack(self.folder, 0xABC)
        self.marker(self.stamp(), PACK_LINE, name='prepared_u3_variants')
        self.assertFalse(LAUNCHER.shaders_prepared({}, 'u3'))

    def test_folder_cache_of_an_earlier_build_does_not_count(self):
        # 0.3.6.66: one-line marker, profile.bin, no pack: the game's first start decides (moves it when it is
        # obviously this port's and of these versions); the launcher does not count it.
        (self.folder / 'profile.bin').write_bytes(b'p')
        (self.folder / 'tiling_pipelines.txt').write_text('')
        self.marker('1234', identity=False, name='prepared_u3_variants')
        self.assertFalse(LAUNCHER.shaders_prepared({}, 'u3'))

    def test_marker_without_cache_versions_does_not_count(self):
        write_pack(self.folder, 0xABC)
        self.marker('1234', PACK_LINE)
        self.assertFalse(LAUNCHER.shaders_prepared({}, 'u3'))

    def test_no_marker(self):
        write_pack(self.folder, 0xABC)
        self.assertFalse(LAUNCHER.shaders_prepared({}, 'u3'))


if __name__ == '__main__':
    unittest.main()
