"""A failed GUI service must report its log instead of waiting indefinitely."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


START_SCRIPT = Path(__file__).resolve().parents[1] / 'tools/pulseview/start.sh'


class PulseViewStartupTests(unittest.TestCase):
    def test_failed_display_server_stops_startup_with_diagnostics(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            commands = root / 'bin'
            commands.mkdir()
            scripts = {
                'make': 'mkdir -p out/pulseview\n',
                'Xvfb': 'echo "display initialization failed" >&2\nexit 7\n',
            }
            for name, body in scripts.items():
                path = commands / name
                path.write_text('#!/bin/sh\n' + body)
                path.chmod(0o755)
            environment = dict(os.environ, PATH=str(commands) + os.pathsep + os.environ['PATH'])
            result = subprocess.run(['sh', str(START_SCRIPT)], cwd=root,
                                    env=environment, capture_output=True, text=True, timeout=3)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('Service exited before creating', result.stderr)
            self.assertIn('display initialization failed', result.stderr)


if __name__ == '__main__':
    unittest.main()
