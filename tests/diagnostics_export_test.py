"""Exercise the launcher's diagnostics export using synthetic logs only."""
import concurrent.futures
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from launcher import diagnostics


class DiagnosticsExportTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.logs = self.root / 'data' / 'logs'
        self.game = self.root / 'game'
        self.dest = self.root / 'exports'
        self.logs.mkdir(parents=True)
        (self.game / 'logs').mkdir(parents=True)

    def test_export_before_first_save_includes_the_failed_run_after_relaunch(self):
        (self.logs / 'launcher.log').write_bytes(b'game exited with code 71')
        (self.game / 'logs' / 'nx2navi.log').write_bytes(b'new run')
        (self.game / 'logs' / 'nx2navi.crash.log').touch()
        (self.game / 'logs' / 'nx2navi.1.log').write_bytes(b'previous run reached the log cap')
        (self.game / 'logs' / 'nx2navi.1.crash.log').write_bytes(b'fatal stack')
        (self.logs / 'other.log').write_bytes(b'unrelated')
        (self.game / 'logs' / 'private.keys').write_bytes(b'excluded')
        path = diagnostics.export_diagnostics(self.game, self.logs, self.dest)
        with zipfile.ZipFile(path) as archive:
            self.assertEqual({name: archive.read(name) for name in archive.namelist()}, {
                'launcher/launcher.log': b'game exited with code 71',
                'game/nx2navi.log': b'new run',
                'game/nx2navi.1.log': b'previous run reached the log cap',
                'game/nx2navi.1.crash.log': b'fatal stack',
            })

    def test_launcher_only_and_simultaneous_exports(self):
        (self.logs / 'launcher.log').write_bytes(b'install failed')
        (self.logs / 'launcher.1.log').write_bytes(b'previous launcher log')
        (self.logs / 'crash.log').write_bytes(b'startup failure')
        with concurrent.futures.ThreadPoolExecutor(2) as pool:
            exports = [pool.submit(diagnostics.export_diagnostics, None, self.logs, self.dest) for _ in range(2)]
        paths = set(task.result() for task in exports)
        self.assertEqual(len(paths), 2)
        self.assertEqual(set(self.dest.iterdir()), paths)
        for path in paths:
            with zipfile.ZipFile(path) as archive:
                self.assertEqual(set(archive.namelist()), {'launcher/launcher.log', 'launcher/launcher.1.log',
                                                           'launcher/crash.log'})

    def test_empty_or_interrupted_export_leaves_no_archive(self):
        with self.assertRaisesRegex(OSError, 'No launcher or game logs'):
            diagnostics.export_diagnostics(self.game, self.logs, self.dest)
        (self.logs / 'launcher.log').write_bytes(b'install failed')
        with mock.patch.object(diagnostics.os, 'replace', side_effect=OSError('destination unavailable')):
            with self.assertRaisesRegex(OSError, 'destination unavailable'):
                diagnostics.export_diagnostics(self.game, self.logs, self.dest)
        self.assertEqual(list(self.dest.iterdir()), [])

    def test_oversized_log_keeps_the_failure_at_the_end(self):
        (self.logs / 'launcher.log').write_bytes(b'long build log\nerror: could not link')
        with mock.patch.object(diagnostics, 'LAUNCHER_LOG_LIMIT', 21):
            path = diagnostics.export_diagnostics(None, self.logs, self.dest)
        with zipfile.ZipFile(path) as archive:
            self.assertEqual(archive.read('launcher/launcher.log'), diagnostics.TRUNCATED + b'error: could not link')


if __name__ == '__main__':
    unittest.main()
