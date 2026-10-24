"""Check that the pinned kernel subset has not acquired local modifications."""
import hashlib
from pathlib import Path
import unittest


class FreeRtosSourcesTests(unittest.TestCase):
    def test_vendored_files_match_the_recorded_source_manifest(self):
        kernel = Path(__file__).resolve().parents[1] / 'third_party/FreeRTOS-Kernel'
        expected = set()
        for line in (kernel / 'SHA256SUMS').read_text().splitlines():
            digest, name = line.split('  ', 1)
            expected.add(name)
            with self.subTest(file=name):
                self.assertEqual(hashlib.sha256((kernel / name).read_bytes()).hexdigest(), digest)
        actual = {str(path.relative_to(kernel)) for path in kernel.rglob('*') if path.is_file()}
        self.assertEqual(actual - {'README.md', 'SHA256SUMS'}, expected)
        self.assertIn('LICENSE.md', expected)
        self.assertIn('portable/GCC/ARM_CM4F/port.c', expected)


if __name__ == '__main__':
    unittest.main()
