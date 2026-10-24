# STM32F407 analyzer model in QEMU

[`qemu/overlay/stm32f407-sump-analyzer.c`](../qemu/overlay/stm32f407-sump-analyzer.c)
adds a `stm32f407-sump-analyzer` machine based on STM32F405/Cortex-M4. It models
the peripherals used by the capture firmware, with the limits described below.
The stock `netduinoplus2` machine remains available.

## Test environments

| Command | Machine | Code under test |
| --- | --- | --- |
| `make qemu-test` | `netduinoplus2` | Portable C11 core on Cortex-M4 |
| `make qemu-stm32-test QEMU=qemu-system-arm-sump-analyzer` | `stm32f407-sump-analyzer` | Capture driver and required STM32 registers |
| `make qemu-rtos-test QEMU=qemu-system-arm-sump-analyzer` | `stm32f407-sump-analyzer` | FreeRTOS ARM_CM4F port, application and USB TX tasks, message buffers and controlled test ports; also runs the capture integration target below |
| `make qemu-rtos-capture-test QEMU=qemu-system-arm-sump-analyzer` | `stm32f407-sump-analyzer` | The same tasks with the production capture driver and deferred IRQ bridge |

The RTOS tests use the same kernel and runtime sources as the firmware. An
additional task controls test USB and capture ports: it raises interrupts,
delays IN completion, temporarily rejects transmissions and changes connection
state. Scenarios cover full queues, NAK, SUMP RESET, reconnection, RX failure
during an active transmission and the lifetime of a USB-owned TX buffer.
A USB IRQ pending before scheduler startup is enabled through `activate()`
in task context. The test's NVIC priority grouping matches the HAL configuration.

The second RTOS ELF links the production `capture_stm32` and
`capture_task_events` modules with the tasks. DMA writes samples, an ISR at
priority 1 pends an IRQ at priority 5, and the application task receives the
event and forwards the result through a message buffer. Both ELFs run the
scheduler and substitute a test USB port. Integration with the OTG controller
is checked on [physical hardware](hardware-validation.md).

## Modeled peripherals

| Peripheral | Supported behavior |
| --- | --- |
| GPIOD | Configuration registers, IDR, ODR, BSRR, input changes and EXTI |
| SYSCFG | Stock model with corrected port/pin routing |
| TIM1 | PSC/ARR, CEN, update events, TRGO and DMA requests; fixed 168 MHz clock |
| TIM8 | TIM1 ITR0 input, post-trigger sample counter, OPM and update IRQ |
| DMA2 | Stream 5/channel 6, 32-bit peripheral-to-memory transfers in direct mode, MINC, CIRC, NDTR and TC/TE IRQs |
| Test board | Controlled input pattern and DMA fault injection |

DMA writes into emulated RAM, changes NDTR and raises NVIC interrupts. Tests
execute the same `platform/stm32/capture_stm32.c`, CMSIS and HAL GPIO/NVIC code
used by the firmware. USB uses an in-memory test transport. PLL/RCC startup is
not run; clock discovery returns a fixed value.

Tests inspect full 32-bit GPIO samples before task processing, then verify that
the low bytes are packed once after DMA stops. This includes buffer boundaries
and circular captures.

The test generator at `0x60000000` does not exist on a physical STM32. Integration
tests use these registers:

| Offset | Meaning |
| --- | --- |
| 0x00 | Read-only ID `0x534c4131` (`SLA1`) |
| 0x04 | GPIOD IDR input levels, read/write |
| 0x08 | Input counter increment |
| 0x0c | Bit 0 enables automatic input changes before each TIM1 sample |
| 0x10 | TIM1 event count; a write resets the counter |
| 0x14 | Inject an error on the next DMA request |

## Build the model

The root Dockerfile and `make docker-build` include the complete model build and
test sequence. See [CI/CD](ci-cd.md) for individual test commands inside the
container.

For a standalone Linux build, install a C/C++ compiler, Ninja, Python with
`venv`, pkg-config, bzip2, and development headers for GLib 2.66 or newer,
libfdt and zlib. Building the test ELF files also requires the Arm GNU Toolchain
(`arm-none-eabi-gcc`) and newlib. Run from the repository root:

```sh
git clone --depth 1 --branch v9.2.4 https://github.com/qemu/qemu.git /tmp/qemu-analyzer
# Expected commit: b2995afed254e4804611df52210067a4d5dbdf0b
python3 qemu/apply_overlay.py /tmp/qemu-analyzer
mkdir -p out/qemu-build
cd out/qemu-build
/tmp/qemu-analyzer/configure --target-list=arm-softmmu --disable-docs \
  --disable-tools --disable-user --disable-plugins --disable-pixman
ninja qemu-system-arm
cd ../..
make qemu-stm32-test QEMU="$PWD/out/qemu-build/qemu-system-arm"
make qemu-rtos-test QEMU="$PWD/out/qemu-build/qemu-system-arm"
```

The overlay installer checks the QEMU version and can be run repeatedly. It adds
the machine source, an entry in `hw/arm/meson.build` and a routing correction in
`hw/misc/stm32f4xx_syscfg.c`. QEMU 9.2.4's EXTICR handling uses the full
`port * 16 + pin` value where only `pin` is required; selecting GPIOD otherwise
triggers an assertion.

## Validation limits

The model does not implement USB OTG, PLL startup, other DMA streams or
channels, FIFO, half-transfer IRQs, DBM, memory-to-memory transfers, arbitrary
timer modes, PWM, input capture, other GPIO ports, state migration or detailed
bus timing. Registers needed by HAL initialization retain their written values,
but some hardware side effects are outside the model. PSC/ARR change update
intervals; continuous reads of TIM1 CNT phase are unsupported.

Unsupported DMA directions, widths or channels, and enabled PINC, raise a
transfer error. DMA byte and halfword reads of IDR are rejected because the
register requires 32-bit access under RM0090 §8.4.5. Narrow CPU reads of IDR
return zero and log a guest error. Register writes use 32-bit accesses.

The model detects software errors in peripheral configuration, DMA state,
interrupt handling and sample order. It cannot measure real STM32 throughput,
ISR latency, trigger jitter or electrical characteristics. These integration
tests do not run the complete USB firmware with PulseView.

References:

- [Upstream QEMU STM32 support](https://www.qemu.org/docs/master/system/arm/stm32.html)
- [ST RM0090 reference manual](https://www.st.com/resource/en/reference_manual/rm0090-stm32f407-advanced-armbased-32bit-mcus-stmicroelectronics.pdf)

The model source is licensed under GPL-2.0-or-later for inclusion in QEMU.
