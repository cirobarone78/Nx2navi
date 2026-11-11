"""Launcher Game Settings round trips against synthetic nx2navi.ini files."""
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from launcher import gameini


class GameOptionsTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.game = Path(tmp.name)
        self.ini = self.game / 'nx2navi.ini'

    def test_grouped_save_and_unrelated_settings(self):
        untouched = b'\xef\xbb\xbf# Some notes\r\n[mods]\r\nmods_order = "Custom \xe9"\r\n'
        multipliers = b'fg_mult = dynamic\r\nframegen_multiplier = 4\r\n'
        self.ini.write_bytes(untouched + b'[display]\r\n' + multipliers + b'[frame rate]\r\nFPS_LIMIT = 73\r\n')
        original = gameini.read_options(self.game)
        self.assertEqual(original['fps_limit'], '73')
        self.assertTrue(all(v == 'default' for k, v in original.items() if k != 'fps_limit'))
        gameini.save_options(self.game, {'upscaling': 'dlss-quality', 'framegen': 'dlss', 'game_res': '2160',
                                        'microphone': '0', 'fullscreen': '1', 'perf_overlay': 'full'}, original)
        self.assertEqual(gameini.get(self.game, 'dlaa'), '1')
        self.assertEqual(gameini.get(self.game, 'taa'), 'dlss')
        self.assertEqual(gameini.get(self.game, 'dlss'), 'quality')
        self.assertEqual(gameini.get(self.game, 'upscaler'), 'bilinear')
        self.assertEqual(gameini.read_options(self.game), {'fps_limit': '73', 'upscaling': 'dlss-quality',
                         'framegen': 'dlss', 'game_res': '2160', 'microphone': '0', 'fullscreen': '1', 'perf_overlay': 'full'})
        self.assertTrue(self.ini.read_bytes().startswith(untouched))
        self.assertIn(multipliers, self.ini.read_bytes())
        self.assertNotIn(b'\n', self.ini.read_bytes().replace(b'\r\n', b''))

    def test_temporal_modes_and_dependency(self):
        for option in gameini.CHOICES['upscaling']:
            if not option.startswith(('fsr3-', 'dlss-')):
                continue
            gameini.save_options(self.game, {'upscaling': option, 'framegen': 'fsr3'})
            self.assertEqual(gameini.read_options(self.game)['upscaling'], option)
        gameini.save_options(self.game, {'upscaling': 'fsr'})
        self.assertEqual(gameini.get(self.game, 'dlaa'), '0')
        self.assertEqual(gameini.get(self.game, 'framegen'), 'off')
        before = self.ini.read_bytes()
        with self.assertRaises(ValueError):
            gameini.save_options(self.game, {'fps_limit': '60', 'framegen': 'dlss'})
        self.assertEqual(before, self.ini.read_bytes())
        gameini.save_options(self.game, {'upscaling': 'default', 'framegen': 'default'})
        self.assertTrue(all(v == 'default' for v in gameini.read_options(self.game).values()))

    def test_legacy_fps_duplicates_and_reset(self):
        self.ini.write_text('fps60 = 0\n# fps_limit = 120\nNX2NAVI_FPS_LIMIT = "75"\nfps_limit =\n')
        self.assertEqual(gameini.read_options(self.game)['fps_limit'], '75')
        gameini.save_options(self.game, {'fps_limit': 'default'})
        self.assertEqual(self.ini.read_bytes(), b'# fps_limit = 120\n')
        self.ini.write_text('FPS60=0\n')
        self.assertEqual(gameini.read_options(self.game)['fps_limit'], '30')

    def test_three_way_merge_and_conflict(self):
        original = gameini.read_options(self.game)
        gameini.put(self.game, 'audio', 'volume', '24')
        gameini.save_options(self.game, {'upscaling': 'fsr3-balanced'}, original)
        self.assertEqual(gameini.get(self.game, 'volume'), '24')
        before = self.ini.read_bytes()
        with self.assertRaises(OSError):
            gameini.save_options(self.game, {'upscaling': 'dlss-performance', 'fullscreen': '1'}, original)
        self.assertEqual(self.ini.read_bytes(), before)

    def test_atomic_failure_preserves_file_and_default_does_not_create_one(self):
        gameini.save_options(self.game, {'microphone': 'default'})
        self.assertFalse(self.ini.exists())
        self.ini.write_bytes(b'# my game\n')
        with mock.patch.object(os, 'replace', side_effect=OSError('busy')), self.assertRaises(OSError):
            gameini.save_options(self.game, {'upscaling': 'dlss-balanced', 'fps_limit': '120'})
        self.assertEqual(self.ini.read_bytes(), b'# my game\n')
        self.assertEqual(list(self.game.iterdir()), [self.ini])


if __name__ == '__main__':
    unittest.main()
