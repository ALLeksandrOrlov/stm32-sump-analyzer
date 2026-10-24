# Hardware validation

Start with the [SUMP smoke test](hardware-smoke.md) to check USB and commands
without a signal generator. It saves replies, samples, and a JSON report.
An external signal is needed to validate waveforms and timing.

Emulation does not establish DMA throughput, physical USB behavior, or trigger
accuracy. After building and flashing the firmware, use this test procedure:

1. Verify the 8 MHz HSE, supply, and PD0–PD7 connections on the board. Some
   Discovery boards connect onboard circuitry to PD5; consult the board schematic.
   PA9 must receive VBUS from the USB OTG FS connector in use. The firmware enables
   hardware VBUS sensing and configures PA9 as an input without pulls. On
   STM32F4DISCOVERY this is the USB VBUS net. For other boards, verify the circuit
   and permitted connection. Do not assign PA9 to USART or another signal.
   Supplying the board separately does not replace the VBUS signal.
2. Test power-on with USB already connected, then connection after the board has
   started. The TX task must enable the USB IRQ only after the scheduler starts.
   Request ID and metadata through PulseView/SUMP. Expect `1ALS`, eight channels,
   28,672 bytes of sample memory, and an advertised maximum of 20 MHz.
3. Apply a square wave of known frequency. Capture 4, 68, 1024, and 28,672 samples
   without a trigger. Check sample count, order, and signal period.
4. Test rising and falling edges on one channel, the pre/post-trigger split, and
   an edge arriving after several wraps of the DMA ring.
5. Send RESET while waiting for a trigger and during transmission, then start a
   new RUN. Cancellation discards queued packets while retaining an active packet
   until completion. Start another RUN immediately after receiving the final
   sample. Disconnect and reconnect USB both during transmission and between
   captures. Keep the board powered from the debugger or a separate supply while
   removing the OTG FS cable. Confirm that capture stops, stale commands are
   discarded, and a new ID/RUN succeeds after reconnection without a power cycle.
6. Test sample rates from low values to the device limit. Measure trigger-position
   jitter with USB active. Stopping post-trigger acquisition from an ISR does not
   guarantee single-sample accuracy at the maximum rate.
7. Request an oversized buffer and unsupported RLE/DDR modes. DMA must not start.
   Send RESET and valid settings before the next acquisition.
8. Pause USB reads on the host until the queues fill, then resume reading. Check
   for lost commands, damaged packets, or stalled tasks. Measure the minimum free
   task stack with `uxTaskGetStackHighWaterMark2()` during full-buffer captures,
   repeated metadata requests, and USB reconnection.

For each test, record the board revision, firmware commit, generator frequency,
capture settings, and sample file. Compare pulse counts and periods with an
independent source or instrument. Published results must state the measured
frequency range and trigger accuracy for that board.

## USB disconnect handling

A drop in VBUS produces an OTG FS Session End event. HAL forwards it to
`USBD_LL_DevDisconnected()`. CDC closes the IN, OUT, and command endpoints before
calling `CDC_DeInit_FS()`. Only then is the application notified of the disconnect
and allowed to release memory held by an unfinished transfer. USB suspend alone
does not indicate cable removal.
