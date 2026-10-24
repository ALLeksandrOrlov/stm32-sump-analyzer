"""Behavior tests for the embedded C safety gate, including real analyzer inputs."""
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import check_embedded as checker


class EmbeddedCheckTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        (self.root / "core").mkdir()
        (self.root / "platform").mkdir()

    def report(self, identifier, severity="style", filename="core/example.c"):
        location = f'<location file="{filename}" line="7"/>' if filename else ""
        return (f'<results version="2"><cppcheck version="2.10"/><errors>'
                f'<error id="{identifier}" severity="{severity}" msg="example">'
                f'{location}</error></errors></results>')

    def test_only_selected_misra_rules_fail_owned_code(self):
        for rule in checker.MISRA_RULES:
            with self.subTest(rule=rule):
                diagnostics = checker.parse_diagnostics(self.report(rule), self.root, {})
                self.assertEqual([item.rule for item in diagnostics], [rule])
        self.assertEqual(checker.parse_diagnostics(self.report("misra-c2012-15.5"),
                                                  self.root, {}), [])

    def test_vendor_diagnostics_are_excluded_by_path_not_rule(self):
        for directory in ("third_party", "Drivers", "Middlewares", "platform-other"):
            with self.subTest(directory=directory):
                diagnostics = checker.parse_diagnostics(
                    self.report("misra-c2012-21.3", filename=f"{directory}/library.c"),
                    self.root, {})
                self.assertEqual(diagnostics, [])
        self.assertEqual(len(checker.parse_diagnostics(
            self.report("nullPointer", "error", "platform/stm32/port.c"), self.root, {})), 1)

    def test_real_warnings_and_portability_issues_fail(self):
        for severity in ("warning", "error", "portability"):
            with self.subTest(severity=severity):
                self.assertEqual(len(checker.parse_diagnostics(
                    self.report("someDiagnostic", severity), self.root, {})), 1)

    def test_owned_callsite_in_vendor_diagnostic_is_not_discarded(self):
        report = self.report("nullPointer", "error", "Drivers/library.c").replace(
            '</error>', '<location file="core/caller.c" line="12"/></error>')
        result = checker.parse_diagnostics(report, self.root, {})
        self.assertEqual(len(result), 1)
        self.assertTrue(result[0].file.endswith("core/caller.c"))

    def test_source_aliases_preserve_ownership_and_report_original_path(self):
        copied = self.root / "out" / "copied.c"
        original = self.root / "core" / "example.c"
        aliases = {str(copied): str(original)}
        diagnostics = checker.parse_diagnostics(
            self.report("misra-c2012-18.8", filename=str(copied)), self.root, aliases)
        self.assertEqual(diagnostics[0].file, str(original))

    def test_tool_and_syntax_failures_are_never_ignored(self):
        for identifier in ("syntaxError", "internalError", "internalAstError",
                           "preprocessorErrorDirective", "unknownMacro"):
            with self.subTest(identifier=identifier):
                self.assertEqual(len(checker.parse_diagnostics(
                    self.report(identifier, "error", "Drivers/vendor.h"), self.root, {})), 1)
        self.assertEqual(len(checker.parse_diagnostics(
            self.report("addonFailed", "error", ""), self.root, {})), 1)
        for invalid in ("", "not XML", "<results/>", "<other/>"):
            with self.subTest(invalid=invalid), self.assertRaises(checker.CheckFailure):
                checker.parse_diagnostics(invalid, self.root, {})

    def test_compile_database_matches_owned_files_and_quoted_arguments(self):
        (self.root / "core" / "example.c").write_text("void example(void) {}\n")
        output = ('mkdir -p out\n'
                  'arm-none-eabi-gcc -I"include with spaces" -DVALUE=1 '
                  '-c core/example.c -o out/example.o\n'
                  'arm-none-eabi-gcc -c Drivers/vendor.c -o out/vendor.o\n')
        entries = checker.compilation_database(output, self.root)
        self.assertEqual(len(entries), 1)
        self.assertIn("-Iinclude with spaces", entries[0]["arguments"])
        self.assertIn("-DVALUE=1", entries[0]["arguments"])
        self.assertEqual(entries[0]["file"], str(self.root / "core" / "example.c"))
        (self.root / "platform" / "forgotten.c").write_text("void forgotten(void) {}\n")
        with self.assertRaises(checker.CheckFailure):
            checker.compilation_database(output, self.root)

    def test_empty_duplicate_and_malformed_compile_commands_fail(self):
        (self.root / "core" / "one.c").write_text("void one(void) {}\n")
        for output in ("", "cc -c", "cc -c core/one.c\ncc -c core/one.c\n", 'cc -c "'):
            with self.subTest(output=output), self.assertRaises(checker.CheckFailure):
                checker.compilation_database(output, self.root)

    def test_resolved_graph_detects_direct_and_mutual_recursion(self):
        definitions = {"direct": ("core/direct.c", "1"), "first": ("core/first.c", "2"),
                       "second": ("platform/second.c", "3"), "leaf": ("core/leaf.c", "4")}
        graph = {"direct": {"direct"}, "first": {"second"}, "second": {"first", "leaf"},
                 "leaf": set()}
        failures = checker.recursion_diagnostics(graph, definitions)
        self.assertEqual({item.file for item in failures},
                         {"core/direct.c", "core/first.c", "platform/second.c"})
        self.assertEqual(checker.recursion_diagnostics({"first": {"leaf"}}, definitions), [])

    def test_heap_gate_checks_exact_symbols_including_reentrant_and_undefined(self):
        symbols = "\n".join(f"{name} T 08000100 10" for name in checker.HEAP_SYMBOLS)
        self.assertEqual(set(checker.heap_symbols(symbols)), checker.HEAP_SYMBOLS)
        self.assertEqual(checker.heap_symbols("malloc U\n_free_r T 10 20\npvPortMalloc T 30 40\n"),
                         ["_free_r", "malloc", "pvPortMalloc"])
        self.assertEqual(checker.heap_symbols("malloc.constprop.0 T 10 20"), ["malloc.constprop.0"])
        self.assertEqual(checker.heap_symbols("USBD_static_malloc T 10 20\nfreertos_hook T 20 10"), [])

    def test_heap_gate_fails_closed_for_missing_symbols_or_invalid_nm_output(self):
        for output in ("", "filename:", "something that is not nm output"):
            with self.subTest(output=output), self.assertRaises(checker.CheckFailure):
                checker.heap_symbols(output)
        with self.assertRaises(checker.CheckFailure):
            checker.check_elf(self.root / "missing.elf", "nm", self.root)
        elf = self.root / "firmware.elf"
        elf.write_bytes(b"not an ELF")
        with mock.patch.object(checker, "run_command") as command:
            command.return_value = mock.Mock(returncode=1, stdout="", stderr="bad ELF")
            with self.assertRaises(checker.CheckFailure):
                checker.check_elf(elf, "nm", self.root)


