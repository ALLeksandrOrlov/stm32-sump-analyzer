#!/usr/bin/env python3
"""Run C tests on Cortex-M4 with semihosting and a hard timeout."""
import argparse
import subprocess
import sys


def run_test(command, success, timeout=30):
    """Require a clean exit and a complete success line from the guest."""
    try:
        result = subprocess.run(command, capture_output=True, text=True,
                                errors="replace", timeout=timeout)
    except subprocess.TimeoutExpired as error:
        # Python retains partial output as bytes even when text=True. Keep the
        # last guest messages: they identify which scenario stopped progressing.
        for output, destination in ((error.stdout, sys.stdout), (error.stderr, sys.stderr)):
            if output:
                text = output.decode(errors="replace") if isinstance(output, bytes) else output
                print(text, end="", file=destination)
        print(f"QEMU failed: {error}", file=sys.stderr)
        return 1
    except OSError as error:
        print(f"QEMU failed: {error}", file=sys.stderr)
        return 1

    print(result.stdout, end="")
    print(result.stderr, end="", file=sys.stderr)
    lines = result.stdout.splitlines() + result.stderr.splitlines()
    if result.returncode or success not in lines:
        print("QEMU did not report a successful test run", file=sys.stderr)
        return 1
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", default="qemu-system-arm")
    parser.add_argument("--elf", required=True)
    parser.add_argument("--machine", default="netduinoplus2")
    parser.add_argument("--success", default="ALL CORE TESTS PASSED")
    args = parser.parse_args(argv)
    command = [args.qemu, "-M", args.machine, "-nographic", "-monitor", "none",
               "-serial", "none", "-semihosting-config", "enable=on,target=native",
               "-icount", "shift=0,align=off,sleep=off", "-kernel", args.elf]
    return run_test(command, args.success)


if __name__ == "__main__":
    sys.exit(main())
