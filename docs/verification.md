# Verification strategy

Tests focus on contracts and failures that can corrupt a capture: DMA overruns,
commands lost at packet boundaries, modification of an active TX buffer, incorrect
sample order, and failure to recover from errors. A passing test establishes the
checked contract in its stated environment.

## Test levels

| Level | What it checks | Outside its scope |
| --- | --- | --- |
| Native core | SUMP, parameter validation, state machine, period calculation, sample order, transmission | STM32 registers, IRQs, physical USB |
| Native CDC adapter | Busy rejection, memory ownership, ZLP completion, nested critical sections, USB IRQ enabling | USB hardware, scheduler, ST library |
| PulseView fixture build | Compatibility of the test transport and signal generator with core interfaces | GUI execution, signal decoding, physical USB |
| ASan/UBSan | Memory errors and undefined behavior on executed paths | Unexecuted paths and hardware races |
| Core in QEMU Cortex-M4 | Execution of the same C code on the target architecture | Physical MCU characteristics |
| STM32 driver in extended QEMU | Register-level GPIO, DMA, timers, EXTI, interrupt handlers | Model accuracy, bus delays, maximum frequency |
| FreeRTOS in QEMU Cortex-M4F | Real kernel and port, two worker tasks, MessageBuffers, IRQ notifications, cancellation, reconnection | Physical USB and capture peripherals are replaced by controlled test ports |
| FreeRTOS and STM32 driver in extended QEMU | Command-to-capture path, DMA, deferred notification from priority-1 ISR through priority-5 IRQ to task, sample transmission | USB uses a test port; physical model limitations still apply |
| Firmware build | Combined application, FreeRTOS, adapters, HAL, USB, memory layout | Execution on a physical board |
| Board with generator and PulseView | USB, electrical levels, sample rate, trigger accuracy | Other boards and untested operating modes |

The QEMU model and the driver must not share an algorithm for calculating expected
samples. A test supplies the signal and checks the result independently, so a
defect cannot be duplicated in both implementation and oracle. See the
[QEMU model documentation](qemu.md) for its boundaries.

## Contracts and priorities

| Area | Required behavior |
| --- | --- |
| SUMP stream | Commands survive USB packet splits; arguments are little-endian; an unknown long command consumes all five bytes; RESET within an argument is data |
| Metadata | Complete, bounded replies; ID and capabilities match the device; numeric metadata is big-endian |
| Command conversion | The 24-bit divider converts to nanoseconds; counts expand without overflow; unsupported bits across the full 32-bit FLAGS value are rejected at RUN |
| Capture plan | Minimum and maximum sizes, values on both sides of boundaries, post ≤ total, positive period, rejection of multiple trigger channels or nonexistent channel bits |
| Timer | Actual rate does not exceed the request or device limit; period boundaries, ARR/PSC transition, rejection beyond timer capacity |
| State machine | Valid transitions, rejection of RUN while active, configuration snapshot at RUN, RESET, recovery from ERROR |
| Core transport | Copy on acceptance, multiple queued packets, full queue, retried write without losing position, waiting for full completion, no control replies inserted into samples |
| Cancellation | RESET discards queued packets and retains the active packet; a new capture cannot overwrite memory used by an earlier USB transfer |
| CDC adapter | Rejected writes retain caller ownership; release follows ZLP completion; nested critical sections restore BASEPRI without masking capture IRQs |
| FreeRTOS tasks | One writer and reader per MessageBuffer, partial and batched commands, NAK on full queue, sleeping without events, BUSY retries, message order |
| USB sessions | Disconnect/reconnect, first packet of a new session, stale-command rejection, RX failure during active TX, valid zero-length packet |
| RTOS startup | A pending USB IRQ is enabled only after kernel startup with HAL priority grouping; parser and drivers run outside the USB ISR |
| STM32 driver | 32-bit GPIO reads, one-time packing in task context, minimum/full buffer, ring wrap, both edges, post-trigger counter, cancellation, DMA error precedence over simultaneous completion, recovery |
| Capture and RTOS | Real driver handlers wake the task through a deferred IRQ; DMA data passes through the controller, application, MessageBuffer, and USB port |
| Infrastructure | Process failure and timeout, incomplete packaging, stale-artifact rejection, dependency/artifact checksums, cleanup confined to out |
| CubeMX | Paired USER CODE markers and unchanged generated text outside user sections |

This table defines the scope. Tests and individual run reports record the scenarios
actually executed. Hardware risks require the separate
[board validation procedure](hardware-validation.md).

## Writing tests

1. Name the observable behavior: initial condition, action, and expected result.
2. Prepare the smallest useful state without relying on another test.
3. Assert results through the public interface. Check side effects separately when
   they form part of the contract, such as refusing to start DMA for invalid
   parameters or not rearming OUT without room for a complete packet.
4. Partition numeric inputs into valid and invalid classes, then test boundaries.
   Exhaustive enumeration is appropriate for small spaces such as command bytes.
5. For a bug fix, add a minimal scenario that reproduces the condition and checks
   the correct result.

Transport and driver test doubles must support controlled failures and completion.
They must not duplicate the implementation under test. Firmware logic tests are
deterministic and do not depend on networks or real-time waits. Infrastructure
tests use real deadlines when checking timeout handling and termination of hung
processes. Emulator runs require both a timeout and an expected success marker;
exit code zero alone is insufficient.

## Running tests and reading reports

Run the full suite in the same environment used by GitHub Actions:

```sh
make docker-build
```

For faster checks with local tools:

```sh
make test
make test-sanitize
make coverage
make qemu-test
make qemu-stm32-test QEMU=/path/to/qemu-system-arm-sump-analyzer
make qemu-rtos-test QEMU=/path/to/qemu-system-arm-sump-analyzer
```

Coverage requires GCC, gcov, and gcovr. `out/coverage/coverage.html` reports executed
lines and branches in `core/` and `platform/stm32/usb_cdc_port.c`. XML and JSON in
the same directory support automated processing. The two task implementations,
FreeRTOS, register-level capture driver, HAL, ST USB stack, and QEMU are outside
this native metric.

`qemu-rtos-test` runs two ELF images with real FreeRTOS: controlled-port tests and
integration with the register-level capture driver on the extended model.
`qemu-stm32-test` checks peripherals and the driver in greater detail. These tests
do not establish physical USB correctness or board timing.

`make coverage` fails below 95% line coverage or 90% branch coverage within that
scope. These thresholds prevent regression; they are not a universal testing
standard. Changes to thresholds or scope must explain their effect on the tested
contracts rather than merely make CI pass.

Coverage helps identify untested branches and decide whether another scenario is
needed or unreachable code should be removed. A high percentage alone does not
establish assertion quality. Do not add artificial calls solely to raise coverage
or substitute them for checks of data, errors, and recovery.

The full pipeline writes `out/ci/reports/summary.md`, `results.json`, `junit.xml`,
stage logs, and coverage reports under `out/ci/reports/coverage/`. JUnit records
stage outcomes, not a separate test case for each C assertion. Validated firmware
appears in `out/ci/dist/` only after every stage succeeds.

Results apply to specific sources and environments. When sharing them, record the
commit, local modifications, and tool versions, and retain the reports. Container
build metadata is in `build-info.json`. The
[PulseView fixture screenshots](pulseview-screenshots.md) demonstrate synthetic
data and do not replace a physical board test report.
