#!/usr/bin/env python3
"""Exercise a selected SUMP serial port and retain the complete wire exchange.

This POSIX-only check needs no signal generator or third-party Python packages.
It checks byte counts, framing and recovery, not sample values or timing accuracy.
"""
import argparse
from datetime import datetime, timezone
import errno
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import select
import struct
import sys
import termios
import time
import tty


RESET, RUN, ID, META = 0x00, 0x01, 0x02, 0x04
DIVIDER, COUNTS, FLAGS, TRIGGER_MASK = 0x80, 0x81, 0x82, 0xC0
IDENTITY = b"1ALS"
CHANNEL_FLAGS = 0x38  # Keep the eight input channels; disable absent groups.
DISABLE_INPUT_GROUP = 0x04
SUMP_CLOCK_HZ = 100_000_000
CAPTURE_SIZES = (4, 68, 1024, 28672)
QUIET_SECONDS = 0.05


class CheckFailure(Exception):
    """A bounded operation failed or a response violated the protocol."""


def command(opcode, argument):
    return bytes((opcode,)) + struct.pack("<I", argument)


def capture_configuration(samples, divider, flags=CHANNEL_FLAGS):
    if samples < 4 or samples % 4 or samples > 262144:
        raise ValueError("SUMP sample count must be a multiple of four in 4..262144")
    encoded = samples // 4 - 1
    return (command(DIVIDER, divider) +
            command(COUNTS, encoded | (encoded << 16)) +
            command(FLAGS, flags) + command(TRIGGER_MASK, 0))


