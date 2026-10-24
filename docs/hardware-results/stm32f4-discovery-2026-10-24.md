# STM32F4 Discovery test results — 24 October 2026

The physical board passed 12 USB/SUMP checks, six PD0 capture checks, and 30,000 ID
requests without corrupted or missing replies. Removing USB while waiting for a
trigger stopped capture; reconnecting restored communication. The container CI
run for the same firmware revision passed all nine stages.

The additional [USER button test](stm32f4-discovery-user-button-2026-10-24.md)
uses a separate diagnostic build with channel 0 on PA0. Its results and restoration
of the normal input configuration are documented separately.

## Device and conditions

| Parameter | Value |
| --- | --- |
| Board | STM32F4 Discovery, STM32F407 |
| Source revision | `51bf62b7d927a4b8163999f41db067e54011478a` |
| Source state at build | Clean working tree |
| Compiler | Arm GNU Toolchain GCC 12.2.1, `20221205` |
| Firmware | `stm32_sump_analyzer.elf` from container CI |
| Command and sample transport | Physical USB OTG FS, CDC |
| Debug probe | Onboard ST-Link running J-Link firmware; SWD at 250 kHz |
| Signal source | The board's PD0 output, read through GPIO IDR |
| External signal connections | None |

SHA-256 checksums of artifacts from that CI build:

| File | SHA-256 |
| --- | --- |
| `stm32_sump_analyzer.elf` | `f6b4772cd9247245eb38aaaf91af8a363f1b7feb95f60e5e35c3c73278554e9b` |
| `stm32_sump_analyzer.bin` | `ecfa7eb1d9ee07b4c27c589b98ee1bc3cbe87c824268e18ead4f28ef36f2d8d1` |

The debugger did not halt the CPU during testing. For the PD0 checks, the pin was
temporarily configured as an output and changed through SWD writes to GPIO.
Firmware capture continued running and returned data through USB. The GPIO mode
was restored afterwards; the report records `restored_moder: 0x0`.

This setup exercises GPIO → DMA → application → USB and edge handling on one
channel. Its signal source is not an independent frequency or timing instrument.

## USB and SUMP commands — 12/12

Captures requested 100,000 samples/s, a SUMP period of 10,000 ns. This was a
commanded rate; its physical accuracy was not measured in this test.

| Check | Result |
| --- | --- |
| ID and metadata | Passed |
| 4-sample capture | Received 4 bytes |
| 68-sample capture | Received 68 bytes |
| 1024-sample capture | Received 1024 bytes |
| Full-buffer capture | Received 28,672 bytes |
| RUN immediately after a capture | Passed 10 consecutive 68-sample captures |
| Commands split across host USB writes | Passed; received 68 samples |
| RESET, ID, metadata, and ID in one batch | Replies verified |
| RESET after an incomplete command | Synchronization recovered |
| Unknown long command | Subsequent command boundaries preserved |
| RUN with input group disabled | Recovery and subsequent 4-sample capture verified |
| RUN exceeding buffer capacity | Recovery and subsequent 4-sample capture verified |

The device reported `STM32 SUMP Analyzer`, version `0.1.0-dev`, eight channels,
28,672 bytes of sample memory, and a maximum rate of 20 MHz. The last value is
device metadata; this run does not establish capture at 20 MHz. Initial and final
transport drains found no extra bytes.

## PD0 signal — 6/6

| Check | Result |
| --- | --- |
| Constant low | Verified 128 samples |
| Constant high | Verified 128 samples |
| Three level changes | Verified 1024 samples and the order `0 → 1 → 0 → 1` |
| Rising edge | Received 128 samples |
| Falling edge | Received 128 samples |
| RESET while waiting for an edge | Received the correct four-byte ID |

The three-transition capture contained chronological groups of 210 low, 211 high,
205 low, and 398 high samples. These counts establish the observed sequence of
levels; they are not used to estimate frequency, jitter, or trigger latency.

## USB with host reading paused

The host sent 30,000 ID requests and paused reply reads for 0.5 s. After reads
resumed, exactly 120,000 bytes were received: 30,000 consecutive `1ALS` replies,
with no omissions, corruption, or extra data. Reading after the pause took about
1.358 s. This is the duration of this host scenario, not a maximum USB throughput
measurement.

