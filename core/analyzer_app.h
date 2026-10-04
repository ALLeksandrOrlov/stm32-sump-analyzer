#ifndef ANALYZER_APP_H
#define ANALYZER_APP_H

#include "capture.h"
#include "sump_protocol.h"

typedef struct {
    void *context;
    /* ready is false until the last accepted transfer is fully complete.
     * write borrows the buffer until ready; false means retry without progress. */
    bool (*ready)(void *context);
    bool (*write)(void *context, const uint8_t *bytes, size_t length);
} analyzer_transport_t;

typedef struct {
    sump_parser_t parser;
    capture_config_t config;
    capture_controller_t capture;
    analyzer_transport_t transport;
    uint8_t reply[64];
    size_t reply_length;
    uint8_t tx[64];
    uint32_t samples_sent;
    bool tx_in_flight;
    bool tx_contains_samples;
} analyzer_app_t;

void analyzer_app_init(analyzer_app_t *app, capture_driver_t driver,
                       analyzer_transport_t transport, uint32_t timer_clock_hz);
void analyzer_app_reset(analyzer_app_t *app);
/* Returns false without consuming the byte when a reply must be drained first.
 * All app APIs are foreground-only; interrupt adapters must buffer input. */
bool analyzer_app_receive(analyzer_app_t *app, uint8_t byte);
void analyzer_app_poll(analyzer_app_t *app);

#endif