class SerialPort:
    """Nonblocking serial I/O with deadlines and byte-for-byte trace files."""

    def __init__(self, path, timeout, output):
        self.path = path
        self.timeout = timeout
        self.output = output
        self.fd = None
        self.original_settings = None
        self.tx = None
        self.rx = None

    def __enter__(self):
        try:
            self.fd = os.open(self.path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
            # Cooperating serial clients cannot accidentally share this run.
            fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.original_settings = termios.tcgetattr(self.fd)
            tty.setraw(self.fd, termios.TCSANOW)
            settings = termios.tcgetattr(self.fd)
            settings[4] = settings[5] = termios.B115200
            settings[2] |= termios.CLOCAL | termios.CREAD
            settings[2] &= ~getattr(termios, "CRTSCTS", 0)
            termios.tcsetattr(self.fd, termios.TCSANOW, settings)
            self.tx = (self.output / "tx.bin").open("wb")
            self.rx = (self.output / "rx.bin").open("wb")
            return self
        except BaseException:
            self.close()
            raise

    def close(self):
        if self.fd is not None:
            try:
                if self.original_settings is not None:
                    termios.tcsetattr(self.fd, termios.TCSANOW, self.original_settings)
            except (OSError, termios.error):
                # Disconnection must not hide the actual test failure.
                pass
            finally:
                os.close(self.fd)
                self.fd = None
        for stream in (self.tx, self.rx):
            if stream is not None:
                stream.close()

    def __exit__(self, *_):
        self.close()

    def write(self, data):
        deadline = time.monotonic() + self.timeout
        pending = memoryview(data)
        while pending:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select([], [self.fd], [], remaining)[1]:
                raise CheckFailure(f"write timed out with {len(pending)} bytes pending")
            try:
                count = os.write(self.fd, pending)
            except BlockingIOError:
                continue
            if count == 0:
                raise CheckFailure("serial write made no progress")
            self.tx.write(pending[:count])
            self.tx.flush()
            pending = pending[count:]

    def read_some(self, count, timeout):
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select([self.fd], [], [], remaining)[0]:
                return b""
            try:
                data = os.read(self.fd, count)
            except BlockingIOError:
                continue
            except OSError as error:
                if error.errno == errno.EIO:
                    raise CheckFailure("serial port disconnected") from error
                raise
            if not data:
                raise CheckFailure("serial port closed")
            self.rx.write(data)
            self.rx.flush()
            return data

    def read_exact(self, count, *, deadline=None):
        if deadline is None:
            deadline = time.monotonic() + self.timeout
        result = bytearray()
        while len(result) < count:
            data = self.read_some(count - len(result), deadline - time.monotonic())
            if not data:
                raise CheckFailure(f"response timed out: expected {count} bytes, received {len(result)}")
            result.extend(data)
        return bytes(result)

    def expect_quiet(self, duration=QUIET_SECONDS):
        extra = self.read_some(4096, duration)
        if extra:
            raise CheckFailure(f"unexpected trailing response: {len(extra)} bytes, {extra[:32].hex()}")

    def reset_and_drain(self):
        # Five zeroes resynchronize even after an interrupted long command.
        self.write(bytes((RESET,)) * 5)
        deadline = time.monotonic() + self.timeout
        discarded = 0
        while True:
            remaining = deadline - time.monotonic()
            if remaining < QUIET_SECONDS:
                raise CheckFailure("serial input did not become quiet after RESET")
            data = self.read_some(4096, QUIET_SECONDS)
            if not data:
                return discarded
            discarded += len(data)


def read_metadata(port):
    """Decode typed SUMP fields without treating integer/string zeroes as END."""
    deadline = time.monotonic() + port.timeout
    fields = {}
    consumed = 0

    def take(count):
        nonlocal consumed
        consumed += count
        if consumed > 512:
            raise CheckFailure("metadata exceeds 512 bytes")
        return port.read_exact(count, deadline=deadline)

    while True:
        key = take(1)[0]
        if key == 0:
            break
        if key in fields:
            raise CheckFailure(f"duplicate metadata key 0x{key:02x}")
        kind = key >> 5
        if kind == 0:
            value = bytearray()
            while True:
                byte = take(1)[0]
                if byte == 0:
                    break
                value.append(byte)
            try:
                fields[key] = value.decode("utf-8")
            except UnicodeDecodeError as error:
                raise CheckFailure("metadata string is not UTF-8") from error
        elif kind == 1:
            fields[key] = int.from_bytes(take(4), "big")
        elif kind == 2:
            fields[key] = take(1)[0]
        else:
            raise CheckFailure(f"unsupported metadata type 0x{key:02x}")

    expected = {0x01: "STM32 SUMP Analyzer", 0x21: 28672,
                0x23: 20_000_000, 0x40: 8, 0x41: 2}
    for key, value in expected.items():
        if fields.get(key) != value:
            raise CheckFailure(f"metadata 0x{key:02x}: expected {value!r}, received {fields.get(key)!r}")
    if not fields.get(0x03):
        raise CheckFailure("firmware version is missing from metadata")
    return {f"0x{key:02x}": value for key, value in fields.items()}


class SmokeSuite:
    def __init__(self, port, output, divider, repeats, report):
        self.port = port
        self.output = output
        self.divider = divider
        self.repeats = repeats
        self.report = report

    def check(self, name, action):
        result = {"name": name, "status": "failed"}
        self.report["checks"].append(result)
        started = time.monotonic()
        try:
            result.update(action() or {})
            result["status"] = "passed"
        except Exception as error:
            result["error"] = str(error)
            raise
        finally:
            result["seconds"] = round(time.monotonic() - started, 6)
            print(f"{result['status'].upper()}: {name}", flush=True)

    def expect_id(self):
        actual = self.port.read_exact(len(IDENTITY))
        if actual != IDENTITY:
            raise CheckFailure(f"expected ID {IDENTITY!r}, received {actual!r}")

    def save_capture(self, name, data):
        path = self.output / f"{name}.bin"
        path.write_bytes(data)
        return {"samples": len(data), "file": path.name,
                "sha256": hashlib.sha256(data).hexdigest()}

    def read_capture(self, samples):
        # Include acquisition time separately from the USB/host response budget.
        duration = samples * (self.divider + 1) / SUMP_CLOCK_HZ
        deadline = time.monotonic() + self.port.timeout + duration * 2
        return self.port.read_exact(samples, deadline=deadline)

    def identify(self):
        discarded = self.port.reset_and_drain()
        self.port.write(bytes((ID, META)))
        self.expect_id()
        metadata = read_metadata(self.port)
        self.port.expect_quiet()
        self.report["metadata"] = metadata
        return {"initial_discarded_bytes": discarded}

    def capture(self, samples, name, fragmented=False):
        data = capture_configuration(samples, self.divider) + bytes((RUN,))
        if fragmented:
            # Separate host writes exercise parser boundaries. The OS may still
            # coalesce writes into USB packets; this does not prove packet layout.
            for byte in data:
                self.port.write(bytes((byte,)))
                time.sleep(0.002)
        else:
            self.port.write(data)
        samples_data = self.read_capture(samples)
        self.port.expect_quiet()
        return self.save_capture(name, samples_data)

    def repeat_runs(self):
        self.port.write(capture_configuration(68, self.divider))
        captures = []
        for _ in range(self.repeats):
            self.port.write(bytes((RUN,)))
            captures.append(self.read_capture(68))
            # Start the next RUN immediately: file I/O and quiet checks follow
            # the whole sequence so they cannot hide the completion race.
        self.port.expect_quiet()
        return {"captures": [self.save_capture(f"repeat-{index + 1:02d}", data)
                             for index, data in enumerate(captures)]}

    def batched_requests(self):
        self.port.write(bytes((RESET,)) * 5 + bytes((ID, META, ID)))
        self.expect_id()
        metadata = read_metadata(self.port)
        self.expect_id()
        self.port.expect_quiet()
        return {"metadata": metadata}

    def resynchronize(self):
        self.port.write(bytes((DIVIDER, 0x12)))
        self.port.write(bytes((RESET,)) * 5 + bytes((ID,)))
        self.expect_id()
        self.port.expect_quiet()

    def unknown_command(self):
        # These argument bytes are valid short opcodes only outside a frame.
        self.port.write(command(0xFE, 0x04020100) + bytes((ID,)))
        self.expect_id()
        self.port.expect_quiet()

    def rejected_run(self, samples, flags, name):
        self.port.write(bytes((RESET,)) * 5 +
                        capture_configuration(samples, self.divider, flags) + bytes((RUN, ID)))
        # ID is a barrier: a successful invalid capture would precede or replace
        # this reply once capture completes. Also wait through acquisition in
        # case an incorrectly accepted RUN answered ID while still sampling.
        self.expect_id()
        invalid_capture_time = samples * (self.divider + 1) / SUMP_CLOCK_HZ
        self.port.expect_quiet(invalid_capture_time * 2 + QUIET_SECONDS)
        self.port.write(capture_configuration(4, self.divider) + bytes((RUN, ID)))
        self.expect_id()  # Correct parameters alone must not clear ERROR.
        self.port.expect_quiet()
        self.port.write(bytes((RESET,)) * 5)
        return self.capture(4, name)

    def run(self):
        self.check("identity-and-metadata", self.identify)
        for samples in CAPTURE_SIZES:
            name = f"capture-{samples}"
            self.check(name, lambda samples=samples, name=name: self.capture(samples, name))
        self.check("immediate-repeated-run", self.repeat_runs)
        self.check("fragmented-command-writes", lambda: self.capture(68, "fragmented", True))
        self.check("batched-reset-id-metadata-id", self.batched_requests)
        self.check("reset-resynchronizes-partial-command", self.resynchronize)
        self.check("unknown-long-command-framing", self.unknown_command)
        self.check("disabled-input-run-recovery", lambda: self.rejected_run(
            68, CHANNEL_FLAGS | DISABLE_INPUT_GROUP, "recovery-flags"))
        self.check("oversized-run-recovery", lambda: self.rejected_run(
            CAPTURE_SIZES[-1] + 4, CHANNEL_FLAGS, "recovery-counts"))


def positive_float(value):
    number = float(value)
    if not math.isfinite(number) or number <= 0:
        raise argparse.ArgumentTypeError("must be finite and greater than zero")
    return number


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="explicit serial device path; no device discovery")
    parser.add_argument("--output", type=Path, help="new artifact directory (default: out/hardware/<UTC time>)")
    parser.add_argument("--timeout", type=positive_float, default=3.0, help="response budget in seconds, excluding capture time")
    parser.add_argument("--sample-rate", type=int, default=100_000, help="requested samples/second, 1000..1000000 (default: 100000)")
    parser.add_argument("--repeat", type=int, default=10, help="immediate 68-sample RUN count, 2..100 (default: 10)")
    parser.add_argument("--board", default="unspecified", help="board identifier recorded in report")
    parser.add_argument("--firmware-commit", default="unspecified", help="commit actually flashed, recorded as supplied")
    args = parser.parse_args(argv)
    if not 1000 <= args.sample_rate <= 1_000_000:
        parser.error("--sample-rate must be in 1000..1000000")
    if not 2 <= args.repeat <= 100:
        parser.error("--repeat must be in 2..100")
    if args.timeout < QUIET_SECONDS * 2:
        parser.error("--timeout must be at least 0.1 seconds")
    return args


