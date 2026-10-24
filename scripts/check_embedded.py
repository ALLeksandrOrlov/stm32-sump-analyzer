#!/usr/bin/env python3
"""Enforce selected embedded C rules and reject heap dependencies in firmware.

This is not a MISRA compliance assessment. Cppcheck checks rules 17.2, 18.8,
21.3 and its warning/error diagnostics. A graph of resolved direct calls also
checks recursion across translation units; indirect callbacks require review.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
OWNED_DIRECTORIES = ("core", "platform")
MISRA_RULES = {"misra-c2012-17.2", "misra-c2012-18.8", "misra-c2012-21.3"}
TOOL_FAILURES = {"syntaxError", "internalError", "internalAstError", "preprocessorErrorDirective",
                 "unknownMacro"}
HEAP_SYMBOLS = {
    "malloc", "calloc", "realloc", "free", "memalign", "aligned_alloc", "valloc", "pvalloc",
    "_malloc_r", "_calloc_r", "_realloc_r", "_free_r", "_memalign_r", "_valloc_r", "_pvalloc_r",
    "sbrk", "_sbrk", "_sbrk_r", "pvPortMalloc", "pvPortCalloc", "vPortFree",
}


class CheckFailure(RuntimeError):
    """The analysis could not be completed reliably."""


@dataclass(frozen=True)
class Diagnostic:
    rule: str
    message: str
    file: str = ""
    line: str = "0"

    def __str__(self) -> str:
        location = f"{self.file}:{self.line}: " if self.file else ""
        return f"{location}{self.rule}: {self.message}"


def run_command(arguments: list[str], root: Path) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(arguments, cwd=root, text=True, capture_output=True,
                              check=False, timeout=150)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise CheckFailure(f"could not run {arguments[0]}: {error}") from error


def source_path(name: str, root: Path, aliases: dict[str, str]) -> Path:
    path = Path(name)
    if not path.is_absolute():
        path = root / path
    normalized = str(path.resolve())
    return Path(aliases.get(normalized, normalized))


def is_owned(name: str, root: Path, aliases: dict[str, str]) -> bool:
    if not name:
        return False
    try:
        relative = source_path(name, root, aliases).relative_to(root.resolve())
    except ValueError:
        return False
    return bool(relative.parts) and relative.parts[0] in OWNED_DIRECTORIES


def compilation_database(output: str, root: Path) -> list[dict]:
    """Extract the actual owned-source compiler invocations from a make dry run."""
    entries = []
    for line in output.splitlines():
        try:
            arguments = shlex.split(line)
        except ValueError as error:
            raise CheckFailure(f"invalid make command: {line}") from error
        if "-c" not in arguments:
            continue
        index = arguments.index("-c") + 1
        if index >= len(arguments):
            raise CheckFailure("compiler command has no source after -c")
        filename = arguments[index]
        if not is_owned(filename, root, {}) or Path(filename).suffix != ".c":
            continue
        entries.append({"directory": str(root), "file": str((root / filename).resolve()),
                        "arguments": arguments})
    expected = {str(path.resolve()) for directory in OWNED_DIRECTORIES
                for path in (root / directory).rglob("*.c")}
    observed = {entry["file"] for entry in entries}
    if not entries or observed != expected or len(entries) != len(observed):
        missing = sorted(expected - observed)
        raise CheckFailure(f"compile database does not cover owned sources exactly; missing: {missing}")
    return entries


def parse_diagnostics(xml_text: str, root: Path, aliases: dict[str, str]) -> list[Diagnostic]:
    try:
        report = ET.fromstring(xml_text)
    except ET.ParseError as error:
        raise CheckFailure(f"cppcheck did not produce valid XML: {error}") from error
    if report.tag != "results" or report.find("cppcheck") is None or report.find("errors") is None:
        raise CheckFailure("cppcheck report is missing its result structure")

    failures = []
    for item in report.findall("errors/error"):
        rule = item.get("id", "")
        severity = item.get("severity", "")
        locations = item.findall("location")
        location = next((loc for loc in locations if is_owned(loc.get("file", ""), root, aliases)),
                        locations[0] if locations else None)
        owned = any(is_owned(loc.get("file", ""), root, aliases) for loc in locations)
        # Tool and syntax failures invalidate the entire run, even in a header.
        fatal = rule in TOOL_FAILURES or rule.startswith("internal") or not locations
        selected = rule in MISRA_RULES or severity in {"error", "warning", "portability"}
        if fatal or (owned and selected):
            filename = str(source_path(location.get("file", ""), root, aliases)) if location is not None else ""
            failures.append(Diagnostic(rule or "unknown", item.get("msg", ""), filename,
                                       location.get("line", "0") if location is not None else "0"))
    return failures


def resolved_calls(dump_file: Path, root: Path, aliases: dict[str, str]) -> tuple[dict, dict]:
    """Read cppcheck's AST, never parse C text; retain resolved direct calls only."""
    try:
        tree = ET.parse(dump_file).getroot()
    except (ET.ParseError, OSError) as error:
        raise CheckFailure(f"invalid or missing analysis dump {dump_file}: {error}") from error
    configurations = tree.findall("dump")
    if tree.tag != "dumps" or not configurations:
        raise CheckFailure(f"analysis dump has no configurations: {dump_file}")
    graph: dict[str, set[str]] = {}
    definitions = {}
    for configuration in configurations:
        tokens = {token.get("id"): token for token in configuration.findall("tokenlist/token")}
        scopes = {scope.get("id"): scope for scope in configuration.findall("scopes/scope")}
        functions = {}
        for function in configuration.findall("scopes/scope/functionList/function"):
            name = function.get("name", "")
            key = f"{dump_file}:{name}" if function.get("isStatic") == "true" else name
            functions[function.get("id")] = key
        for scope in scopes.values():
            if scope.get("type") != "Function":
                continue
            function = functions.get(scope.get("function"))
            start = tokens.get(scope.get("bodyStart"))
            if function and start is not None:
                graph.setdefault(function, set())
                if is_owned(start.get("file", ""), root, aliases):
                    definitions[function] = (str(source_path(start.get("file", ""), root, aliases)),
                                             start.get("linenr", "0"))
        for token in tokens.values():
            if token.get("str") != "(":
                continue
            operand = tokens.get(token.get("astOperand1"))
            called = functions.get(operand.get("function")) if operand is not None else None
            if called is None:
                continue
            scope = scopes.get(token.get("scope"))
            visited = set()
            while scope is not None and scope.get("type") != "Function":
                identifier = scope.get("id")
                if identifier in visited:
                    raise CheckFailure(f"cyclic scope data in {dump_file}")
                visited.add(identifier)
                scope = scopes.get(scope.get("nestedIn"))
            caller = functions.get(scope.get("function")) if scope is not None else None
            if caller:
                graph.setdefault(caller, set()).add(called)
    return graph, definitions


