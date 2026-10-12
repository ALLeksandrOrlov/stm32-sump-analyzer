#include "analyzer_app.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint8_t data[CAPTURE_BUFFER_SAMPLES];
    uint32_t next;
    unsigned starts, stops, polls, buffer_reads;
    bool start_ok, busy, reject, missing_buffer;
    capture_hw_event_t event;
    capture_plan_t plan;
    const uint8_t *borrowed;
    size_t borrowed_length;
    uint8_t wire[CAPTURE_BUFFER_SAMPLES + 256];
    size_t wire_length;
} test_fixture_t;
static test_fixture_t fixture;
static analyzer_app_t app;

static bool driver_start(void *context, const capture_plan_t *plan)
{
    test_fixture_t *state = context;
    ++state->starts;
    state->plan = *plan;
    return state->start_ok;
}

static void driver_stop(void *context)
{
    test_fixture_t *state = context;
    ++state->stops;
    state->event = CAPTURE_HW_NONE;
}

static capture_hw_event_t driver_poll(void *context)
{
    test_fixture_t *state = context;
    ++state->polls;
    capture_hw_event_t event = state->event;
    state->event = CAPTURE_HW_NONE;
    return event;
}

static const uint8_t *driver_buffer(void *context, uint32_t *next)
{
    test_fixture_t *state = context;
    ++state->buffer_reads;
    *next = state->next;
    return state->missing_buffer ? NULL : state->data;
}

static bool transport_ready(void *context)
{
    return !((test_fixture_t *)context)->busy;
}

static bool transport_write(void *context, const uint8_t *data, size_t length)
{
    test_fixture_t *state = context;
    assert(!state->busy);
    if (state->reject) {
        return false;
    }
    state->borrowed = data;
    state->borrowed_length = length;
    state->busy = true;
    return true;
}
/* Copy on completion to catch writes to buffers still owned by transport. */
static void complete_transfer(void)
{
    assert(fixture.busy);
    assert(fixture.wire_length + fixture.borrowed_length <= sizeof(fixture.wire));
    memcpy(fixture.wire + fixture.wire_length, fixture.borrowed, fixture.borrowed_length);
    fixture.wire_length += fixture.borrowed_length;
    fixture.busy = false;
}

static void reset_fixture(void)
{
    memset(&fixture, 0, sizeof(fixture));
    fixture.start_ok = true;
    capture_driver_t driver = {
        .context = &fixture,
        .start = driver_start,
        .stop = driver_stop,
        .poll = driver_poll,
        .buffer = driver_buffer
    };
    analyzer_transport_t transport = {
        .context = &fixture,
        .ready = transport_ready,
        .write = transport_write
    };
    analyzer_app_init(&app, driver, transport, 168000000);
}

static void send_command(uint8_t opcode, uint32_t value)
{
    assert(analyzer_app_receive(&app, opcode));
    if (opcode & 0x80) {
        for (unsigned i = 0; i < 4; ++i) {
            assert(analyzer_app_receive(&app, (uint8_t)(value >> (8 * i))));
        }
    }
}

static void drain_transfers(void)
{
    for (unsigned i = 0; i < 1000; ++i) {
        analyzer_app_poll(&app);
        if (fixture.busy) {
            complete_transfer();
        } else if (!app.reply_length && app.capture.state != CAPTURE_SENDING) {
            return;
        }
    }
    assert(!"drain timed out");
}

