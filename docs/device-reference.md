# Device reference

STM32 SUMP Analyzer exposes eight logic channels through SUMP over USB CDC.
The firmware targets STM32F407 with an 8 MHz HSE and a 168 MHz system clock.
PA9 must receive VBUS from the USB FS connector so cable removal is detected
when the board remains powered through another connection.

## Channels and capture storage

Channels 0–7 map to PD0–PD7. Each transmitted sample is one byte; bit 0 represents
PD0 and bit 7 represents PD7. The driver configures these pins as inputs with
internal pull-downs.

The maximum capture depth is 28,672 samples. The requested total and post-trigger
counts are checked before DMA starts. SUMP encodes each count in units of four
samples, using `(encoded + 1) * 4`. A post-trigger count must be positive and no
larger than the total count.

DMA reads GPIO IDR with 32-bit accesses into a 112 KiB buffer. After acquisition
stops, the application task packs the low byte of each word in place, producing
up to 28 KiB of sample data without another capture buffer. SUMP receives
samples newest first; the client reconstructs chronological order for display.

## Sample rate

Device metadata advertises a maximum of 20 MHz. The timer plan rounds the
requested period upward, so the configured rate exceeds neither the request
nor that ceiling. With the standard 168 MHz TIM1 clock, the fastest permitted
configuration divides the timer clock by nine:

```text
168 MHz / 9 = 18.666… MHz
```

This approximately 18.667 MHz value describes timer quantization. It is not a
measured guarantee that GPIO and DMA sustain that rate without missing input
transitions. The sustainable maximum, timing accuracy and jitter require
measurement on the board. The [hardware report](hardware-results/stm32f4-discovery-2026-10-24.md)
states which scenarios have been tested.

## Capture and trigger behavior

- With no trigger selected, capture completes on DMA Transfer Complete.
- A trigger selects the rising or falling edge of one channel through EXTI.
  Masks with multiple channels or channels outside 0–7 are rejected.
- Before the trigger, DMA records into a circular buffer. The edge handler
  starts TIM8, which counts TIM1 updates for the post-trigger interval.
- Completion stops acquisition in an ISR. Interrupt latency therefore affects
  trigger placement in the returned samples.
- A RUN received during capture or transmission does not restart the active
  acquisition. Configuration commands apply to the next capture.

Multibit level comparison and multistage triggers are not implemented.

## Unsupported settings and recovery

RLE, DDR, external sampling clocks and disabling channel group 0 are unsupported.
Group 0 contains the device's eight channels. Disabling absent groups 1–3 is
accepted. Other unsupported FLAGS bits are rejected as well.

A RUN from IDLE with unsupported settings or invalid counts enters ERROR before
DMA starts. SUMP provides no separate error response for a rejected RUN, so the
client receives no capture data. Send RESET to return to IDLE and restore the
default configuration: a requested 1 MHz rate, 1,024 total samples, 1,024
post-trigger samples and no trigger. A DMA or driver startup failure also enters
ERROR and requires RESET.

The FPGA filter flag is accepted as a client hint because libsigrok's OLS driver
sets it automatically at these rates. It does not modify GPIO samples; there
is no digital filter in the firmware.

Unknown commands are ignored after their entire frame is consumed. In
particular, an unknown long command consumes all four argument bytes so the
next command remains aligned. USB packet boundaries do not define command
boundaries. See [architecture](architecture.md) for parser, transport and memory
ownership details.
