#ifndef ANALYZER_RUNTIME_H
#define ANALYZER_RUNTIME_H

#include "capture.h"

/**
 * @brief Nonblocking USB adapter used exclusively by runtime tasks.
 *
 * The adapter serializes its USB stack accesses with the USB IRQ. write()
 * borrows the supplied bytes until idle() reports completion or the endpoint
 * is closed. rearm() permits one further full-speed OUT packet. All callbacks
 * are required; none may wait for USB traffic.
 */
typedef struct {
    void *context;
    /** Enable the prepared USB IRQ after the scheduler has started. */
    void (*activate)(void *context);
    /** Borrow one immutable packet; false means no ownership was taken. */
    bool (*write)(void *context, const uint8_t *bytes, size_t length);
    /** True after the endpoint has released its borrowed packet. */
    bool (*idle)(void *context);
    /** Arm OUT after capacity is reserved; false leaves reception unarmed. */
    bool (*rearm)(void *context);
} analyzer_usb_port_t;

/**
 * @brief Create the singleton analyzer with entirely static storage.
 * @pre Call once before scheduler startup, with USB interrupts disabled.
 * @param timer_clock_hz Capture timer input frequency, in hertz.
 * @return False for invalid adapters, repeated initialization or task failure.
 *
 * The application task owns command parsing and capture control. The TX task
 * owns USB IN submission. Only the TX task activates USB interrupts.
 */
bool analyzer_runtime_init(capture_driver_t capture, analyzer_usb_port_t usb,
                           uint32_t timer_clock_hz);

/**
 * @brief Begin a fresh USB and parser session. FreeRTOS-safe USB ISR only.
 * @pre CDC has closed any previous endpoint and armed its first OUT packet.
 */
void analyzer_runtime_usb_connected_from_isr(void);
/**
 * @brief Invalidate the connection and parser session. USB ISR only.
 * @pre The endpoint has stopped accessing its borrowed IN buffer.
 */
void analyzer_runtime_usb_disconnected_from_isr(void);
/**
 * @brief Copy at most 64 received bytes and notify tasks. USB ISR only.
 *
 * No parsing or capture operation occurs here. A malformed callback resets
 * the parser session in task context without releasing an active IN buffer.
 */
void analyzer_runtime_usb_receive_from_isr(const uint8_t *bytes, uint32_t length);
/**
 * @brief Retry deferred endpoint work after USB resumes. USB ISR only.
 * @pre The adapter has restored the device state and PHY clock.
 *
 * Resume preserves parser state, queued packets and endpoint buffer ownership.
 */
void analyzer_runtime_usb_resumed_from_isr(void);
/** @brief Wake TX after the endpoint releases its packet. USB ISR only. */
void analyzer_runtime_usb_tx_complete_from_isr(void);
/**
 * @brief Wake capture processing from a FreeRTOS-safe deferred interrupt.
 *
 * Capture interrupts above the kernel priority boundary must pend a separate
 * software interrupt instead of calling this function directly.
 */
void analyzer_runtime_capture_event_from_isr(void);

#endif
