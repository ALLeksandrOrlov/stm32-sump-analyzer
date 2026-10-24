# SUMP serial smoke test

[`sump_smoke.py`](../tools/hardware/sump_smoke.py) runs on Linux and macOS with
Python 3 and its standard library. Flash the board first and close PulseView or
any other application using the port. The port is required explicitly; the script
does not discover or open other devices.

Run from the repository root, substituting the actual port and flashed commit:

```sh
python3 tools/hardware/sump_smoke.py \
  --port /dev/cu.usbmodemXXXX \
  --board 'STM32F407 board identifier' \
  --firmware-commit '<commit actually flashed>'
```

On Linux, use the device path for your board, such as `/dev/ttyACM0`. Check the
port name before running the command. The script sets 115200 8N1, makes no explicit
DTR/RTS changes, and does not flash the device or configure GPIO outputs. Capture
uses the firmware's normal input configuration. No external generator is needed:
the sampled values may be arbitrary.

## Checks

- ID `1ALS`, device name, firmware version, eight channels, 28,672 bytes of memory,
  and the advertised 20 MHz limit in metadata.
- Exactly 4, 68, 1024, and 28,672 samples at a requested 100 kHz without a trigger.
- Ten consecutive RUN commands, each sent immediately after the last sample.
- Commands split across host writes, multiple commands in one write, RESET after
  an incomplete command, and argument consumption for an unknown long command.
- Rejection of RUN with the input group disabled or an oversized buffer, ID
  replies in the error state, and capture recovery after RESET.

The operating system determines how host writes become USB packets. Writing one
byte at a time does not prove that each byte used a separate USB packet.
The script sends RESET when finished and attempts to stop capture after an error.
A failed check or timeout produces a nonzero exit code.

## Results and options

Each run creates a new directory under `out/hardware/<UTC time>/`:

- `report.json`: results, settings, and operator-supplied board details.
- `tx.bin`, `rx.bin`: the recorded exchange, including data received before a
  failure.
- `capture-*.bin`, `repeat-*.bin`, and other `.bin` files: samples, one byte each,
  in SUMP transmission order from newest to oldest.

Use `--output` to select a different **new** directory. `--timeout` sets the reply
budget, defaulting to three seconds; acquisition time is accounted for separately.
`--repeat` sets the number of repeated captures. `--sample-rate` accepts rates from
1 kHz to 1 MHz.

This test does not measure the actual sampling period or throughput. Tests for
edges, sample order, USB reconnection, and maximum frequency are described in the
[hardware validation procedure](hardware-validation.md).

## Testing the host tool

The script's unit tests do not access hardware:

```sh
python3 -m unittest discover -s tests -p test_hardware_smoke.py
```

The complete scenario can also run against the
[PulseView PTY fixture](../tools/pulseview/sump_fixture.c), using
`--port /tmp/stm32-sump-analyzer`. That run validates the host tool and portable
core; it is not evidence of STM32 or physical USB behavior.
