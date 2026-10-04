"""Regression checks for release gating and failure diagnostics."""
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import xml.etree.ElementTree as ET

spec = importlib.util.spec_from_file_location("ci_runner", Path(__file__).parents[1] / "scripts/ci.py")
ci = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ci)


class PipelineTests(unittest.TestCase):
    def test_failed_qemu_removes_old_release_and_keeps_report(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp).resolve()
            stale = root / 'out/ci/dist'
            stale.mkdir(parents=True)
            (stale / 'stm32_sump_analyzer.bin').write_bytes(b'old-success')

            def stage(name, command, reports, timeout):
                (reports / f'{name}.log').write_text('simulated stage output')
                return {'name': name, 'exit_code': 7 if name == 'qemu-stm32' else 0,
                        'seconds': 0, 'log': f'{name}.log'}

            with mock.patch.object(ci, 'ROOT', root), mock.patch.object(ci, 'execute', stage), \
                 mock.patch.object(ci, 'package') as package, mock.patch.object(sys, 'argv', ['ci.py']), \
                 mock.patch('sys.stdout', new_callable=io.StringIO):
                self.assertEqual(ci.main(), 1)
                package.assert_not_called()
            self.assertFalse(stale.exists())
            report = ET.parse(root / 'out/ci/reports/junit.xml').getroot()
            self.assertEqual(report.get('failures'), '1')
            self.assertEqual(report.find("testcase[@name='qemu-stm32']/failure").get('message'), 'exit 7')
            results = json.loads((root / 'out/ci/reports/results.json').read_text())
            self.assertEqual(results[-1]['name'], 'firmware')  # diagnostics continue

    def test_checksums_cover_binary_and_source_metadata(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp)
            firmware = output / 'work/firmware'
            firmware.mkdir(parents=True)
            for suffix in ('bin', 'hex', 'elf', 'map'):
                (firmware / f'stm32_sump_analyzer.{suffix}').write_bytes(suffix.encode())
            with mock.patch.object(ci, 'version', return_value='test version'):
                ci.package(output, 'abc123', 'refs/tags/v0.1.0', 'false')
            dist = output / 'dist'
            entries = (dist / 'SHA256SUMS').read_text().splitlines()
            self.assertEqual(len(entries), 5)
            for entry in entries:
                digest, name = entry.split('  ')
                self.assertEqual(digest, hashlib.sha256((dist / name).read_bytes()).hexdigest())
            info = json.loads((dist / 'build-info.json').read_text())
            self.assertEqual(info['source_revision'], 'abc123')
            self.assertEqual(info['source_dirty'], 'false')

    def test_process_failure_and_timeout_are_reported(self):
        with tempfile.TemporaryDirectory() as temp:
            reports = Path(temp)
            result = ci.execute('failure', [sys.executable, '-c',
                                'print("failure evidence"); raise SystemExit(7)'], reports, 10)
            self.assertEqual(result['exit_code'], 7)
            self.assertIn('failure evidence', (reports / 'failure.log').read_text())
            with mock.patch.object(ci.subprocess, 'run', side_effect=subprocess.TimeoutExpired('qemu', 1)):
                result = ci.execute('timeout', ['qemu'], reports, 1)
            self.assertNotEqual(result['exit_code'], 0)
            self.assertIn('timed out', (reports / 'timeout.log').read_text())

    def test_source_directory_cannot_be_selected_for_cleanup(self):
        with mock.patch.object(sys, 'argv', ['ci.py', '--output', 'core']), \
             mock.patch('sys.stderr', new_callable=io.StringIO):
            with self.assertRaises(SystemExit) as error:
                ci.main()
            self.assertEqual(error.exception.code, 2)


if __name__ == '__main__':
    unittest.main()