static void test_parser_preserves_framing_across_packet_boundaries(void)
{
    sump_parser_t parser;
    sump_command_t out;
    const uint8_t stream[] = {
        0x80, 0x78, 0x56, 0x34, 0x12,
        0x02,
        0xee, 0x00, 0x01, 0x02, 0x04,
        0x04
    };
    const sump_command_t expected[] = {
        {SUMP_DIVIDER, 0x12345678},
        {SUMP_ID, 0},
        {0xee, 0x04020100},
        {SUMP_META, 0}
    };
    for (size_t split = 0; split <= sizeof(stream); ++split) {
        sump_parser_reset(&parser);
        unsigned count = 0;
        /* Every possible packet boundary, including empty packets. */
        for (unsigned packet = 0; packet < 2; ++packet) {
            size_t first = packet ? split : 0;
            size_t last = packet ? sizeof(stream) : split;
            for (size_t i = first; i < last; ++i) {
                if (!sump_parser_push(&parser, stream[i], &out)) {
                    continue;
                }
                assert(count < sizeof(expected) / sizeof(expected[0]));
                assert(out.opcode == expected[count].opcode);
                assert(out.value == expected[count].value);
                ++count;
            }
        }
        assert(count == 4);
    }
    for (unsigned opcode = 0x80; opcode <= 0xff; ++opcode) {
        sump_parser_reset(&parser);
        assert(!sump_parser_push(&parser, (uint8_t)opcode, &out));
        for (unsigned i = 0; i < 3; ++i) {
            assert(!sump_parser_push(&parser, 0, &out));
        }
        assert(sump_parser_push(&parser, 0, &out));
        assert(sump_parser_push(&parser, SUMP_ID, &out) && out.opcode == SUMP_ID);
    }
    sump_parser_reset(&parser);
    assert(!sump_parser_push(&parser, SUMP_COUNTS, &out));
    sump_parser_reset(&parser);
    assert(sump_parser_push(&parser, SUMP_ID, &out));
}

static void test_metadata_layout_and_capacity(void)
{
    uint8_t data[80];
    memset(data, 0xa5, sizeof(data));
    assert(sump_metadata(data, 1, CAPTURE_BUFFER_SAMPLES, CAPTURE_MAX_SAMPLE_RATE_HZ) == 0);
    assert(data[0] == 0xa5);
    size_t length = sump_metadata(data, sizeof(data), CAPTURE_BUFFER_SAMPLES, CAPTURE_MAX_SAMPLE_RATE_HZ);
    assert(length < sizeof(data) && data[length] == 0xa5);
    /* Metadata must fit the application's single control-reply packet. */
    assert(length <= sizeof(app.reply));
    assert(data[0] == 0x01);
    assert(strcmp((const char *)data + 1, "STM32 SUMP Analyzer") == 0);
    size_t at = 0;
    unsigned strings = 0;
    while (data[at] >= 1 && data[at] <= 3) {
        ++strings;
        ++at;
        while (data[at++]) {
            assert(at < length);
        }
    }
    assert(strings == 2);
    const uint8_t tail[] = {0x23, 0x01, 0x31, 0x2d, 0x00,
                           0x21, 0, 0, 0x70, 0, 0x40, 8, 0x41, 2, 0};
    assert(length - at == sizeof(tail));
    assert(memcmp(data + at, tail, sizeof(tail)) == 0);

    uint8_t exact[80];
    memset(exact, 0x5a, sizeof(exact));
    assert(sump_metadata(exact, length - 1, CAPTURE_BUFFER_SAMPLES, CAPTURE_MAX_SAMPLE_RATE_HZ) == 0);
    for (size_t i = 0; i < sizeof(exact); ++i) {
        assert(exact[i] == 0x5a);
    }
    assert(sump_metadata(exact, length, CAPTURE_BUFFER_SAMPLES, CAPTURE_MAX_SAMPLE_RATE_HZ) == length);
    assert(memcmp(exact, data, length) == 0);
    assert(exact[length] == 0x5a);
}

