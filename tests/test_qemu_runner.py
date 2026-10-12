"""The guest success marker alone must never hide a failed or truncated run."""
import contextlib
import importlib.util
import io
from pathlib import Path
import sys
import subprocess
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location(
    'qemu_runner', Path(__file__).parents[1] / 'scripts/run_qemu.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class QemuRunnerTests(unittest.TestCase):
    def run_guest(self, code, timeout=5):
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return runner.run_test([sys.executable, '-c', code], 'TESTS PASSED', timeout)

    def test_success_on_stdout_or_semihosting_stderr(self):
        self.assertEqual(self.run_guest('print("TESTS PASSED")'), 0)
        self.assertEqual(self.run_guest('import sys; print("TESTS PASSED", file=sys.stderr)'), 0)

    def test_nonzero_exit_overrides_success_marker(self):
        self.assertEqual(self.run_guest('print("TESTS PASSED"); raise SystemExit(9)'), 1)

    def test_no_marker_or_partial_marker_cannot_pass(self):
        for output in ('booted', 'NOT TESTS PASSED', 'TESTS PASSED BUT FAILED'):
            with self.subTest(output=output):
                self.assertEqual(self.run_guest(f'print({output!r})'), 1)

    def test_hung_guest_fails_even_after_printing_success(self):
        self.assertEqual(self.run_guest(
            'import time; print("TESTS PASSED", flush=True); time.sleep(30)', 0.1), 1)

    def test_missing_emulator_fails(self):
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(runner.run_test(['/nonexistent/qemu'], 'TESTS PASSED'), 1)

    def test_timeout_preserves_partial_guest_diagnostics(self):
        error = subprocess.TimeoutExpired('qemu', 30, output=b'capture started\n',
                                          stderr=b'last IRQ\xff\n')
        with mock.patch.object(runner.subprocess, 'run', side_effect=error), \
             contextlib.redirect_stdout(io.StringIO()) as output, \
             contextlib.redirect_stderr(io.StringIO()) as errors:
            self.assertEqual(runner.run_test(['qemu'], 'TESTS PASSED'), 1)
        self.assertIn('capture started', output.getvalue())
        self.assertIn('last IRQ', errors.getvalue())
        self.assertIn('timed out', errors.getvalue())


if __name__ == '__main__':
    unittest.main()
