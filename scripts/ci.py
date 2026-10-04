#!/usr/bin/env python3
"""The single CI entry point, run inside the project Docker image."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]


def execute(name, command, reports, timeout):
    print(f"\n=== {name} ===", flush=True)
    started = time.monotonic()
    log = reports / f"{name}.log"
    code = 1
    with log.open("w") as output:
        output.write("Command: " + " ".join(command) + "\n")
        output.flush()
        try:
            result = subprocess.run(command, cwd=ROOT, stdout=output,
                                    stderr=subprocess.STDOUT, timeout=timeout)
            code = result.returncode
        except (OSError, subprocess.TimeoutExpired) as error:
            output.write(f"\n{error}\n")
    print(log.read_text(), end="", flush=True)
    return {"name": name, "exit_code": code,
            "seconds": round(time.monotonic() - started, 3), "log": log.name}


def version(command):
    return subprocess.check_output(command, text=True).splitlines()[0]


def package(output, revision, source_ref, dirty):
    dist = output / "dist"
    dist.mkdir()
    for suffix in ("elf", "hex", "bin", "map"):
        name = f"stm32_sump_analyzer.{suffix}"
        shutil.copyfile(output / "work/firmware" / name, dist / name)
    info = {
        "source_revision": revision, "source_ref": source_ref,
        "source_dirty": dirty,
        "compiler": version(["arm-none-eabi-gcc", "--version"]),
        "qemu": version(["qemu-system-arm-sump-analyzer", "--version"]),
        "tests": "passed",
    }
    (dist / "build-info.json").write_text(json.dumps(info, indent=2) + "\n")
    hashes = []
    for path in sorted(dist.iterdir()):
        hashes.append(f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}\n")
    (dist / "SHA256SUMS").write_text("".join(hashes))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", default="out/ci")
    args = parser.parse_args()
    output = (ROOT / args.output).resolve()
    # Never remove arbitrary source paths, even if an output argument is wrong.
    if ROOT / "out" not in output.parents:
        parser.error("output must be a subdirectory of the project's out directory")
    if output.exists():
        shutil.rmtree(output)
    reports = output / "reports"
    reports.mkdir(parents=True)
    build_dir = str((output / "work").relative_to(ROOT))
    stages = [
        ("pipeline", ["python3", "-m", "unittest", "discover", "-s", "tests", "-p", "test_ci_runner.py"], 30),
        ("cubemx", ["make", "check-cubemx"], 30),
        ("native", ["make", "test"], 120),
        ("sanitizers", ["make", "test-sanitize"], 120),
        ("qemu-core", ["make", "qemu-test"], 120),
        ("qemu-stm32", ["make", "qemu-stm32-test", "QEMU=qemu-system-arm-sump-analyzer"], 120),
        ("firmware", ["make", "-j2", "firmware"], 300),
    ]
    results = []
    for name, command, timeout in stages:
        if command[0] == "make":
            command = command + [f"BUILD_DIR={build_dir}"]
        results.append(execute(name, command, reports, timeout))
    if not any(r["exit_code"] for r in results):
        try:
            package(output, os.getenv("SOURCE_REVISION", "unknown"),
                    os.getenv("SOURCE_REF", "local"), os.getenv("SOURCE_DIRTY", "unknown"))
        except (OSError, subprocess.SubprocessError) as error:
            (reports / "packaging.log").write_text(str(error) + "\n")
            results.append({"name": "packaging", "exit_code": 1, "seconds": 0,
                            "log": "packaging.log"})
            # A partially assembled release is never eligible for publication.
            shutil.rmtree(output / "dist", ignore_errors=True)
    failed = sum(r["exit_code"] != 0 for r in results)
    suite = ET.Element("testsuite", name="firmware-ci", tests=str(len(results)), failures=str(failed))
    for result in results:
        case = ET.SubElement(suite, "testcase", name=result["name"],
                             time=str(result["seconds"]))
        if result["exit_code"]:
            ET.SubElement(case, "failure", message=f"exit {result['exit_code']}").text = (
                reports / result["log"]).read_text()[-16000:]
        ET.SubElement(case, "system-out").text = f"Full log: {result['log']}"
    ET.ElementTree(suite).write(reports / "junit.xml", encoding="utf-8", xml_declaration=True)
    (reports / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    lines = ["# Firmware CI", "", "| Stage | Result | Seconds |", "| --- | --- | --- |"]
    lines += [f"| {r['name']} | {'FAIL' if r['exit_code'] else 'PASS'} | {r['seconds']} |" for r in results]
    (reports / "summary.md").write_text("\n".join(lines) + "\n")
    print(f"\nCI: {len(results) - failed}/{len(results)} stages passed. Reports: {reports}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