static void test_capture_limits_and_default_timing(void)
{
    capture_config_t config;
    capture_plan_t plan;
    capture_config_default(&config);
    assert(capture_make_plan(&config, 168000000, &plan));
    assert(plan.prescaler == 0 && plan.reload == 167);
    config.sample_period_ns = 10;
    assert(capture_make_plan(&config, 168000000, &plan));
    assert(plan.reload == 8); /* 18.667 MHz: never exceed advertised 20 MHz */
    config.sample_period_ns = 167772160;
    assert(capture_make_plan(&config, 168000000, &plan));
    uint64_t ticks = ((uint64_t)plan.prescaler + 1) * (plan.reload + 1u);
    assert(ticks >= ((uint64_t)168000000 * 0x1000000u + 99999999) / 100000000);
    config.samples = CAPTURE_BUFFER_SAMPLES;
    assert(capture_make_plan(&config, 168000000, &plan));
    config.samples++;
    assert(!capture_make_plan(&config, 168000000, &plan));
    config.samples = 0;
    assert(!capture_make_plan(&config, 168000000, &plan));
    capture_config_default(&config);
    config.post_trigger = config.samples + 1;
    assert(!capture_make_plan(&config, 168000000, &plan));
    config.post_trigger = 0;
    assert(!capture_make_plan(&config, 168000000, &plan));
}

static void test_command_counts_and_capture_transitions(void)
{
    reset_fixture();
    send_command(SUMP_COUNTS, 0xffffffff);
    assert(app.config.samples == 262144 && app.config.post_trigger == 262144);
    send_command(SUMP_RUN, 0);
    assert(fixture.starts == 0 && app.capture.state == CAPTURE_ERROR);
    send_command(SUMP_RESET, 0);
    send_command(SUMP_COUNTS, 0);
    assert(app.config.samples == 4 && app.config.post_trigger == 4);
    send_command(SUMP_TRIGGER_MASK, 2);
    send_command(SUMP_TRIGGER_VALUE, 0x102);
    assert(app.config.trigger_mask == 2 && app.config.trigger_value == 2);
    send_command(SUMP_RUN, 0);
    assert(app.capture.state == CAPTURE_ARMED);
    send_command(SUMP_RUN, 0);
    assert(fixture.starts == 1);
    send_command(SUMP_COUNTS, 0x00010001);
    assert(fixture.plan.config.samples == 4); /* configuration snapshot */
    fixture.event = CAPTURE_HW_TRIGGERED;
    analyzer_app_poll(&app);
    assert(app.capture.state == CAPTURE_RUNNING);
    send_command(SUMP_RESET, 0);
    assert(app.capture.state == CAPTURE_IDLE && app.config.trigger_mask == 0);
    fixture.event = CAPTURE_HW_COMPLETE;
    analyzer_app_poll(&app);
    assert(app.capture.state == CAPTURE_IDLE && !fixture.busy);
    fixture.start_ok = false;
    send_command(SUMP_RUN, 0);
    assert(app.capture.state == CAPTURE_ERROR);
}

static void test_control_replies_retry_and_preserve_borrowed_buffers(void)
{
    reset_fixture();
    send_command(SUMP_ID, 0);
    assert(!analyzer_app_receive(&app, SUMP_META));
    fixture.reject = true;
    analyzer_app_poll(&app);
    assert(app.reply_length == 4 && !app.tx_in_flight);
    fixture.reject = false;
    analyzer_app_poll(&app);
    send_command(SUMP_META, 0);
    analyzer_app_poll(&app);
    complete_transfer();
    assert(fixture.wire_length == 4 && memcmp(fixture.wire, "1ALS", 4) == 0);
    drain_transfers();
    uint8_t expected[64];
    size_t length = sump_metadata(expected, sizeof(expected), CAPTURE_BUFFER_SAMPLES, CAPTURE_MAX_SAMPLE_RATE_HZ);
    assert(fixture.wire_length == 4 + length);
    assert(memcmp(fixture.wire + 4, expected, length) == 0);
}

