/** @file analyzer_app.c @brief Bounded application service for one SUMP session. */
#include "analyzer_app.h"

#include <string.h>

#define ABSENT_CHANNEL_GROUPS (SUMP_FLAG_DISABLE_GROUP_1 | \
    SUMP_FLAG_DISABLE_GROUP_2 | SUMP_FLAG_DISABLE_GROUP_3)
#define SUPPORTED_SUMP_FLAGS (ABSENT_CHANNEL_GROUPS | SUMP_FLAG_FILTER_HINT)
#define SUMP_COUNT_FIELD_MASK UINT32_C(0x0000ffff)
#define SUMP_POST_TRIGGER_SHIFT 16U

_Static_assert(SUMP_METADATA_MAX_BYTES <= ANALYZER_TRANSFER_BYTES,
               "Application reply storage must hold SUMP metadata");

static void reset_session(analyzer_app_t *app)
{
    sump_parser_reset(&app->parser);
    capture_config_default(&app->config);
    app->sump_flags = ABSENT_CHANNEL_GROUPS;
    app->reply_length = 0U;
    app->samples_sent = 0U;
}

void analyzer_app_init(analyzer_app_t *app, capture_driver_t driver,
                       analyzer_transport_t transport, uint32_t timer_clock_hz)
{
    *app = (analyzer_app_t){.transport = transport};
    capture_controller_init(&app->capture, driver, timer_clock_hz);
    reset_session(app);
}

void analyzer_app_reset(analyzer_app_t *app)
{
    (void)capture_dispatch(&app->capture, CAPTURE_EVENT_RESET, NULL);
    app->transport.cancel(app->transport.context);
    reset_session(app);
}

static void finish_delivered_capture(analyzer_app_t *app)
{
    if ((app->capture.state == CAPTURE_SENDING) &&
        (app->samples_sent == app->capture.plan.config.samples) &&
        app->transport.idle(app->transport.context)) {
        (void)capture_dispatch(&app->capture, CAPTURE_EVENT_SENT, NULL);
    }
}

static void request_capture(analyzer_app_t *app)
{
    const capture_config_t *requested_config = NULL;

    /* The host can issue RUN immediately after reading the final sample, before
     * this task has observed the corresponding USB completion notification. */
    finish_delivered_capture(app);

    /* libsigrok requests its FPGA input filter below 100 MHz. Accept that bit
     * as a hint, since GPIO sampling cannot implement the FPGA filter. Reject
     * other unsupported modes, including disabling the only available group. */
    if ((app->sump_flags & ~SUPPORTED_SUMP_FLAGS) == 0U) {
        requested_config = &app->config;
    }

    /* The controller checks the state before the configuration. A repeated RUN
     * cannot stop an active capture, even when pending settings are invalid. */
    if (capture_dispatch(&app->capture, CAPTURE_EVENT_RUN, requested_config)) {
        app->samples_sent = 0U;
    }
}

static void prepare_identity_reply(analyzer_app_t *app)
{
    size_t length;
    const uint8_t *bytes = sump_identity(&length);

    memcpy(app->reply, bytes, length);
    app->reply_length = length;
}

static void apply_command(analyzer_app_t *app, const sump_command_t *command)
{
    switch (command->opcode) {
    case SUMP_RESET:
        analyzer_app_reset(app);
        break;
    case SUMP_RUN:
        request_capture(app);
        break;
    case SUMP_ID:
        prepare_identity_reply(app);
        break;
    case SUMP_META:
        app->reply_length = sump_metadata(app->reply, sizeof(app->reply),
            CAPTURE_BUFFER_SAMPLES, CAPTURE_MAX_SAMPLE_RATE_HZ);
        break;
    case SUMP_DIVIDER:
        app->config.sample_period_ns =
            ((command->value & SUMP_DIVIDER_MASK) + 1U) * SUMP_CLOCK_PERIOD_NS;
        break;
    case SUMP_COUNTS:
        app->config.samples =
            ((command->value & SUMP_COUNT_FIELD_MASK) + 1U) * SUMP_COUNT_UNIT_SAMPLES;
        app->config.post_trigger =
            ((command->value >> SUMP_POST_TRIGGER_SHIFT) + 1U) * SUMP_COUNT_UNIT_SAMPLES;
        break;
    case SUMP_FLAGS:
        app->sump_flags = command->value;
        break;
    case SUMP_TRIGGER_MASK:
        app->config.trigger_mask = command->value;
        break;
    case SUMP_TRIGGER_VALUE:
        app->config.trigger_value = (uint8_t)(command->value & UINT8_MAX);
        break;
    default:
        /* Unknown commands still consume their complete frame in the parser. */
        break;
    }
}

bool analyzer_app_receive(analyzer_app_t *app, uint8_t byte)
{
    sump_command_t command;

    /* A pending reply exists only at a command boundary. RESET must remain
     * available when a host stops reading; it cancels that reply and queued
     * capture data. Other bytes stay pending until their response has room. */
    if ((app->reply_length != 0U) && (byte != SUMP_RESET)) {
        return false;
    }
    if (sump_parser_push(&app->parser, byte, &command)) {
        apply_command(app, &command);
    }
    return true;
}

static bool queue_capture_packet(analyzer_app_t *app)
{
    size_t length;

    if (app->samples_sent == app->capture.plan.config.samples) {
        return false;
    }

    length = capture_read_reverse(&app->capture, app->samples_sent,
                                   app->tx, sizeof(app->tx));
    if (length == 0U) {
        (void)capture_dispatch(&app->capture, CAPTURE_EVENT_ERROR, NULL);
        return false;
    }
    if (!app->transport.write(app->transport.context, app->tx, length)) {
        return false;
    }

    /* length is bounded by sizeof(tx), and the total by the validated plan. */
    app->samples_sent += (uint32_t)length;
    return true;
}

static bool queue_control_reply(analyzer_app_t *app)
{
    bool accepted = false;

    if (app->reply_length != 0U) {
        accepted = app->transport.write(app->transport.context,
                                         app->reply, app->reply_length);
        if (accepted) {
            app->reply_length = 0U;
        }
    }
    return accepted;
}

bool analyzer_app_poll(analyzer_app_t *app)
{
    const capture_state_t previous_state = app->capture.state;
    bool packet_accepted;

    capture_poll(&app->capture);
    finish_delivered_capture(app);

    /* A capture owns the response stream until every sample is delivered. */
    if (app->capture.state == CAPTURE_SENDING) {
        packet_accepted = queue_capture_packet(app);
    } else {
        packet_accepted = queue_control_reply(app);
    }

    return packet_accepted || (previous_state != app->capture.state);
}
