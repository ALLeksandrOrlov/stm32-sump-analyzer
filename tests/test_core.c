#include "analyzer_app.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint8_t data[CAPTURE_CAPACITY];
    uint32_t next;
    unsigned starts, stops;
    bool start_ok, busy, reject;
    capture_hw_event_t event;
    capture_plan_t plan;
    const uint8_t *borrowed;
    size_t borrowed_length;
    uint8_t wire[CAPTURE_CAPACITY + 256];
    size_t wire_length;
} fake_t;
static fake_t fake;
static analyzer_app_t app;

static bool start(void *ctx, const capture_plan_t *plan)
{
    fake_t *f = ctx;
    ++f->starts;
    f->plan = *plan;
    return f->start_ok;
}
static void stop(void *ctx) { ++((fake_t *)ctx)->stops; }
static capture_hw_event_t poll(void *ctx)
{
    fake_t *f = ctx;
    capture_hw_event_t event = f->event;
    f->event = CAPTURE_HW_NONE;
    return event;
}
static const uint8_t *buffer(void *ctx, uint32_t *next)
{
    fake_t *f = ctx;
    *next = f->next;
    return f->data;
}
static bool ready(void *ctx) { return !((fake_t *)ctx)->busy; }
static bool write_bytes(void *ctx, const uint8_t *data, size_t length)
{
    fake_t *f = ctx;
    assert(!f->busy);
    if (f->reject) return false;
    f->borrowed = data;
    f->borrowed_length = length;
    f->busy = true;
    return true;
}
/* Copy on completion, not submission: catches modifying USB-owned buffers. */
static void finish_tx(void)
{
    assert(fake.busy);
    assert(fake.wire_length + fake.borrowed_length <= sizeof(fake.wire));
    memcpy(fake.wire + fake.wire_length, fake.borrowed, fake.borrowed_length);
    fake.wire_length += fake.borrowed_length;
    fake.busy = false;
}
static void init(void)
{
    memset(&fake, 0, sizeof(fake));
    fake.start_ok = true;
    analyzer_app_init(&app, (capture_driver_t){&fake, start, stop, poll, buffer},
                      (analyzer_transport_t){&fake, ready, write_bytes}, 168000000);
}
static void command(uint8_t opcode, uint32_t value)
{
    assert(analyzer_app_receive(&app, opcode));
    if (opcode & 0x80) {
        for (unsigned i = 0; i < 4; ++i)
            assert(analyzer_app_receive(&app, (uint8_t)(value >> (8 * i))));
    }
}
static void drain(void)
{
    for (unsigned i = 0; i < 1000; ++i) {
        analyzer_app_poll(&app);
        if (fake.busy) finish_tx();
        else if (!app.reply_length && app.capture.state != CAPTURE_SENDING) return;
    }
    assert(!"drain timed out");
}
static void test_parser(void)
{
    sump_parser_t parser;
    sump_command_t out;
    const uint8_t stream[] = {0x80,0x78,0x56,0x34,0x12, 0x02, 0xee,0,1,2,4, 0x04};
    for (size_t split = 0; split <= sizeof(stream); ++split) {
        sump_parser_reset(&parser);
        unsigned count = 0;
        /* Every possible packet boundary, including empty packets. */
        for (unsigned packet = 0; packet < 2; ++packet) {
            size_t first = packet ? split : 0;
            size_t last = packet ? sizeof(stream) : split;
            for (size_t i = first; i < last; ++i) {
                if (!sump_parser_push(&parser, stream[i], &out)) continue;
                if (count == 0) assert(out.opcode == SUMP_DIVIDER && out.value == 0x12345678);
                if (count == 1) assert(out.opcode == SUMP_ID && out.value == 0);
                if (count == 2) assert(out.opcode == 0xee && out.value == 0x04020100);
                if (count == 3) assert(out.opcode == SUMP_META);
                ++count;
            }
        }
        assert(count == 4);
    }
    for (unsigned opcode = 0x80; opcode <= 0xff; ++opcode) {
        sump_parser_reset(&parser);
        assert(!sump_parser_push(&parser, (uint8_t)opcode, &out));
        for (unsigned i = 0; i < 3; ++i) assert(!sump_parser_push(&parser, 0, &out));
        assert(sump_parser_push(&parser, 0, &out));
        assert(sump_parser_push(&parser, SUMP_ID, &out) && out.opcode == SUMP_ID);
    }
    sump_parser_reset(&parser);
    assert(!sump_parser_push(&parser, SUMP_COUNTS, &out));
    sump_parser_reset(&parser);
    assert(sump_parser_push(&parser, SUMP_ID, &out));
}
static void test_metadata(void)
{
    uint8_t data[80];
    memset(data, 0xa5, sizeof(data));
    assert(sump_metadata(data, 1, CAPTURE_CAPACITY, CAPTURE_MAX_RATE) == 0);
    assert(data[0] == 0xa5);
    size_t length = sump_metadata(data, sizeof(data), CAPTURE_CAPACITY, CAPTURE_MAX_RATE);
    assert(length < sizeof(data) && data[length] == 0xa5);
    size_t at = 0;
    unsigned strings = 0;
    while (data[at] >= 1 && data[at] <= 3) {
        ++strings;
        ++at;
        while (data[at++]) assert(at < length);
    }
    assert(strings == 3);
    const uint8_t tail[] = {0x23, 0x01, 0x31, 0x2d, 0x00,
                           0x21, 0, 0, 0x70, 0, 0x40, 8, 0x41, 2, 0};
    assert(length - at == sizeof(tail));
    assert(memcmp(data + at, tail, sizeof(tail)) == 0);
}
static void test_plans(void)
{
    capture_config_t config;
    capture_plan_t plan;
    capture_config_default(&config);
    assert(capture_make_plan(&config, 168000000, &plan));
    assert(plan.prescaler == 0 && plan.reload == 167);
    config.divider = 0;
    assert(capture_make_plan(&config, 168000000, &plan));
    assert(plan.reload == 8); /* 18.667 MHz: never exceed advertised 20 MHz */
    config.divider = 0xffffff;
    assert(capture_make_plan(&config, 168000000, &plan));
    uint64_t ticks = ((uint64_t)plan.prescaler + 1) * (plan.reload + 1u);
    assert(ticks >= ((uint64_t)168000000 * 0x1000000u + 99999999) / 100000000);
    config.samples = CAPTURE_CAPACITY;
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
    capture_config_default(&config);
    const uint16_t unsupported[] = {1, 2, 4, 0x40, 0x80, 0x100, 0x8000};
    for (size_t i = 0; i < sizeof(unsupported)/sizeof(unsupported[0]); ++i) {
        config.flags = unsupported[i];
        assert(!capture_make_plan(&config, 168000000, &plan));
    }
}
static void test_commands(void)
{
    init();
    command(SUMP_COUNTS, 0xffffffff);
    assert(app.config.samples == 262144 && app.config.post_trigger == 262144);
    command(SUMP_RUN, 0);
    assert(fake.starts == 0 && app.capture.state == CAPTURE_ERROR);
    command(SUMP_RESET, 0);
    command(SUMP_COUNTS, 0);
    assert(app.config.samples == 4 && app.config.post_trigger == 4);
    command(SUMP_TRIGGER_MASK, 0x102);
    command(SUMP_TRIGGER_VALUE, 0x102);
    assert(app.config.trigger_mask == 2 && app.config.trigger_value == 2);
    command(SUMP_RUN, 0);
    assert(app.capture.state == CAPTURE_ARMED);
    command(SUMP_RUN, 0);
    assert(fake.starts == 1);
    command(SUMP_COUNTS, 0x00010001);
    assert(fake.plan.config.samples == 4); /* configuration snapshot */
    fake.event = CAPTURE_HW_TRIGGERED;
    analyzer_app_poll(&app);
    assert(app.capture.state == CAPTURE_RUNNING);
    command(SUMP_RESET, 0);
    assert(app.capture.state == CAPTURE_IDLE && app.config.trigger_mask == 0);
    fake.event = CAPTURE_HW_COMPLETE;
    analyzer_app_poll(&app);
    assert(app.capture.state == CAPTURE_IDLE && !fake.busy);
    fake.start_ok = false;
    command(SUMP_RUN, 0);
    assert(app.capture.state == CAPTURE_ERROR);
}
static void test_replies(void)
{
    init();
    command(SUMP_ID, 0);
    assert(!analyzer_app_receive(&app, SUMP_META));
    fake.reject = true;
    analyzer_app_poll(&app);
    assert(app.reply_length == 4 && !app.in_flight);
    fake.reject = false;
    analyzer_app_poll(&app);
    command(SUMP_META, 0);
    analyzer_app_poll(&app);
    finish_tx();
    assert(fake.wire_length == 4 && memcmp(fake.wire, "1ALS", 4) == 0);
    drain();
    uint8_t expected[64];
    size_t length = sump_metadata(expected, sizeof(expected), CAPTURE_CAPACITY, CAPTURE_MAX_RATE);
    assert(fake.wire_length == 4 + length);
    assert(memcmp(fake.wire + 4, expected, length) == 0);
}
static void test_capture_order(void)
{
    /* Includes DMA wrap, exact end, zero tail, partial packets and maximum RAM. */
    const uint32_t sizes[] = {4, 64, 68, CAPTURE_CAPACITY};
    for (size_t s = 0; s < sizeof(sizes)/sizeof(sizes[0]); ++s) {
        uint32_t count = sizes[s];
        const uint32_t tails[] = {0, 1, count - 1, count};
        for (size_t t = 0; t < 4; ++t) {
            init();
            app.config.samples = count;
            app.config.post_trigger = count;
            for (uint32_t i = 0; i < count; ++i) fake.data[i] = (uint8_t)(i * 7);
            fake.next = tails[t];
            command(SUMP_RUN, 0);
            fake.event = CAPTURE_HW_COMPLETE;
            fake.reject = true;
            analyzer_app_poll(&app);
            assert(app.sent == 0);
            fake.reject = false;
            drain();
            assert(fake.wire_length == count && app.capture.state == CAPTURE_IDLE);
            for (uint32_t i = 0; i < count; ++i)
                assert(fake.wire[i] == fake.data[(tails[t] + count - 1 - i) % count]);
        }
    }
}
static void test_reset_while_sending(void)
{
    init();
    app.config.samples = 128;
    app.config.post_trigger = 128;
    memset(fake.data, 0x5a, 128);
    command(SUMP_RUN, 0);
    fake.event = CAPTURE_HW_COMPLETE;
    analyzer_app_poll(&app);
    assert(fake.busy && app.capture.state == CAPTURE_SENDING);
    command(SUMP_RESET, 0);
    command(SUMP_ID, 0);
    analyzer_app_poll(&app);
    finish_tx();
    for (unsigned i = 0; i < 64; ++i) assert(fake.wire[i] == 0x5a);
    drain();
    assert(fake.wire_length == 68 && memcmp(fake.wire + 64, "1ALS", 4) == 0);
    command(SUMP_RUN, 0);
    fake.event = CAPTURE_HW_ERROR;
    analyzer_app_poll(&app);
    assert(app.capture.state == CAPTURE_ERROR);
}
static void test_eca_guards(void)
{
    init();
    assert(!capture_dispatch(&app.capture, CAPTURE_EVENT_TRIGGER, NULL));
    assert(!capture_dispatch(&app.capture, CAPTURE_EVENT_COMPLETE, NULL));
    assert(!capture_dispatch(&app.capture, CAPTURE_EVENT_SENT, NULL));
    app.config.trigger_mask = 1;
    command(SUMP_RUN, 0);
    fake.event = CAPTURE_HW_COMPLETE; /* trigger and completion between polls */
    analyzer_app_poll(&app);
    assert(app.capture.state == CAPTURE_SENDING);
    assert(!capture_dispatch(&app.capture, CAPTURE_EVENT_RUN, &app.config));
}
int main(void)
{
    test_parser();
    test_metadata();
    test_plans();
    test_commands();
    test_replies();
    test_capture_order();
    test_reset_while_sending();
    test_eca_guards();
    puts("ALL CORE TESTS PASSED");
    return 0;
}