static void test_captured_samples_are_sent_newest_first(void)
{
    /* Includes DMA wrap, exact end, zero tail, partial packets and maximum RAM. */
    const uint32_t sizes[] = {4, 64, 68, CAPTURE_BUFFER_SAMPLES};
    for (size_t s = 0; s < sizeof(sizes)/sizeof(sizes[0]); ++s) {
        uint32_t count = sizes[s];
        const uint32_t tails[] = {0, 1, count - 1, count};
        for (size_t t = 0; t < 4; ++t) {
            reset_fixture();
            app.config.samples = count;
            app.config.post_trigger = count;
            for (uint32_t i = 0; i < count; ++i) {
                fixture.data[i] = (uint8_t)(i * 7);
            }
            fixture.next = tails[t];
            send_command(SUMP_RUN, 0);
            fixture.event = CAPTURE_HW_COMPLETE;
            fixture.reject = true;
            analyzer_app_poll(&app);
            assert(app.samples_sent == 0);
            fixture.reject = false;
            drain_transfers();
            assert(fixture.wire_length == count && app.capture.state == CAPTURE_IDLE);
            for (uint32_t i = 0; i < count; ++i) {
                assert(fixture.wire[i] == fixture.data[(tails[t] + count - 1 - i) % count]);
            }
        }
    }
}

static void test_reset_while_sending(void)
{
    reset_fixture();
    app.config.samples = 128;
    app.config.post_trigger = 128;
    memset(fixture.data, 0x5a, 128);
    send_command(SUMP_RUN, 0);
    fixture.event = CAPTURE_HW_COMPLETE;
    analyzer_app_poll(&app);
    assert(fixture.busy && app.capture.state == CAPTURE_SENDING);
    send_command(SUMP_RESET, 0);
    send_command(SUMP_ID, 0);
    analyzer_app_poll(&app);
    complete_transfer();
    for (unsigned i = 0; i < 64; ++i) {
        assert(fixture.wire[i] == 0x5a);
    }
    drain_transfers();
    assert(fixture.wire_length == 68 && memcmp(fixture.wire + 64, "1ALS", 4) == 0);
    send_command(SUMP_RUN, 0);
    fixture.event = CAPTURE_HW_ERROR;
    analyzer_app_poll(&app);
    assert(app.capture.state == CAPTURE_ERROR);
}

static void test_capture_events_obey_state_guards(void)
{
    reset_fixture();
    assert(!capture_dispatch(&app.capture, CAPTURE_EVENT_TRIGGER, NULL));
    assert(!capture_dispatch(&app.capture, CAPTURE_EVENT_COMPLETE, NULL));
    assert(!capture_dispatch(&app.capture, CAPTURE_EVENT_SENT, NULL));
    app.config.trigger_mask = 1;
    send_command(SUMP_RUN, 0);
    fixture.event = CAPTURE_HW_COMPLETE; /* trigger and completion between polls */
    analyzer_app_poll(&app);
    assert(app.capture.state == CAPTURE_SENDING);
    assert(!capture_dispatch(&app.capture, CAPTURE_EVENT_RUN, &app.config));
}

static void test_short_commands_and_incomplete_arguments(void)
{
    sump_parser_t parser;
    sump_command_t result;
    sump_parser_reset(&parser);
    for (unsigned opcode = 0; opcode < 0x80; ++opcode) {
        assert(sump_parser_push(&parser, (uint8_t)opcode, &result));
        assert(result.opcode == opcode && result.value == 0);
    }

    result = (sump_command_t){SUMP_ID, 0xabcdef01};
    assert(!sump_parser_push(&parser, SUMP_COUNTS, &result));
    for (unsigned byte = 0; byte < 3; ++byte) {
        assert(!sump_parser_push(&parser, SUMP_RESET, &result));
        assert(result.opcode == SUMP_ID && result.value == 0xabcdef01);
    }
    assert(sump_parser_push(&parser, 0x80, &result));
    assert(result.opcode == SUMP_COUNTS && result.value == 0x80000000);
}

static void assert_plan_rejected(const capture_config_t *config, uint32_t timer_clock)
{
    capture_plan_t plan;
    memset(&plan, 0xa5, sizeof(plan));
    uint8_t before[sizeof(plan)];
    memcpy(before, &plan, sizeof(plan));
    assert(!capture_make_plan(config, timer_clock, &plan));
    assert(memcmp(&plan, before, sizeof(plan)) == 0);
}