def recursion_diagnostics(graph: dict[str, set[str]], definitions: dict) -> list[Diagnostic]:
    failures = []
    # Iterative traversal avoids a host recursion limit on a large dependency graph.
    for function, (filename, line) in sorted(definitions.items()):
        pending = list(graph.get(function, set()))
        visited = set()
        while pending:
            called = pending.pop()
            if called == function:
                failures.append(Diagnostic("misra-c2012-17.2", "resolved call graph contains recursion",
                                           filename, line))
                break
            if called not in visited:
                visited.add(called)
                pending.extend(graph.get(called, set()))
    return failures


def compiler_predefines(arguments: list[str], root: Path) -> list[str]:
    """Use the compiler's target macros, including CMSIS compiler/FPU selection."""
    target_flags = [argument for argument in arguments[1:]
                    if argument.startswith(("-m", "-std="))]
    result = run_command([arguments[0], *target_flags, "-dM", "-E", "-x", "c", "/dev/null"], root)
    if result.returncode != 0:
        raise CheckFailure(f"compiler predefined-macro query failed: {result.stderr}")
    definitions = []
    for line in result.stdout.splitlines():
        match = re.fullmatch(r"#define ([A-Za-z_][A-Za-z_0-9]*)(?: (.*))?", line)
        if match:
            definitions.append(f"-D{match[1]}={match[2] or ''}")
    if not definitions:
        raise CheckFailure("compiler returned no predefined object macros")
    return definitions


