#ifndef ANALYZER_APP_H
#define ANALYZER_APP_H

#include "capture.h"
#include "sump_protocol.h"

#define ANALYZER_TRANSFER_BYTES 64u

typedef struct {
    void *context;
    /* ready is false until the last accepted transfer is fully complete.
     * write borrows the buffer until ready; false means retry without progress. */
    bool (*ready)(void *context);
    bool (*write)(void *context, const uint8_t *bytes, size_t length);
} analyzer_transport_t;

typedef struct {
    sump_parser_t parser;
    /* Wire-format options are validated here; the controller sees only
     * capture settings expressed in physical units and sample counts. */
    uint32_t sump_flags;
    capture_config_t config;
    capture_controller_t capture;
    analyzer_transport_t transport;
    /* Pending control replies and the transport-owned packet are separate:
     * receive can prepare a reply while the previous transfer is in flight. */
    uint8_t reply[ANALYZER_TRANSFER_BYTES];
    size_t reply_length;
    uint8_t tx[ANALYZER_TRANSFER_BYTES];
    /* Counts accepted sample bytes; the last packet may still be in flight. */
    uint32_t samples_sent;
    bool tx_in_flight;
    bool tx_contains_samples;
} analyzer_app_t;

void analyzer_app_init(analyzer_app_t *app, capture_driver_t driver,
                       analyzer_transport_t transport, uint32_t timer_clock_hz);
/* Cancel capture, parser state and queued replies. A transport-owned packet
 * remains untouched until ready signals that ownership has been released. */
void analyzer_app_reset(analyzer_app_t *app);
/* Returns false without consuming the byte when a reply must be drained first.
 * All app APIs are foreground-only; interrupt adapters must buffer input. */
bool analyzer_app_receive(analyzer_app_t *app, uint8_t byte);
void analyzer_app_poll(analyzer_app_t *app);

#endif