static void test_plan_validation_boundaries(void)
{
    capture_config_t config;
    capture_plan_t plan;
    capture_config_default(&config);
    assert_plan_rejected(&config, 0);
    config.sample_period_ns = 0;
    assert_plan_rejected(&config, 168000000);

    capture_config_default(&config);
    config.samples = 1;
    config.post_trigger = 1;
    assert(capture_make_plan(&config, 168000000, &plan));
    for (unsigned pin = 0; pin < 8; ++pin) {
        config.trigger_mask = (uint8_t)(1u << pin);
        assert(capture_make_plan(&config, 168000000, &plan));
        config.trigger_value = config.trigger_mask;
        assert(capture_make_plan(&config, 168000000, &plan));
    }
    const uint8_t multiple_channels[] = {0x03, 0x81, 0xff};
    for (size_t i = 0; i < sizeof(multiple_channels); ++i) {
        config.trigger_mask = multiple_channels[i];
        assert_plan_rejected(&config, 168000000);
    }

    const uint32_t absent_channels[] = {0x100, 0x102, 0x80000000};
    for (size_t i = 0; i < sizeof(absent_channels) / sizeof(absent_channels[0]); ++i) {
        config.trigger_mask = absent_channels[i];
        assert_plan_rejected(&config, 168000000);
    }

}

static void test_timer_rounding_never_exceeds_requested_rate(void)
{
    /* Exercise register rollover and 64-bit arithmetic without depending on
     * which valid prescaler/reload pair the planner chooses. */
    const uint32_t clocks[] = {1, 1000000, 168000000, UINT32_MAX};
    const uint32_t periods[] = {1, 10, 50, 1000, 655350, 655360, 167772160};
    for (size_t clock = 0; clock < sizeof(clocks) / sizeof(clocks[0]); ++clock) {
        for (size_t request = 0; request < sizeof(periods) / sizeof(periods[0]); ++request) {
            capture_config_t config;
            capture_plan_t plan;
            capture_config_default(&config);
            config.sample_period_ns = periods[request];
            assert(capture_make_plan(&config, clocks[clock], &plan));
            uint64_t period = ((uint64_t)plan.prescaler + 1u) * (plan.reload + 1u);
            assert(period * 1000000000 >= (uint64_t)clocks[clock] * periods[request]);
            assert(period * CAPTURE_MAX_SAMPLE_RATE_HZ >= clocks[clock]);
        }
    }

    capture_config_t config;
    capture_plan_t plan;
    capture_config_default(&config);
    config.sample_period_ns = UINT32_MAX;
    assert(capture_make_plan(&config, 168000000, &plan));
    assert_plan_rejected(&config, UINT32_MAX);
    config.sample_period_ns = 2000000000;
    assert(capture_make_plan(&config, 2147483648u, &plan));
    assert(plan.prescaler == UINT16_MAX && plan.reload == UINT16_MAX);
    ++config.sample_period_ns;
    assert_plan_rejected(&config, 2147483648u);
}

static void test_configuration_commands_and_unknown_opcodes(void)
{
    reset_fixture();
    /* Payload bytes that look like short commands remain arguments. */
    send_command(0xee, 0x04020100);
    send_command(0x7f, 0);
    assert(app.capture.state == CAPTURE_IDLE);
    assert(app.reply_length == 0 && fixture.starts == 0);

    send_command(SUMP_DIVIDER, 0xff123456);
    assert(app.config.sample_period_ns == 0x123457 * 10);
    send_command(SUMP_COUNTS, 0x00020004);
    assert(app.config.samples == 20 && app.config.post_trigger == 12);
    send_command(SUMP_FLAGS, 0x3a);
    assert(app.sump_flags == 0x3a);
    send_command(SUMP_TRIGGER_MASK, 3);
    send_command(SUMP_RUN, 0);
    assert(app.capture.state == CAPTURE_ERROR && fixture.starts == 0);

    send_command(SUMP_RESET, 0);
    send_command(SUMP_COUNTS, 0x00010000);
    send_command(SUMP_RUN, 0);
    assert(app.capture.state == CAPTURE_ERROR && fixture.starts == 0);
}