The `paused_state` diagnostic fields were read sequentially while the CPU was
running. They are not an atomic snapshot, so the difference between accepted and
released message counters is not used to estimate queue depth or detect leaks.
The result is based on comparing the complete received reply sequence with the
expected bytes.

## Physical USB removal and reconnection

The USB OTG FS cable was removed while waiting for a rising edge on PD0. The board
remained powered by the debug probe. Application state and DMA/timer enables were
read through SWD without halting the CPU.

| Stage | USB `connected` | Capture state | DMA | Timer |
| --- | ---: | --- | --- | --- |
| Before removal | 1 | `ARMED` | Enabled | Enabled |
| After removal | 0 | `IDLE` | Disabled | Disabled |
| After reconnection | 1 | `IDLE` | Disabled | Disabled |

After reconnection, the `1ALS` ID reply and a new 68-sample capture were verified.
Exactly 68 bytes arrived; the extra-data check and final RESET passed. This
establishes cancellation of an armed capture on cable removal and recovery of
command exchange without a power cycle. Removal during an active USB IN transfer
was not tested by this scenario.

## FreeRTOS state after load

After the load tests, the scheduler was running (`xSchedulerRunning = 1`) with
three tasks. `xTickCount` advanced from 266,728 to 266,837 between two reads.
At the time of reading, `CFSR`, `HFSR`, and the recorded fatal-error fields
`failure_file` and `failure_line` were all zero.

Stack dumps gave the following estimates of minimum free space. One stack word
is four bytes.

| Task | Stack size, words | Observed free space, words | Free space, bytes |
| --- | ---: | ---: | ---: |
| Application (`application`) | 768 | 683 | 2732 |
| USB TX (`transmitter`) | 512 | 453 | 1812 |
| Idle | 256 | 236 | 944 |

The estimate uses the contiguous `0xA5` fill region starting at the low address of
each stack, as in a FreeRTOS high-water mark. It is a heuristic for the executed
scenario, not proof of maximum stack depth under every possible load. The dumps
were taken without halting the CPU and do not form an atomic snapshot of all tasks.

## Peripheral configuration

Register reads confirmed the configuration used in the tests:

| Register | Value | Verified fields |
| --- | --- | --- |
| `DMA2_Stream5_CR` | `0x0c025507` | `PSIZE = 2`, `MSIZE = 2`: 32-bit peripheral reads and memory writes |
| `GPIOA_MODER` | `0xaa800000` | `MODER9 = 0`: PA9 is an input |
| `USB_GCCFG` | `0x0009ffff` | `VBUSBSEN = 1`, `NOVBUSSENS = 0`: hardware VBUS sensing enabled |

These values establish DMA and USB configuration. Physical cable removal was
checked in the armed-trigger scenario described above.

## Container CI — 9/9

The `pipeline`, `cubemx`, `native`, `sanitizers`, `coverage`, `qemu-core`,
`qemu-stm32`, `qemu-rtos`, and `firmware` stages all succeeded. The build used
QEMU 9.2.4 with the project model; `build-info.json` records `tests: passed`.

QEMU and physical board tests complement each other. The
[CI/CD guide](../ci-cd.md) describes the automated checks; the
[verification strategy](../verification.md) defines their boundaries.

## Remaining validation

- Physical USB removal during an active USB IN transfer.
- Cold start after removing all power.
- Independent measurement of sample rate, jitter, and trigger-position accuracy,
  including the maximum rate with USB active.
- Input electrical characteristics and an external signal source.
- Interrupt-handler duration.
- Stack free space under other loads, including USB reconnection.

PD0 testing does not establish the electrical or timing properties of the other
seven channels. Further measurements follow the
[hardware validation procedure](../hardware-validation.md).

## Evidence

Results come from these run artifacts for `51bf62b`:

- `usb-smoke/report.json`: commands, capture lengths, and data checksums.
- `pd0-loopback/report.json`: six PD0 checks.
- `usb-stress/report.json`: ID requests with host reading paused.
- `disconnect-observed/report.json`: physical removal in `ARMED` and recovery
  after reconnection.
- `runtime-and-stacks.json`: scheduler state, fault registers, stack-space
  estimates.
- `peripheral-settings.json`: DMA, GPIO, and USB register reads.
- `reports/summary.md` and `reports/results.json`: nine CI stage outcomes.
- `dist/build-info.json`: source revision and build tools.

Board-specific USB identifiers and local test-system paths are omitted from this
public report.