def addon_launcher(build_dir: Path) -> Path:
    """Keep distinct rule IDs when cppcheck merges identical same-line messages.

    The installed add-on stays unchanged. Its generic messages otherwise cause
    an unselected MISRA rule to hide a selected rule on the same source line.
    """
    launcher = build_dir / "addon-python"
    launcher.write_text('''#!/usr/bin/env python3
import json
from pathlib import Path
import subprocess
import sys

result = subprocess.run([sys.executable, *sys.argv[1:]], capture_output=True, text=True)
with Path(__file__).with_suffix(".receipts").open("a") as receipts:
    receipts.write(json.dumps({"arguments": sys.argv[1:], "status": result.returncode}) + "\\n")
sys.stderr.write(result.stderr)
for line in result.stdout.splitlines():
    try:
        diagnostic = json.loads(line)
    except json.JSONDecodeError:
        print(line)
        continue
    if isinstance(diagnostic, dict) and "errorId" in diagnostic and "message" in diagnostic:
        diagnostic["message"] = diagnostic["errorId"] + ": " + diagnostic["message"]
    print(json.dumps(diagnostic))
sys.exit(result.returncode)
''')
    launcher.chmod(0o755)
    return launcher


def verify_addon_runs(launcher: Path, expected_dumps: set[str], stdout: str) -> None:
    """Cppcheck 2.10 can report success even when an add-on fails to launch."""
    if any(marker in stdout.lower() for marker in ("bailing out", "failed to execute", "internal error")):
        raise CheckFailure(f"cppcheck add-on execution failed: {stdout.strip()}")
    receipts = launcher.with_suffix(".receipts")
    try:
        invocations = [json.loads(line) for line in receipts.read_text().splitlines()]
    except (OSError, json.JSONDecodeError) as error:
        raise CheckFailure("MISRA add-on did not confirm successful execution") from error
    checked = set()
    for invocation in invocations:
        if invocation.get("status") != 0:
            raise CheckFailure(f"MISRA add-on invocation failed: {invocation}")
        for argument in invocation.get("arguments", []):
            if argument.endswith(".dump"):
                checked.add(str(Path(argument).resolve()))
    if checked != expected_dumps:
        raise CheckFailure(f"MISRA add-on coverage mismatch; missing dumps: {sorted(expected_dumps - checked)}")


def analyze(entries: list[dict], root: Path, build_dir: Path, cppcheck: str) -> list[Diagnostic]:
    """Use exact source copies so cppcheck dumps never appear beside source files."""
    aliases = {}
    projected = []
    predefined = {}
    for entry in entries:
        original = Path(entry["file"])
        copied = build_dir / "sources" / original.relative_to(root)
        copied.parent.mkdir(parents=True, exist_ok=True)
        copied.write_bytes(original.read_bytes())
        arguments = list(entry["arguments"])
        target = tuple(argument for argument in arguments
                       if argument == arguments[0] or argument.startswith(("-m", "-std=")))
        if target not in predefined:
            predefined[target] = compiler_predefines(arguments, root)
        arguments[1:1] = predefined[target]
        arguments[arguments.index("-c") + 1] = str(copied)
        # Preserve local quoted-header lookup after moving the source file.
        arguments.extend(["-I", str(original.parent)])
        projected.append({"directory": str(root), "file": str(copied), "arguments": arguments})
        aliases[str(copied.resolve())] = str(original)
    database = build_dir / "compile_commands.json"
    database.write_text(json.dumps(projected, indent=2) + "\n")
    launcher = addon_launcher(build_dir)
    result = run_command([cppcheck, f"--project={database}", "--addon=misra",
                          f"--addon-python={launcher}", "--dump",
                          "-j", "4", "--quiet", "--xml", "--xml-version=2", "--enable=warning,portability",
                          "--std=c11", "--platform=unix32"], root)
    # Without --error-exitcode, nonzero status denotes an analyzer failure.
    if result.returncode != 0:
        raise CheckFailure(f"cppcheck exited {result.returncode}: {result.stderr or result.stdout}")
    verify_addon_runs(launcher, {str(Path(entry["file"] + ".dump").resolve()) for entry in projected},
                      result.stdout)
    (build_dir / "cppcheck.xml").write_text(result.stderr)
    failures = parse_diagnostics(result.stderr, root, aliases)
    graph = {}
    definitions = {}
    for entry in projected:
        calls, locations = resolved_calls(Path(entry["file"] + ".dump"), root, aliases)
        for caller, callees in calls.items():
            graph.setdefault(caller, set()).update(callees)
        definitions.update(locations)
    failures.extend(recursion_diagnostics(graph, definitions))
    return failures