static void test_protocol_flags_are_validated_before_starting_hardware(void)
{
    for (unsigned bit = 0; bit < 32; ++bit) {
        reset_fixture();
        send_command(SUMP_FLAGS, 1u << bit);
        send_command(SUMP_RUN, 0);
        bool supported = bit == 1 || bit == 3 || bit == 4 || bit == 5;
        if (supported) {
            assert(app.capture.state == CAPTURE_RUNNING && fixture.starts == 1);
        } else {
            assert(app.capture.state == CAPTURE_ERROR && fixture.starts == 0);
        }
    }

    reset_fixture();
    send_command(SUMP_FLAGS, 0x3a);
    send_command(SUMP_RUN, 0);
    assert(app.capture.state == CAPTURE_RUNNING);
    send_command(SUMP_FLAGS, UINT32_MAX);
    send_command(SUMP_RUN, 0);
    assert(app.capture.state == CAPTURE_RUNNING && fixture.starts == 1);
    assert(fixture.stops == 0);
    send_command(SUMP_RESET, 0);
    assert(app.sump_flags == 0x38);
    send_command(SUMP_RUN, 0);
    assert(app.capture.state == CAPTURE_RUNNING && fixture.starts == 2);
}

static void test_trigger_masks_cannot_select_absent_channels(void)
{
    const uint32_t masks[] = {0x100, 0x102, 0x80000000};
    for (size_t i = 0; i < sizeof(masks) / sizeof(masks[0]); ++i) {
        reset_fixture();
        send_command(SUMP_TRIGGER_MASK, masks[i]);
        assert(app.config.trigger_mask == masks[i]);
        send_command(SUMP_RUN, 0);
        assert(app.capture.state == CAPTURE_ERROR && fixture.starts == 0);
    }
}

static void test_divider_decoding_uses_physical_time_units(void)
{
    reset_fixture();
    send_command(SUMP_DIVIDER, 0);
    assert(app.config.sample_period_ns == 10);
    send_command(SUMP_DIVIDER, 0xffffff);
    assert(app.config.sample_period_ns == 167772160);
    send_command(SUMP_DIVIDER, 0xff000063);
    assert(app.config.sample_period_ns == 1000);
}

static void test_failed_start_stops_hardware_and_requires_reset(void)
{
    reset_fixture();
    fixture.start_ok = false;
    send_command(SUMP_RUN, 0);
    assert(fixture.starts == 1 && fixture.stops == 1);
    assert(app.capture.state == CAPTURE_ERROR);
    fixture.start_ok = true;
    send_command(SUMP_RUN, 0);
    assert(fixture.starts == 1);
    send_command(SUMP_RESET, 0);
    send_command(SUMP_RUN, 0);
    assert(fixture.starts == 2 && app.capture.state == CAPTURE_RUNNING);

    reset_fixture();
    assert(!capture_dispatch(&app.capture, CAPTURE_EVENT_RUN, NULL));
    assert(fixture.starts == 0 && fixture.stops == 1);
    assert(app.capture.state == CAPTURE_ERROR);
}

static void test_capture_snapshot_is_independent_of_new_configuration(void)
{
    reset_fixture();
    send_command(SUMP_COUNTS, 0x00010003);
    send_command(SUMP_TRIGGER_MASK, 1);
    send_command(SUMP_TRIGGER_VALUE, 1);
    send_command(SUMP_RUN, 0);
    send_command(SUMP_COUNTS, 0x00020005);
    send_command(SUMP_DIVIDER, 999);
    send_command(SUMP_FLAGS, 0x3a);
    send_command(SUMP_TRIGGER_MASK, 2);
    send_command(SUMP_TRIGGER_VALUE, 0);
    assert(fixture.plan.config.samples == 16 && fixture.plan.config.post_trigger == 8);
    assert(fixture.plan.config.sample_period_ns == 1000);
    assert(fixture.plan.config.trigger_mask == 1 && fixture.plan.config.trigger_value == 1);
    assert(app.capture.plan.config.samples == 16);
    assert(app.capture.plan.config.sample_period_ns == 1000);
}

