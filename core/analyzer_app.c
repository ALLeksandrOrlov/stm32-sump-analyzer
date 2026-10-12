#include "analyzer_app.h"

#include <string.h>

enum {
    UNUSED_CHANNEL_GROUPS = SUMP_FLAG_DISABLE_GROUP_1 |
        SUMP_FLAG_DISABLE_GROUP_2 | SUMP_FLAG_DISABLE_GROUP_3,
    SUPPORTED_FLAGS = UNUSED_CHANNEL_GROUPS | SUMP_FLAG_FILTER_HINT
};

void analyzer_app_init(analyzer_app_t *app, capture_driver_t driver,
                       analyzer_transport_t transport, uint32_t timer_clock_hz)
{
    memset(app, 0, sizeof(*app));
    app->transport = transport;
    capture_controller_init(&app->capture, driver, timer_clock_hz);
    capture_config_default(&app->config);
    app->sump_flags = UNUSED_CHANNEL_GROUPS;
}

void analyzer_app_reset(analyzer_app_t *app)
{
    capture_dispatch(&app->capture, CAPTURE_EVENT_RESET, NULL);
    sump_parser_reset(&app->parser);
    capture_config_default(&app->config);
    app->sump_flags = UNUSED_CHANNEL_GROUPS;
    app->reply_length = 0;
    app->samples_sent = 0;
    app->tx_contains_samples = false;
    /* The transport may still borrow tx. Its bytes and ownership stay valid. */
}

static void apply_command(analyzer_app_t *app, const sump_command_t *command)
{
    switch (command->opcode) {
    case SUMP_RESET:
        analyzer_app_reset(app);
        break;
    case SUMP_RUN: {
        /* libsigrok requests its FPGA filter below 100 MHz. Raw GPIO samples
         * cannot implement that filter, so this flag is accepted as a hint.
         * Disabling absent channel groups is safe; all other flags are errors.
         * NULL rejects invalid options through the controller's RUN guard,
         * which must ignore new requests while a capture is already active. */
        const capture_config_t *config =
            (app->sump_flags & (uint32_t)~SUPPORTED_FLAGS) == 0 ? &app->config : NULL;
        if (capture_dispatch(&app->capture, CAPTURE_EVENT_RUN, config)) {
            app->samples_sent = 0;
        }
        break;
    }
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
    case SUMP_DIVIDER:
        app->config.sample_period_ns =
            ((command->value & SUMP_DIVIDER_MASK) + 1u) * SUMP_CLOCK_PERIOD_NS;
        break;
    case SUMP_COUNTS:
        app->config.samples = ((command->value & 0xffffu) + 1u) * SUMP_COUNT_UNIT_SAMPLES;
        app->config.post_trigger = ((command->value >> 16) + 1u) * SUMP_COUNT_UNIT_SAMPLES;
        break;
    case SUMP_FLAGS:
        app->sump_flags = command->value;
        break;
    case SUMP_TRIGGER_MASK:
        app->config.trigger_mask = command->value;
        break;
    case SUMP_TRIGGER_VALUE:
        app->config.trigger_value = (uint8_t)command->value;
        break;
    default:
        /* Framing unknown commands keeps subsequent commands synchronized. */
        break;
    }
}

bool analyzer_app_receive(analyzer_app_t *app, uint8_t byte)
{
    if (app->reply_length != 0) {
        return false;
    }

    sump_command_t command;
    if (sump_parser_push(&app->parser, byte, &command)) {
        apply_command(app, &command);
    }
    return true;
}

static void finish_transfer(analyzer_app_t *app)
{
    if (!app->tx_in_flight) {
        return;
    }
    app->tx_in_flight = false;
    if (app->tx_contains_samples && app->samples_sent == app->capture.plan.config.samples) {
        capture_dispatch(&app->capture, CAPTURE_EVENT_SENT, NULL);
    }
    app->tx_contains_samples = false;
}

void analyzer_app_poll(analyzer_app_t *app)
{
    capture_poll(&app->capture);
    if (!app->transport.ready(app->transport.context)) {
        return;
    }
    finish_transfer(app);

    size_t length;
    bool samples = app->capture.state == CAPTURE_SENDING;
    /* A capture is one continuous response. Control replies wait until its
     * last sample packet has completed, including asynchronous transport I/O. */
    if (samples) {
        length = capture_read_reverse(&app->capture, app->samples_sent, app->tx, sizeof(app->tx));
        if (!length) {
            capture_dispatch(&app->capture, CAPTURE_EVENT_ERROR, NULL);
            return;
        }
    } else if (app->reply_length) {
        length = app->reply_length;
        memcpy(app->tx, app->reply, length);
    } else {
        return;
    }

    if (app->transport.write(app->transport.context, app->tx, length)) {
        app->tx_in_flight = true;
        app->tx_contains_samples = samples;
        if (samples) {
            app->samples_sent += (uint32_t)length;
        } else {
            app->reply_length = 0;
        }
    }
}