def heap_symbols(nm_output: str) -> list[str]:
    symbols = []
    for line in nm_output.splitlines():
        fields = line.split()
        if len(fields) < 2 or re.fullmatch(r"[A-Za-z?]", fields[1]) is None:
            raise CheckFailure(f"invalid nm output: {line!r}")
        symbols.append(fields[0])
    if not symbols:
        raise CheckFailure("ELF contains no readable symbols; check the unstripped firmware")
    return sorted({symbol for symbol in symbols if re.split(r"[.@]", symbol)[0] in HEAP_SYMBOLS})


def check_elf(elf: Path, nm: str, root: Path) -> list[Diagnostic]:
    if not elf.is_file():
        raise CheckFailure(f"firmware ELF does not exist: {elf}")
    result = run_command([nm, "--format=posix", "--no-demangle", str(elf)], root)
    if result.returncode != 0 or result.stderr.strip():
        raise CheckFailure(f"nm failed: {result.stderr or result.stdout}")
    return [Diagnostic("no-heap", f"linked symbol {symbol}", str(elf))
            for symbol in heap_symbols(result.stdout)]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    static = commands.add_parser("static", help="check owned firmware C sources")
    static.add_argument("--cppcheck", default="cppcheck")
    static.add_argument("--build-dir", type=Path, default=Path("out/embedded-checks"))
    elf = commands.add_parser("elf", help="reject heap dependencies in a linked ELF")
    elf.add_argument("--nm", default="arm-none-eabi-nm")
    elf.add_argument("file", type=Path)
    arguments = parser.parse_args()
    try:
        if arguments.command == "static":
            build_dir = (ROOT / arguments.build_dir).resolve()
            build_dir.mkdir(parents=True, exist_ok=True)
            result = run_command(["make", "--no-print-directory", "-B", "-n", "firmware",
                                  f"BUILD_DIR={build_dir / 'dry-run'}"], ROOT)
            if result.returncode != 0:
                raise CheckFailure(f"make dry run failed: {result.stderr}")
            entries = compilation_database(result.stdout, ROOT)
            # A fresh directory guarantees dumps belong to this successful invocation.
            with tempfile.TemporaryDirectory(prefix="analysis-", dir=build_dir) as temporary:
                failures = analyze(entries, ROOT, Path(temporary), arguments.cppcheck)
            description = f"selected MISRA rules and analyzer diagnostics, {len(entries)} sources"
        else:
            failures = check_elf(arguments.file.resolve(), arguments.nm, ROOT)
            description = "linked firmware has no heap allocator symbols"
        for failure in failures:
            print(failure, file=sys.stderr)
        if failures:
            return 1
        print(f"Embedded C checks passed: {description}")
        return 0
    except (CheckFailure, OSError) as error:
        print(f"Embedded C check could not complete: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
