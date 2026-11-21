"""The launcher's shader cache versions match the game's, and its prepared check reads the game's marker."""
import importlib.util
import re
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from paths import ROOT


SPEC = importlib.util.spec_from_file_location(
    'uncharted_launcher', ROOT / 'launcher' / 'uncharted_launcher.py')
LAUNCHER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(LAUNCHER)

SERIALIZATION = (ROOT / 'gpu' / 'shadps4' / 'video_core' / 'renderer_vulkan' /
                 'vk_pipeline_serialization.cpp')


def game_versions():
    text = SERIALIZATION.read_text(encoding='utf-8')
    parts = []
    for name in ('ShaderBinaryVersion', 'ShaderMetaVersion', 'PipelineKeyVersion'):
        match = re.search(r'constexpr\s+u32\s+' + name + r'\s*=\s*(\d+)u?\s*;', text)
        if not match:
            raise AssertionError(f'{name} not found in {SERIALIZATION}')
        parts.append(match.group(1))
    return '.'.join(parts)


class ShaderCacheVersionTests(unittest.TestCase):
    def test_launcher_matches_the_game(self):
        self.assertEqual(LAUNCHER.SHADER_CACHE_VERSIONS, game_versions())


class ShadersPreparedTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.list_file = root / 'u3_variants.txt'
        self.list_file.write_text('x' * 123)
        self.cache = root / 'cache'
        self.cache.mkdir()
        # The pack the markers are written for (gpu .../cache_pack.h header: magic, version 1, size 64, id).
        (self.cache / 'shaders.pack').write_bytes(b'BBSHPACK' + (1).to_bytes(4, 'little') + (64).to_bytes(4, 'little')
                                                  + (0xABC).to_bytes(8, 'little') + bytes(40))
        self.saved = LAUNCHER.shader_list, LAUNCHER.shader_cache_dirs
        LAUNCHER.shader_list = lambda key: [self.list_file]
        LAUNCHER.shader_cache_dirs = lambda s, key: [self.cache]

    def tearDown(self):
        LAUNCHER.shader_list, LAUNCHER.shader_cache_dirs = self.saved
        self.tmp.cleanup()

    def prepared(self, marker):
        (self.cache / 'prepared_u3_variants').write_text(marker + '\npack 0000000000000abc\n')
        return LAUNCHER.shaders_prepared(SimpleNamespace(), 'u3')

    def test_current_marker(self):
        self.assertTrue(self.prepared(f'123 cache {LAUNCHER.SHADER_CACHE_VERSIONS}'))

    def test_clip_jitter_marker(self):
        self.assertTrue(self.prepared(f'123 clip-jitter 3 cache {LAUNCHER.SHADER_CACHE_VERSIONS}'))

    def test_old_marker_without_versions(self):
        self.assertFalse(self.prepared('123'))
        self.assertFalse(self.prepared('123 clip-jitter 3'))

    def test_other_cache_versions(self):
        self.assertFalse(self.prepared('123 cache 22.16.8'))

    def test_other_list_size(self):
        self.assertFalse(self.prepared(f'124 cache {LAUNCHER.SHADER_CACHE_VERSIONS}'))

    def test_no_marker(self):
        self.assertFalse(LAUNCHER.shaders_prepared(SimpleNamespace(), 'u3'))


if __name__ == '__main__':
    unittest.main()