def main(argv=None):
    args = parse_args(argv)
    started = datetime.now(timezone.utc)
    output = args.output or Path("out/hardware") / started.strftime("%Y%m%dT%H%M%S.%fZ")
    try:
        output.mkdir(parents=True, exist_ok=False)
    except OSError as error:
        print(f"Cannot create artifact directory: {error}", file=sys.stderr)
        return 1
    divider = (SUMP_CLOCK_HZ + args.sample_rate - 1) // args.sample_rate - 1
    report = {"started_utc": started.isoformat(), "status": "failed", "port": args.port,
              "board": args.board, "firmware_commit": args.firmware_commit,
              "requested_sample_rate_hz": args.sample_rate,
              "sump_sample_period_ns": (divider + 1) * 10,
              "repeat": args.repeat, "timeout_seconds": args.timeout, "checks": [],
              "scope": "SUMP responses and byte counts; no signal pattern or physical timing validation"}
    exit_code = 1
    try:
        with SerialPort(args.port, args.timeout, output) as port:
            try:
                SmokeSuite(port, output, divider, args.repeat, report).run()
                report["status"] = "passed"
                exit_code = 0
            finally:
                # Leave the capture stopped even when a response check fails.
                try:
                    report["cleanup_discarded_bytes"] = port.reset_and_drain()
                except Exception as error:
                    report["cleanup_error"] = str(error)
                    report["status"] = "failed"
                    exit_code = 1
    except KeyboardInterrupt:
        report["status"] = "failed"
        report["error"] = "interrupted"
        exit_code = 130
    except (CheckFailure, OSError, termios.error) as error:
        report["status"] = "failed"
        report["error"] = str(error)
        exit_code = 1
    finally:
        report["finished_utc"] = datetime.now(timezone.utc).isoformat()
        (output / "report.json").write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n")
        print(f"Report: {output / 'report.json'}")
        if "error" in report:
            print(report["error"], file=sys.stderr)
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