static void test_capture_waits_for_final_transfer_before_reply_or_restart(void)
{
    reset_fixture();
    send_command(SUMP_COUNTS, 0x00010001); /* Eight samples fit one transfer. */
    memset(fixture.data, 0x5a, 8);
    send_command(SUMP_RUN, 0);
    fixture.event = CAPTURE_HW_COMPLETE;
    analyzer_app_poll(&app);
    assert(fixture.busy && app.samples_sent == 8);
    assert(app.capture.state == CAPTURE_SENDING);
    send_command(SUMP_RUN, 0);
    assert(fixture.starts == 1);
    send_command(SUMP_ID, 0);
    analyzer_app_poll(&app);
    assert(app.capture.state == CAPTURE_SENDING && app.reply_length == 4);
    assert(fixture.wire_length == 0);

    complete_transfer();
    analyzer_app_poll(&app);
    assert(app.capture.state == CAPTURE_IDLE && fixture.busy);
    complete_transfer();
    assert(fixture.wire_length == 12 && memcmp(fixture.wire + 8, "1ALS", 4) == 0);
}

static void test_control_reply_does_not_interrupt_sample_stream(void)
{
    reset_fixture();
    app.config.samples = 132;
    app.config.post_trigger = 132;
    memset(fixture.data, 0x5a, 132);
    send_command(SUMP_RUN, 0);
    fixture.event = CAPTURE_HW_COMPLETE;
    analyzer_app_poll(&app);
    send_command(SUMP_ID, 0);
    assert(!analyzer_app_receive(&app, SUMP_META));
    drain_transfers();
    assert(fixture.wire_length == 136);
    for (size_t sample = 0; sample < 132; ++sample) {
        assert(fixture.wire[sample] == 0x5a);
    }
    assert(memcmp(fixture.wire + 132, "1ALS", 4) == 0);
}

static void test_invalid_capture_buffer_stops_without_transmitting(void)
{
    for (unsigned missing = 0; missing < 2; ++missing) {
        reset_fixture();
        fixture.missing_buffer = missing != 0;
        fixture.next = app.config.samples + 1;
        send_command(SUMP_RUN, 0);
        fixture.event = CAPTURE_HW_COMPLETE;
        analyzer_app_poll(&app);
        assert(app.capture.state == CAPTURE_ERROR && fixture.stops == 1);
        assert(!fixture.busy && fixture.wire_length == 0);
    }
}

static void test_read_boundaries_preserve_output(void)
{
    reset_fixture();
    uint8_t output[] = {0xa5, 0xa5, 0xa5};
    assert(capture_read_reverse(&app.capture, 0, output, sizeof(output)) == 0);
    send_command(SUMP_COUNTS, 0);
    fixture.data[0] = 10;
    fixture.data[1] = 20;
    fixture.data[2] = 30;
    fixture.data[3] = 40;
    fixture.next = 2;
    send_command(SUMP_RUN, 0);
    assert(capture_dispatch(&app.capture, CAPTURE_EVENT_COMPLETE, NULL));
    assert(capture_read_reverse(&app.capture, 4, output, sizeof(output)) == 0);
    assert(capture_read_reverse(&app.capture, UINT32_MAX, output, sizeof(output)) == 0);
    assert(capture_read_reverse(&app.capture, 0, output, 0) == 0);
    assert(fixture.buffer_reads == 0);
    assert(output[0] == 0xa5 && output[1] == 0xa5 && output[2] == 0xa5);
    assert(capture_read_reverse(&app.capture, 3, output, sizeof(output)) == 1);
    assert(output[0] == 30 && output[1] == 0xa5 && output[2] == 0xa5);
}

