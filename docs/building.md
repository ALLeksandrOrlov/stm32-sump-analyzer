# Building and testing locally

The [quick start](../README.md) builds and tests the firmware in Docker. Native
tools are optional: use them for shorter development cycles or when debugging
the compiler, test runner or QEMU model. [CI/CD](ci-cd.md) describes the full
container pipeline used before publishing artifacts.

## Native firmware build

Install GNU make and GNU Arm Embedded GCC with newlib, including the
`arm-none-eabi-` compiler and binutils. CubeMX and an IDE are not required to
build the checked-in sources. Run commands from the repository root:

```sh
make -j4
```

The output files are
`out/firmware/stm32_sump_analyzer.{elf,hex,bin,map}`. To select a toolchain that
is not on PATH, set its executable prefix:

```sh
make -j4 CROSS_COMPILE=/path/to/toolchain/bin/arm-none-eabi-
```

GNU make selects the hand-maintained `GNUmakefile`. The generated CubeMX
`Makefile` is not tracked or used for the supported build. Build outputs stay
under the ignored `out/` directory. The `BUILD_DIR` variable can select a
different output directory.

## Native tests and coverage

A host C11 compiler and Python 3 are sufficient for the core and USB port tests:

```sh
make test
make test-sanitize
```

`test` runs the core and USB CDC port suites and checks generated CubeMX file
boundaries. `test-sanitize` runs both C suites with AddressSanitizer and
UndefinedBehaviorSanitizer; the host compiler must support them. Select a
specific host compiler with `HOST_CC`, for example `make test HOST_CC=clang`.

Coverage requires GNU GCC, its matching gcov and gcovr:

```sh
make coverage
```

Reports are written to `out/coverage/coverage.html`, `coverage.xml` and
`summary.json`. The target measures the portable core and USB CDC port, with
minimum thresholds of 95% line and 90% branch coverage. Configure the tools with
`COVERAGE_CC`, `GCOV` and `GCOVR` if their names differ on your system. On macOS,
the executable named `gcc` may be Apple Clang; select an installed GNU GCC for
this target. See [verification](verification.md) for coverage scope and test
selection.

The Python checks can also run independently:

```sh
python3 -m unittest discover -s tests -p 'test_*.py'
```

## Cortex-M4 tests in QEMU

All QEMU targets need the ARM toolchain with newlib and Python 3. The core suite
runs on the standard `netduinoplus2` machine:

```sh
make qemu-test
```

This uses `qemu-system-arm` on PATH. Select another executable with `QEMU`.
Peripheral and FreeRTOS integration tests require the project's extended QEMU
machine; see [the model and build instructions](qemu.md):

```sh
make qemu-stm32-test QEMU=/path/to/patched/qemu-system-arm
make qemu-rtos-test QEMU=/path/to/patched/qemu-system-arm
```

`qemu-stm32-test` exercises the STM32 capture driver. `qemu-rtos-test` runs both
the RTOS transport suite and the suite connecting the tasks to the production
capture driver and deferred IRQ. Test ELF files are built in `out/qemu/`.
The runner requires a clean exit and the expected success message within its
timeout. QEMU checks digital behavior; clock accuracy, electrical properties
and sustainable capture rates need [hardware validation](hardware-validation.md).
