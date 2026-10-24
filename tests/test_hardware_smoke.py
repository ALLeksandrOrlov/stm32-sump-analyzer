"""Test the host smoke runner without opening a hardware serial device."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import pty
import tempfile
import time
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location(
    "hardware_smoke", Path(__file__).parents[1] / "tools/hardware/sump_smoke.py")
smoke = importlib.util.module_from_spec(spec)
spec.loader.exec_module(smoke)


def metadata_bytes():
    return (b"\x01STM32 SUMP Analyzer\0\x030.1.0-dev\0" +
            b"\x23" + (20_000_000).to_bytes(4, "big") +
            b"\x21" + (28672).to_bytes(4, "big") + b"\x40\x08\x41\x02\0")


class MetadataPort:
    timeout = 1

    def __init__(self, data):
        self.stream = io.BytesIO(data)

    def read_exact(self, count, **_):
        data = self.stream.read(count)
        if len(data) != count:
            raise smoke.CheckFailure("truncated response")
        return data


class HardwareSmokeTests(unittest.TestCase):
    def test_port_is_required_and_validation_happens_before_open(self):
        with mock.patch.object(smoke.os, "open") as open_port, \
                contextlib.redirect_stderr(io.StringIO()):
            for args in ([], ["--port", "/unused", "--sample-rate", "0"],
                         ["--port", "/unused", "--timeout", "nan"],
                         ["--port", "/unused", "--repeat", "1"]):
                with self.subTest(args=args), self.assertRaises(SystemExit):
                    smoke.main(args)
            open_port.assert_not_called()

    def test_configuration_is_little_endian_and_selects_no_trigger(self):
        data = smoke.capture_configuration(68, 999)
        self.assertEqual(data, bytes.fromhex(
            "80 e7 03 00 00 81 10 00 10 00 82 38 00 00 00 c0 00 00 00 00"))
        for samples in (0, 3, 69, 262148):
            with self.subTest(samples=samples), self.assertRaises(ValueError):
                smoke.capture_configuration(samples, 999)

    def test_metadata_numbers_are_big_endian_and_embedded_zeroes_are_not_end(self):
        fields = smoke.read_metadata(MetadataPort(metadata_bytes()))
        self.assertEqual(fields["0x21"], 28672)
        self.assertEqual(fields["0x23"], 20_000_000)
        self.assertEqual(fields["0x03"], "0.1.0-dev")

    def test_metadata_rejects_incomplete_duplicate_wrong_and_unbounded_fields(self):
        valid = metadata_bytes()
        invalid = (valid[:-1], valid[:-1] + b"\x40\x08\0",
                   valid.replace(b"\x40\x08", b"\x40\x10"),
                   b"\x01" + b"A" * 512, b"\x60", b"\x01\xff\0")
        for data in invalid:
            with self.subTest(data=data[:16]), self.assertRaises(smoke.CheckFailure):
                smoke.read_metadata(MetadataPort(data))

    def test_pty_io_records_bytes_and_bounds_a_missing_response(self):
        master, slave = pty.openpty()
        try:
            with tempfile.TemporaryDirectory() as directory:
                output = Path(directory)
                with smoke.SerialPort(os.ttyname(slave), 0.1, output) as port:
                    port.write(b"\x00\x02")
                    self.assertEqual(os.read(master, 2), b"\x00\x02")
                    os.write(master, b"1ALS")
                    self.assertEqual(port.read_exact(4), b"1ALS")
                    start = time.monotonic()
                    with self.assertRaisesRegex(smoke.CheckFailure, "expected 1 bytes, received 0"):
                        port.read_exact(1)
                    self.assertLess(time.monotonic() - start, 1.0)
                    os.write(master, b"extra")
                    with self.assertRaisesRegex(smoke.CheckFailure, "trailing response"):
                        port.expect_quiet()
                self.assertEqual((output / "tx.bin").read_bytes(), b"\x00\x02")
                self.assertEqual((output / "rx.bin").read_bytes(), b"1ALSextra")
        finally:
            os.close(master)
            os.close(slave)

    def test_open_failure_still_saves_failure_report(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "run"
            with mock.patch.object(smoke.os, "open", side_effect=OSError("test open failure")), \
                    contextlib.redirect_stdout(io.StringIO()), \
                    contextlib.redirect_stderr(io.StringIO()):
                result = smoke.main(["--port", "/unused", "--output", str(output)])
            self.assertEqual(result, 1)
            report = json.loads((output / "report.json").read_text())
            self.assertEqual(report["status"], "failed")
            self.assertEqual(report["error"], "test open failure")


if __name__ == "__main__":
    unittest.main()