static void test_reset_cancels_parser_and_queued_reply(void)
{
    reset_fixture();
    assert(analyzer_app_receive(&app, SUMP_COUNTS));
    assert(analyzer_app_receive(&app, 0xff));
    analyzer_app_reset(&app);
    send_command(SUMP_ID, 0);
    assert(app.reply_length == 4);
    analyzer_app_reset(&app);
    analyzer_app_poll(&app);
    assert(!fixture.busy && app.reply_length == 0);
    assert(app.config.samples == 1024 && app.config.post_trigger == 1024);
}

static void test_reset_allows_new_capture_with_previous_transfer_in_flight(void)
{
    reset_fixture();
    send_command(SUMP_COUNTS, 0);
    memset(fixture.data, 0x11, 4);
    send_command(SUMP_RUN, 0);
    fixture.event = CAPTURE_HW_COMPLETE;
    analyzer_app_poll(&app);
    send_command(SUMP_RESET, 0);
    send_command(SUMP_COUNTS, 0);
    memset(fixture.data, 0x22, 4);
    send_command(SUMP_RUN, 0);
    fixture.event = CAPTURE_HW_COMPLETE;
    analyzer_app_poll(&app);
    assert(app.capture.state == CAPTURE_SENDING && app.samples_sent == 0);
    complete_transfer();
    drain_transfers();
    const uint8_t expected[] = {0x11, 0x11, 0x11, 0x11, 0x22, 0x22, 0x22, 0x22};
    assert(fixture.wire_length == sizeof(expected));
    assert(memcmp(fixture.wire, expected, sizeof(expected)) == 0);
    assert(app.capture.state == CAPTURE_IDLE);
}

static void test_hardware_events_are_polled_only_during_capture(void)
{
    reset_fixture();
    analyzer_app_poll(&app);
    assert(fixture.polls == 0);
    send_command(SUMP_TRIGGER_MASK, 1);
    send_command(SUMP_RUN, 0);
    analyzer_app_poll(&app);
    assert(app.capture.state == CAPTURE_ARMED && fixture.polls == 1);
    fixture.event = CAPTURE_HW_ERROR;
    analyzer_app_poll(&app);
    assert(app.capture.state == CAPTURE_ERROR && fixture.stops == 1);
    analyzer_app_poll(&app);
    assert(fixture.polls == 2);
    send_command(SUMP_RESET, 0);
    send_command(SUMP_RUN, 0);
    fixture.event = CAPTURE_HW_TRIGGERED; /* Unsolicited trigger cannot re-arm. */
    analyzer_app_poll(&app);
    assert(app.capture.state == CAPTURE_RUNNING);
    assert(!capture_dispatch(&app.capture, (capture_event_t)99, NULL));
    assert(app.capture.state == CAPTURE_RUNNING);
}

int main(void)
{
    test_parser_preserves_framing_across_packet_boundaries();
    test_short_commands_and_incomplete_arguments();
    test_metadata_layout_and_capacity();
    test_capture_limits_and_default_timing();
    test_plan_validation_boundaries();
    test_timer_rounding_never_exceeds_requested_rate();
    test_command_counts_and_capture_transitions();
    test_configuration_commands_and_unknown_opcodes();
    test_protocol_flags_are_validated_before_starting_hardware();
    test_trigger_masks_cannot_select_absent_channels();
    test_divider_decoding_uses_physical_time_units();
    test_failed_start_stops_hardware_and_requires_reset();
    test_capture_snapshot_is_independent_of_new_configuration();
    test_control_replies_retry_and_preserve_borrowed_buffers();
    test_captured_samples_are_sent_newest_first();
    test_capture_waits_for_final_transfer_before_reply_or_restart();
    test_control_reply_does_not_interrupt_sample_stream();
    test_invalid_capture_buffer_stops_without_transmitting();
    test_read_boundaries_preserve_output();
    test_reset_while_sending();
    test_reset_cancels_parser_and_queued_reply();
    test_reset_allows_new_capture_with_previous_transfer_in_flight();
    test_capture_events_obey_state_guards();
    test_hardware_events_are_polled_only_during_capture();
    puts("ALL CORE TESTS PASSED");
    return 0;
}
