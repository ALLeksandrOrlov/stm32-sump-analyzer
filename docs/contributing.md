# Contributing

## Development setup

```sh
git clone https://github.com/ALLeksandrOrlov/stm32-sump-analyzer.git
cd stm32-sump-analyzer
make docker-build
```

You need Git, GNU make and a running Docker installation with Buildx. You do not
need to install the ARM toolchain or QEMU on the host. Build artifacts are in
`out/ci/dist`; reports are in `out/ci/reports`. On macOS, Docker runs in a Linux
VM provided by Docker Desktop or Lima.

## Where to change code

- `core/sump_protocol.*`: command encoding, parsing and metadata.
- `core/capture.*`: physical capture parameters, the ECA controller and ring
  reads, with no protocol dependency.
- `core/analyzer_app.*`: command-to-configuration conversion, SUMP flag
  validation, and coordination between the controller and transport.
- `platform/stm32/`: registers, IRQs, the USB CDC adapter and board assembly.
  `capture_stm32.*` owns GPIO, EXTI, TIM1/TIM8 and DMA;
  `capture_task_events.*` forwards capture events through a deferred IRQ;
  `usb_cdc_port.*` manages CDC access and transfer completion;
  `analyzer_board.*` connects the components and starts the scheduler.
- `platform/freertos/`: two tasks, MessageBuffers, notifications and RTOS
  configuration.
- `third_party/FreeRTOS-Kernel/`: unmodified sources from a pinned release. Its
  [README](../third_party/FreeRTOS-Kernel/README.md) describes verification and
  updates.
- `Src/`, `Inc/`: USER CODE sections only; see the [CubeMX rules](cubemx.md).
- `qemu/`: the peripheral model used for testing.
- `tests/`: core, CDC adapter, STM32 driver and RTOS integration tests.

The core must remain C11 with no HAL, USB or FreeRTOS includes and no dynamic
allocation. Keep ISRs short: do not wait for USB or process the protocol there.
The application transport copies each accepted packet. USB itself borrows the
TX task's buffer until the complete transfer, including any ZLP, finishes or the
endpoint is physically disconnected. Each MessageBuffer must retain one
producer and one consumer. Task notifications use index 1; MessageBuffer uses
index 0.

## Readable code

Give each function one testable responsibility. Separate calculations and
parameter validation from register access and other side effects. Prefer
explicit states, named constants and early returns on errors. Include units in
variable names and bound waits for peripherals.

Public interfaces document valid inputs, results, memory ownership and calling
context: the specific task or IRQ. Inside a function, comments explain a
constraint, required hardware sequence or invariant. Avoid comments that merely
repeat the next line of C. Documentation describes current behavior and supported
limits, without narrating how the source changed.

Do not add fallback paths for unsupported modes or configurations. A request
must either have a defined result or be rejected before peripherals change.
Module boundaries should support isolated verification without duplicating the
same state in multiple places.

## Verifying changes

For a quick iteration with a host C compiler installed, run
`make test test-sanitize`. Before submitting changes, run `make docker-build`.
Register and IRQ changes need integration tests in the extended QEMU. Model
changes must be tested against the same STM32 driver and documented with their
supported behavior and limitations.

Verify queue, notification and USB lifecycle changes with
`make qemu-rtos-test QEMU=/path/to/patched/qemu-system-arm`. Two test ELF files
run the actual FreeRTOS kernel on Cortex-M4F. One uses test ports; the other
connects the tasks to the production capture driver and deferred IRQ.

Choose tests according to the contract at risk: pure calculations run natively,
registers and interrupts run in QEMU, and timing and electrical characteristics
need hardware. A test establishes an input condition, performs an action and
checks an observable result. Do not copy the implementation algorithm into the
expected result or depend on internal helper call order unless that order is
part of the contract. See the [verification guide](verification.md) for details
and priorities.

In a pull request, describe:

- The problem being solved and the resulting behavior.
- The checks performed.
- Changes to the protocol, timing or board requirements.
- Checks that still require a physical device.

Build artifacts belong in the ignored `out/` directory. The generated CubeMX
`Makefile` and `build/` directory are also excluded from Git. Do not automatically
update the CubeMX baseline: accidental changes to generated code must not become
approved by the check itself.

[Native builds](building.md) · [Architecture](architecture.md) · [CI/CD](ci-cd.md) ·
[Releases](releases.md) · [Troubleshooting](troubleshooting.md)
