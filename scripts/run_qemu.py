#!/usr/bin/env python3
"""Run C tests on Cortex-M4 with semihosting and a hard timeout."""
import argparse
import subprocess
import sys
parser = argparse.ArgumentParser()
parser.add_argument("--qemu", default="qemu-system-arm")
parser.add_argument("--elf", required=True)
parser.add_argument("--machine", default="netduinoplus2")
parser.add_argument("--success", default="ALL CORE TESTS PASSED")
args = parser.parse_args()
command = [args.qemu, "-M", args.machine, "-nographic", "-monitor", "none",
           "-serial", "none", "-semihosting-config", "enable=on,target=native",
           "-icount", "shift=0,align=off,sleep=off", "-kernel", args.elf]
try:
    result = subprocess.run(command, capture_output=True, text=True, timeout=30)
except (OSError, subprocess.TimeoutExpired) as error:
    sys.exit(f"QEMU failed: {error}")
print(result.stdout, end="")
print(result.stderr, end="", file=sys.stderr)
if result.returncode or args.success not in result.stdout + result.stderr:
    sys.exit("QEMU did not report a successful test run")
