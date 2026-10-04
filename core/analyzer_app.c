#include "analyzer_app.h"
#include <string.h>

void analyzer_app_init(analyzer_app_t *app, capture_driver_t driver,
                       analyzer_transport_t transport, uint32_t timer_clock_hz)
{
    memset(app, 0, sizeof(*app));
    app->transport = transport;
    capture_controller_init(&app->capture, driver, timer_clock_hz);
    capture_config_default(&app->config);
}

void analyzer_app_reset(analyzer_app_t *app)
{
    capture_dispatch(&app->capture, CAPTURE_EVENT_RESET, NULL);
    sump_parser_reset(&app->parser);
    capture_config_default(&app->config);
    app->reply_length = 0;
    app->samples_sent = 0;
    app->tx_contains_samples = false;
    /* tx may still be borrowed by USB; do not change it or tx_in_flight here. */
}

bool analyzer_app_receive(analyzer_app_t *app, uint8_t byte)
{
    sump_command_t command;
    if (app->reply_length) return false;
    if (!sump_parser_push(&app->parser, byte, &command)) return true;
    switch (command.opcode) {
    case SUMP_RESET: analyzer_app_reset(app); break;
    case SUMP_RUN:
        if (capture_dispatch(&app->capture, CAPTURE_EVENT_RUN, &app->config))
            app->samples_sent = 0;
        break;
    case SUMP_ID: {
        size_t length;
        const uint8_t *identity = sump_identity(&length);
        memcpy(app->reply, identity, length);
        app->reply_length = length;
        break;
    }
    case SUMP_META:
        app->reply_length = sump_metadata(app->reply, sizeof(app->reply),
                                          CAPTURE_BUFFER_SAMPLES, CAPTURE_MAX_SAMPLE_RATE_HZ);
        break;
    case SUMP_DIVIDER: app->config.divider = command.value & 0xffffffu; break;
    case SUMP_COUNTS:
        app->config.samples = ((command.value & 0xffffu) + 1u) * 4u;
        app->config.post_trigger = ((command.value >> 16) + 1u) * 4u;
        break;
    case SUMP_FLAGS: app->config.flags = (uint16_t)command.value; break;
    case SUMP_TRIGGER_MASK: app->config.trigger_mask = (uint8_t)command.value; break;
    case SUMP_TRIGGER_VALUE: app->config.trigger_value = (uint8_t)command.value; break;
    default: break; /* Unsupported opcodes are framed and ignored. */
    }
    return true;
}

void analyzer_app_poll(analyzer_app_t *app)
{
    capture_poll(&app->capture);
    if (!app->transport.ready(app->transport.context)) return;
    if (app->tx_in_flight) {
        app->tx_in_flight = false;
        if (app->tx_contains_samples && app->samples_sent == app->capture.plan.config.samples)
            capture_dispatch(&app->capture, CAPTURE_EVENT_SENT, NULL);
        app->tx_contains_samples = false;
    }
    size_t length;
    bool samples = app->capture.state == CAPTURE_SENDING;
    if (samples) {
        length = capture_read_reverse(&app->capture, app->samples_sent, app->tx, sizeof(app->tx));
        if (!length) {
            capture_dispatch(&app->capture, CAPTURE_EVENT_ERROR, NULL);
            return;
        }
    } else if (app->reply_length) {
        length = app->reply_length;
        memcpy(app->tx, app->reply, length);
    } else return;

    if (app->transport.write(app->transport.context, app->tx, length)) {
        app->tx_in_flight = true;
        app->tx_contains_samples = samples;
        if (samples) app->samples_sent += (uint32_t)length;
        else app->reply_length = 0;
    }
}