@unittest.skipUnless(shutil.which("cppcheck"), "cppcheck is available in the Docker CI image")
class CppcheckIntegrationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        (self.root / "core").mkdir()
        self.build = self.root / "out"
        self.build.mkdir()

    def analyze(self, sources):
        entries = []
        for name, content in sources.items():
            filename = self.root / "core" / name
            filename.write_text(content)
            entries.append({"directory": str(self.root), "file": str(filename),
                            "arguments": ["cc", "-std=c11", "-c", str(filename)]})
        return checker.analyze(entries, self.root, self.build, "cppcheck")

    def test_valid_bounded_code_passes(self):
        self.assertEqual(self.analyze({"safe.c": "unsigned sample(unsigned x) { return x & 255U; }\n"}), [])

    def test_actual_variable_length_array_is_rejected(self):
        diagnostics = self.analyze({"vla.c": "void variable(unsigned n) { volatile int data[n]; data[0] = 1; }\n"})
        self.assertIn("misra-c2012-18.8", {item.rule for item in diagnostics})

    def test_actual_allocation_is_rejected(self):
        diagnostics = self.analyze({"heap.c": "#include <stdlib.h>\nvoid *allocate(void) { return malloc(16); }\n"})
        self.assertIn("misra-c2012-21.3", {item.rule for item in diagnostics})

    def test_actual_direct_recursion_is_rejected(self):
        diagnostics = self.analyze({"recursive.c": "int recurse(int n) { return n > 0 ? 1 + recurse(n - 1) : 0; }\n"})
        self.assertIn("misra-c2012-17.2", {item.rule for item in diagnostics})

    def test_actual_mutual_recursion_across_sources_is_rejected(self):
        diagnostics = self.analyze({
            "first.c": "int second(int n); int first(int n) { return n > 0 ? second(n - 1) : 0; }\n",
            "second.c": "int first(int n); int second(int n) { return first(n); }\n",
        })
        self.assertEqual({Path(item.file).name for item in diagnostics
                          if item.rule == "misra-c2012-17.2"}, {"first.c", "second.c"})

    def test_static_functions_in_different_sources_are_distinct(self):
        diagnostics = self.analyze({
            "first.c": "static void local(void) {} void first(void) { local(); }\n",
            "second.c": "void first(void); static void local(void) { first(); } void second(void) { local(); }\n",
        })
        self.assertEqual(diagnostics, [])

    def test_actual_syntax_failure_cannot_pass(self):
        try:
            diagnostics = self.analyze({"broken.c": "void broken(void) { int = ; }\n"})
        except checker.CheckFailure:
            return
        self.assertTrue(diagnostics)

    def test_failed_addon_cannot_pass_even_if_cppcheck_returns_success(self):
        with mock.patch.object(checker, "addon_launcher", return_value=Path("/bin/false")):
            with self.assertRaises(checker.CheckFailure):
                self.analyze({"heap.c": "#include <stdlib.h>\nvoid *allocate(void) { return malloc(16); }\n"})


if __name__ == "__main__":
    unittest.main()
