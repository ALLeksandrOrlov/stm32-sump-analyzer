#!/usr/bin/env python3
"""The single CI entry point, run inside the project Docker image."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
FIRMWARE_NAME = "stm32_sump_analyzer"
PATCHED_QEMU = "qemu-system-arm-sump-analyzer"


def run_process(command, output, timeout):
    """Bound the entire make/compiler process tree, not just its parent."""
    with subprocess.Popen(command, cwd=ROOT, stdout=output,
                          stderr=subprocess.STDOUT, start_new_session=True) as process:
        try:
            return process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            # A timed-out compiler must not keep writing into the next stage's
            # output or leave a QEMU process running after CI reports failure.
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
            raise


def execute(name, command, reports, timeout):
    print(f"\n=== {name} ===", flush=True)
    started = time.monotonic()
    log = reports / f"{name}.log"
    code = 1
    with log.open("w", encoding="utf-8") as output:
        output.write("Command: " + " ".join(command) + "\n")
        output.flush()
        try:
            code = run_process(command, output, timeout)
        except (OSError, subprocess.TimeoutExpired) as error:
            output.write(f"\n{error}\n")
    print(log.read_text(errors="replace"), end="", flush=True)
    return {"name": name, "exit_code": code,
            "seconds": round(time.monotonic() - started, 3), "log": log.name}


def version(command):
    lines = subprocess.check_output(command, text=True, timeout=10).splitlines()
    if not lines:
        raise ValueError(f"No version reported by {command[0]}")
    return lines[0]


def package(output, revision, source_ref, dirty):
    dist = output / "dist"
    dist.mkdir()
    for suffix in ("elf", "hex", "bin", "map"):
        name = f"{FIRMWARE_NAME}.{suffix}"
        shutil.copyfile(output / "work/firmware" / name, dist / name)
    info = {
        "source_revision": revision, "source_ref": source_ref,
        "source_dirty": dirty,
        "compiler": version(["arm-none-eabi-gcc", "--version"]),
        "qemu": version([PATCHED_QEMU, "--version"]),
        "tests": "passed",
    }
    (dist / "build-info.json").write_text(json.dumps(info, indent=2) + "\n")
    hashes = []
    for path in sorted(dist.iterdir()):
        hashes.append(f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}\n")
    (dist / "SHA256SUMS").write_text("".join(hashes))


def prepare_output(requested, parser):
    """Restrict disposable build outputs to a strict subdirectory of out/."""
    output = (ROOT / requested).resolve()
    # Never remove arbitrary source paths, even if an output argument is wrong.
    if ROOT / "out" not in output.parents:
        parser.error("output must be a subdirectory of the project's out directory")
    if output.exists():
        shutil.rmtree(output)
    (output / "reports").mkdir(parents=True)
    return output


def run_checks(output):
    reports = output / "reports"
    build_dir = str((output / "work").relative_to(ROOT))
    stages = [
        ("pipeline", [sys.executable, "-m", "unittest", "discover", "-s", "tests", "-p", "test_*.py"], 30),
        ("cubemx", ["make", "check-cubemx"], 30),
        ("native", ["make", "test", "pulseview-fixture"], 120),
        ("sanitizers", ["make", "test-sanitize"], 120),
        ("coverage", ["make", "coverage"], 120),
        ("qemu-core", ["make", "qemu-test"], 120),
        ("qemu-stm32", ["make", "qemu-stm32-test", f"QEMU={PATCHED_QEMU}"], 120),
        ("qemu-rtos", ["make", "qemu-rtos-test", f"QEMU={PATCHED_QEMU}"], 120),
        ("firmware", ["make", "-j2", "firmware"], 300),
    ]
    results = []
    for name, command, timeout in stages:
        if command[0] == "make":
            command = command + [f"BUILD_DIR={build_dir}"]
        results.append(execute(name, command, reports, timeout))
    # Keep coverage evidence alongside logs even when another stage fails.
    coverage = output / "work/coverage"
    if coverage.is_dir():
        destination = reports / "coverage"
        destination.mkdir()
        for pattern in ("*.html", "*.css", "*.xml", "*.json"):
            for path in coverage.glob(pattern):
                shutil.copyfile(path, destination / path.name)
    return results


def write_reports(reports, results):
    """Emit readable output and valid JUnit even for logs with terminal escapes."""
    failed = sum(r["exit_code"] != 0 for r in results)
    suite = ET.Element("testsuite", name="firmware-ci", tests=str(len(results)), failures=str(failed))
    for result in results:
        case = ET.SubElement(suite, "testcase", name=result["name"],
                             time=str(result["seconds"]))
        if result["exit_code"]:
            log = (reports / result["log"]).read_text(errors="replace")[-16000:]
            ET.SubElement(case, "failure", message=f"exit {result['exit_code']}").text = re.sub(
                r"[\x00-\x08\x0b\x0c\x0e-\x1f]", "", log)
        ET.SubElement(case, "system-out").text = f"Full log: {result['log']}"
    ET.ElementTree(suite).write(reports / "junit.xml", encoding="utf-8", xml_declaration=True)
    (reports / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    lines = ["# Firmware CI", "", "| Stage | Result | Seconds |", "| --- | --- | --- |"]
    lines += [f"| {r['name']} | {'FAIL' if r['exit_code'] else 'PASS'} | {r['seconds']} |" for r in results]
    (reports / "summary.md").write_text("\n".join(lines) + "\n")
    return failed


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", default="out/ci")
    args = parser.parse_args()
    output = prepare_output(args.output, parser)
    reports = output / "reports"
    results = run_checks(output)
    if not any(result["exit_code"] for result in results):
        try:
            package(output, os.getenv("SOURCE_REVISION", "unknown"),
                    os.getenv("SOURCE_REF", "local"), os.getenv("SOURCE_DIRTY", "unknown"))
        except (OSError, subprocess.SubprocessError, ValueError) as error:
            (reports / "packaging.log").write_text(str(error) + "\n")
            results.append({"name": "packaging", "exit_code": 1, "seconds": 0,
                            "log": "packaging.log"})
            # A partially assembled release is never eligible for publication.
            shutil.rmtree(output / "dist", ignore_errors=True)
    failed = write_reports(reports, results)
    print(f"\nCI: {len(results) - failed}/{len(results)} stages passed. Reports: {reports}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
