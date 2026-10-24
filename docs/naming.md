# Project and module names

| Entity | Name |
| --- | --- |
| Repository | `stm32-sump-analyzer` |
| PulseView device and USB Product | `STM32 SUMP Analyzer` |
| USB Manufacturer | `stm32-sump-analyzer` |
| USB Configuration / Interface | `SUMP CDC` |
| CubeMX project | `stm32_sump_analyzer.ioc` |
| ELF, HEX, BIN, MAP | `stm32_sump_analyzer.*` |
| CI container | `stm32-sump-analyzer-ci` |
| PulseView UI container | `stm32-sump-analyzer-pulseview` |
| QEMU machine | `stm32f407-sump-analyzer` |
| Extended QEMU executable | `qemu-system-arm-sump-analyzer` |

Public device strings and the firmware version are defined in
`core/analyzer_identity.h`. SUMP and the USB adapter use the same header. The
development version is `0.1.0-dev`; update it in that header before a release.
The `1ALS` signature and command codes are defined by the SUMP protocol. USB
VID/PID values are in the USB descriptor configuration. Metadata includes the
device name and firmware version. ST HAL/CMSIS names and generated IRQ names
follow their public APIs.

Modules are named for their responsibilities. `sump_protocol` parses the
protocol, `capture` controls acquisition, `analyzer_app` connects commands to the
transport, `capture_stm32` operates peripherals, and `analyzer_board` connects
the application to the board. `capture_task_events` delivers events from
high-priority capture IRQs to the RTOS task. `analyzer_runtime` owns the
application task, `usb_message_transport` connects tasks through MessageBuffers,
and `usb_cdc_port` provides CDC access for the TX task. These names are
independent of the displayed device name.

Quantities with units use explicit suffixes, such as `timer_clock_hz`,
`sample_period_ns` and `CAPTURE_MAX_SAMPLE_RATE_HZ`. `CAPTURE_BUFFER_SAMPLES`
counts samples: the STM32 driver records each as a 32-bit DMA word, then packs
each sample into one byte for transmission. `sump_flags` belongs to the SUMP
format and stays in the application. The controller configuration contains
physical capture parameters without protocol flags.

`samples_sent` counts samples accepted by the transport; they may still be
queued or in an active transfer. Completion is determined through `idle()`.
In the USB transport, `physical_connection` distinguishes endpoint connections,
`connection_session` identifies the current command session, and `tx_generation`
distinguishes queued output before and after cancellation. A malformed RX
packet does not release memory still borrowed by USB. Interrupt handlers such
as `capture_stm32_tim8_update_irq` and `capture_stm32_dma2_stream5_irq` identify
their specific interrupt source.

`GNUmakefile` is the only supported build entry point; CubeMX does not overwrite
it during regeneration. The local directory name and Docker context can be
chosen independently of the device name.
