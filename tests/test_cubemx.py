"""Protect generated text while allowing user code and line-ending changes."""
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location(
    'cubemx_check', Path(__file__).parents[1] / 'scripts/check_cubemx.py')
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)

SOURCE = ('generated_prefix\n/* USER CODE BEGIN Init */\n'
          'initialize();\n/* USER CODE END Init */\ngenerated_suffix\n')


class CubeMxBoundaryTests(unittest.TestCase):
    def check_source(self, content):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'tests').mkdir()
            digest = hashlib.sha256(checker.skeleton(SOURCE).encode()).hexdigest()
            (root / 'tests/cubemx_generated.sha256.json').write_text(
                json.dumps({'generated.c': digest}))
            if content is not None:
                (root / 'generated.c').write_bytes(content.encode())
            with mock.patch.object(checker, 'ROOT', root), \
                 contextlib.redirect_stdout(io.StringIO()), \
                 contextlib.redirect_stderr(io.StringIO()) as errors:
                status = checker.main()
            return status, errors.getvalue()

    def test_user_section_edits_and_crlf_are_allowed(self):
        source = SOURCE.replace('initialize();', 'prepare();\nstart();')
        self.assertEqual(self.check_source(source), (0, ''))
        self.assertEqual(self.check_source(source.replace('\n', '\r\n')), (0, ''))

    def test_generated_text_change_is_rejected(self):
        status, errors = self.check_source(SOURCE.replace('generated_prefix', 'changed'))
        self.assertEqual(status, 1)
        self.assertIn('outside USER CODE', errors)

    def test_unmatched_or_nested_markers_are_rejected(self):
        for source in (
            SOURCE.replace('END Init', 'END Wrong'),
            SOURCE.replace('initialize();', '/* USER CODE BEGIN Nested */\n/* USER CODE END Nested */'),
        ):
            with self.subTest(source=source):
                status, errors = self.check_source(source)
                self.assertEqual(status, 1)
                self.assertIn('unmatched USER CODE markers', errors)

    def test_missing_generated_file_is_rejected(self):
        status, errors = self.check_source(None)
        self.assertEqual(status, 1)
        self.assertIn('missing generated file', errors)


if __name__ == '__main__':
    unittest.main()
