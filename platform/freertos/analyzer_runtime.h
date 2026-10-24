#ifndef ANALYZER_RUNTIME_H
#define ANALYZER_RUNTIME_H

#include "capture.h"

/* All callbacks are required, run in task context and must not wait for I/O.
 * write borrows the TX task's stable buffer until
 * idle or physical disconnect. The adapter serializes access to the USB stack.
 * rearm grants room for one more full-speed OUT packet. */
typedef struct {
    void *context;
    /* Enable the prepared USB IRQ only after the scheduler has initialized
     * its interrupt-priority validation and context-switch machinery. */
    void (*activate)(void *context);
    bool (*write)(void *context, const uint8_t *bytes, size_t length);
    bool (*idle)(void *context);
    void (*rearm)(void *context);
} analyzer_usb_port_t;

/* Create static queues and tasks before initializing USB with its IRQ masked.
 * The TX task activates USB after the caller starts the scheduler. Only one
 * analyzer exists on this board. */
bool analyzer_runtime_init(capture_driver_t capture, analyzer_usb_port_t usb,
                           uint32_t timer_clock_hz);

/* USB callbacks: only IRQ priorities permitted by FreeRTOS may call these.
 * RX copies at most 64 bytes. No protocol or capture processing happens here.
 * Physical disconnect must abort the endpoint before releasing its TX buffer. */
void analyzer_runtime_usb_connected_from_isr(void);
void analyzer_runtime_usb_disconnected_from_isr(void);
void analyzer_runtime_usb_receive_from_isr(const uint8_t *bytes, uint32_t length);
void analyzer_runtime_usb_tx_complete_from_isr(void);

/* High-priority capture IRQs pend a separate syscall-safe software interrupt;
 * only that deferred interrupt calls this notification entry point. */
void analyzer_runtime_capture_event_from_isr(void);

#endif
