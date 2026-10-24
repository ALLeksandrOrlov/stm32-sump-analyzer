#ifndef ANALYZER_APP_H
#define ANALYZER_APP_H

#include "capture.h"
#include "sump_protocol.h"

#define ANALYZER_TRANSFER_BYTES 64u

typedef struct {
    void *context;
    /* All callbacks are required and must return without waiting for I/O. */
    /* Nonblocking and all-or-nothing: true means the transport copied every
     * byte, so the caller may reuse the source immediately. False accepts none. */
    bool (*write)(void *context, const uint8_t *bytes, size_t length);
    /* True only when all accepted bytes have completed or been cancelled.
     * A queued packet or an active hardware transfer keeps the transport busy. */
    bool (*idle)(void *context);
    /* Discard queued packets. An already active packet may complete; its
     * transport-owned storage must remain valid until hardware releases it. */
    void (*cancel)(void *context);
} analyzer_transport_t;

typedef struct {
    sump_parser_t parser;
    /* Wire-format options are validated here; the controller sees only
     * capture settings expressed in physical units and sample counts. */
    uint32_t sump_flags;
    capture_config_t config;
    capture_controller_t capture;
    analyzer_transport_t transport;
    /* One pending reply provides receive backpressure when the transmit queue
     * is full. All accepted packets live in transport-owned storage. */
    uint8_t reply[ANALYZER_TRANSFER_BYTES];
    size_t reply_length;
    uint8_t tx[ANALYZER_TRANSFER_BYTES];
    /* Counts accepted sample bytes; the last packet may still be in flight. */
    uint32_t samples_sent;
} analyzer_app_t;

void analyzer_app_init(analyzer_app_t *app, capture_driver_t driver,
                       analyzer_transport_t transport, uint32_t timer_clock_hz);
/* Cancel capture, parser state, pending replies and queued transport packets.
 * A packet already owned by the hardware may finish after this call. */
void analyzer_app_reset(analyzer_app_t *app);
/* Returns false without consuming the byte when a reply must be drained first.
 * All app APIs belong to one caller/task; interrupt adapters must buffer input. */
bool analyzer_app_receive(analyzer_app_t *app, uint8_t byte);
void analyzer_app_poll(analyzer_app_t *app);

#endif
