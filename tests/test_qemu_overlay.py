"""Behavioral checks for installing the QEMU machine into supported sources."""

import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).parents[1] / 'qemu/apply_overlay.py'
spec = importlib.util.spec_from_file_location('qemu_overlay', SCRIPT)
overlay = importlib.util.module_from_spec(spec)
spec.loader.exec_module(overlay)

MESON = "arm_ss = ss.source_set()\nhw_arch += {'arm': arm_ss}\n"
SYSCFG = '''static void set_irq(void *opaque, int irq, int level)
{
    int icrreg = irq / 4;
    int startbit = (irq & 3) * 4;
    qemu_set_irq(s->gpio_out[irq], level);
}
'''


class OverlayTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / 'hw/arm').mkdir(parents=True)
        (self.root / 'hw/misc').mkdir(parents=True)
        (self.root / 'VERSION').write_text('9.2.4\n', encoding='utf-8')
        self.meson = self.root / 'hw/arm/meson.build'
        self.syscfg = self.root / 'hw/misc/stm32f4xx_syscfg.c'
        self.machine = self.root / 'hw/arm/stm32f407-sump-analyzer.c'
        self.meson.write_text(MESON, encoding='utf-8')
        self.syscfg.write_text(SYSCFG, encoding='utf-8')

    def snapshot(self):
        return {path.relative_to(self.root): path.read_bytes()
                for path in self.root.rglob('*') if path.is_file()}

    def assert_rejected_without_changes(self, message):
        before = self.snapshot()
        with self.assertRaisesRegex(overlay.OverlayError, message):
            overlay.apply_overlay(self.root)
        self.assertEqual(self.snapshot(), before)

    def test_installs_machine_and_routes_exti_by_pin(self):
        changed = overlay.apply_overlay(self.root)
        self.assertEqual(set(changed), {self.meson, self.syscfg, self.machine})
        self.assertEqual(self.machine.read_bytes(), overlay.MACHINE_SOURCE.read_bytes())
        self.assertIn("files('stm32f407-sump-analyzer.c')", self.meson.read_text())
        patched = self.syscfg.read_text()
        self.assertIn('int pin = irq % 16;', patched)
        self.assertIn('int icrreg = pin / 4;', patched)
        self.assertIn('int startbit = (pin & 3) * 4;', patched)
        self.assertIn('qemu_set_irq(s->gpio_out[pin], level);', patched)

    def test_repeat_installation_preserves_contents_and_modification_times(self):
        overlay.apply_overlay(self.root)
        before = self.snapshot()
        times = {path: path.stat().st_mtime_ns for path in
                 (self.meson, self.syscfg, self.machine)}
        self.assertEqual(overlay.apply_overlay(self.root), [])
        self.assertEqual(self.snapshot(), before)
        self.assertEqual({path: path.stat().st_mtime_ns for path in times}, times)

    def test_new_machine_source_updates_only_owned_machine_file(self):
        overlay.apply_overlay(self.root)
        replacement = self.root / 'updated-machine.c'
        replacement.write_bytes(b'/* updated machine implementation */\n')
        self.assertEqual(overlay.apply_overlay(self.root, replacement), [self.machine])
        self.assertEqual(self.machine.read_bytes(), replacement.read_bytes())

    def test_unsupported_version_is_rejected_before_changes(self):
        (self.root / 'VERSION').write_text('9.2.5\n', encoding='utf-8')
        self.assert_rejected_without_changes('expected QEMU 9.2.4')

    def test_missing_or_duplicate_meson_anchor_is_rejected(self):
        for text in ('arm_ss = ss.source_set()\n', MESON + MESON):
            with self.subTest(text=text):
                self.meson.write_text(text, encoding='utf-8')
                self.assert_rejected_without_changes('ARM source-set anchor')

    def test_missing_or_duplicate_syscfg_anchor_is_rejected(self):
        for original, _ in overlay.SYSCFG_REPLACEMENTS:
            for replacement in ('', original + '\n' + original):
                with self.subTest(anchor=original, replacement=replacement):
                    self.syscfg.write_text(SYSCFG.replace(original, replacement), encoding='utf-8')
                    self.assert_rejected_without_changes('routing anchor')

    def test_incomplete_syscfg_patch_is_rejected(self):
        self.syscfg.write_text(SYSCFG.replace('s->gpio_out[irq]', 's->gpio_out[pin]'),
                               encoding='utf-8')
        self.assert_rejected_without_changes('incomplete routing patch')

    def test_installed_entry_does_not_hide_broken_build_anchor(self):
        overlay.apply_overlay(self.root)
        self.meson.write_text(self.meson.read_text().replace(overlay.MESON_ANCHOR, ''),
                              encoding='utf-8')
        self.assert_rejected_without_changes('ARM source-set anchor')

    def test_duplicate_or_unrecognized_machine_entry_is_rejected(self):
        for entry in (overlay.MESON_ENTRY * 2,
                      overlay.MESON_ENTRY + "arm_ss.add(files('stm32f407-sump-analyzer.c'))\n",
                      "arm_ss.add(files('stm32f407-sump-analyzer.c'))\n"):
            with self.subTest(entry=entry):
                self.meson.write_text(entry + MESON, encoding='utf-8')
                self.assert_rejected_without_changes('analyzer machine entr')

    def test_missing_machine_source_does_not_modify_qemu_sources(self):
        before = self.snapshot()
        with self.assertRaises(FileNotFoundError):
            overlay.apply_overlay(self.root, self.root / 'missing-machine.c')
        self.assertEqual(self.snapshot(), before)

    def test_cli_reports_invalid_source_without_traceback(self):
        self.syscfg.unlink()
        before = self.snapshot()
        result = subprocess.run([sys.executable, str(SCRIPT), str(self.root)],
                                capture_output=True, text=True, check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('stm32f4xx_syscfg.c', result.stderr)
        self.assertNotIn('Traceback', result.stderr)
        self.assertEqual(self.snapshot(), before)


if __name__ == '__main__':
    unittest.main()
