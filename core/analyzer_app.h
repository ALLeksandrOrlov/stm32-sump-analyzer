/**
 * @file analyzer_app.h
 * @brief Application service joining SUMP commands, capture, and transport.
 *
 * All functions run in one owner task. Interrupt adapters enqueue received
 * bytes; they must not parse commands or call this service. Every operation is
 * bounded, and no storage is allocated dynamically.
 */
#ifndef ANALYZER_APP_H
#define ANALYZER_APP_H

#include "capture.h"
#include "sump_protocol.h"

#define ANALYZER_TRANSFER_BYTES 64U

/**
 * @brief Nonblocking byte transport. Every callback is required.
 *
 * write() accepts a whole packet or nothing. On success it has copied all bytes
 * into transport-owned storage, allowing the caller to reuse its source.
 * idle() is true only after both queued and active packets have completed or
 * been cancelled. cancel() drops queued work; an active packet may complete,
 * and its transport-owned storage must remain valid until hardware releases it.
 */
typedef struct {
    void *context;
    bool (*write)(void *context, const uint8_t *bytes, size_t length);
    bool (*idle)(void *context);
    void (*cancel)(void *context);
} analyzer_transport_t;

/** Caller-owned application storage; use the API instead of modifying fields. */
typedef struct {
    sump_parser_t parser;
    uint32_t sump_flags;
    capture_config_t config; /**< Pending settings, independent of the active plan. */
    capture_controller_t capture;
    analyzer_transport_t transport;
    uint8_t reply[ANALYZER_TRANSFER_BYTES];
    size_t reply_length; /**< A pending reply applies backpressure to command input. */
    uint8_t tx[ANALYZER_TRANSFER_BYTES];
    uint32_t samples_sent; /**< Bytes accepted by transport, possibly still in flight. */
} analyzer_app_t;

/**
 * @brief Initialize application storage without starting hardware or transport.
 * @pre app is non-NULL; both ports contain every documented callback.
 */
void analyzer_app_init(analyzer_app_t *app, capture_driver_t driver,
                       analyzer_transport_t transport, uint32_t timer_clock_hz);

/**
 * @brief Stop capture and discard parser state, settings, and queued responses.
 *
 * Also used on USB disconnect. A packet already owned by the USB controller may
 * complete after reset; its storage belongs to the transport, not this service.
 */
void analyzer_app_reset(analyzer_app_t *app);

/**
 * @brief Consume a byte and apply a command only when its frame is complete.
 * @return false without consuming byte when a pending reply needs queue space,
 *         except RESET, which cancels that reply immediately.
 *
 * The caller must retry the same byte after poll() progresses. A reset received
 * inside a long command is an argument byte; transport loss uses reset() directly.
 */
bool analyzer_app_receive(analyzer_app_t *app, uint8_t byte);

/**
 * @brief Poll one capture event and attempt at most one output packet.
 * @return true if the capture state changed or a packet was accepted.
 *
 * A false return means this service has no immediate progress to report. The
 * owner can wait for a hardware or transport notification before retrying.
 * Capture bytes remain contiguous; pending control replies follow the complete
 * capture, including delivery of its final packet.
 */
bool analyzer_app_poll(analyzer_app_t *app);

#endif
