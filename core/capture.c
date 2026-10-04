#include "capture.h"
#include <string.h>

void capture_config_default(capture_config_t *config)
{
    *config = (capture_config_t){99, 1024, 1024, 0, 0, 0x38};
}

bool capture_make_plan(const capture_config_t *config, uint32_t timer_clock_hz,
                       capture_plan_t *plan)
{
    /* Eight probes, internal clock, no RLE/DDR. libsigrok's OLS driver always
     * sets its FPGA noise-filter flag (bit 1) below 100 MHz; accept that hint
     * without changing raw GPIO samples. Other group-disable bits are harmless
     * because the metadata advertises only one group. */
    if (!timer_clock_hz || !config->samples || config->samples > CAPTURE_BUFFER_SAMPLES ||
        !config->post_trigger || config->post_trigger > config->samples ||
        (config->flags & (uint16_t)~0x3au)) return false;

    uint64_t ticks = ((uint64_t)timer_clock_hz * ((config->divider & 0xffffffu) + 1u)
                      + CAPTURE_SUMP_CLOCK_HZ - 1u) / CAPTURE_SUMP_CLOCK_HZ;
    uint32_t minimum = (timer_clock_hz + CAPTURE_MAX_SAMPLE_RATE_HZ - 1u) / CAPTURE_MAX_SAMPLE_RATE_HZ;
    if (ticks < minimum) ticks = minimum;
    if (!ticks) ticks = 1;
    uint64_t prescaler = (ticks + 65535u) / 65536u;
    uint64_t reload = (ticks + prescaler - 1u) / prescaler;
    if (prescaler > 65536u || reload > 65536u) return false;
    plan->config = *config;
    plan->prescaler = (uint16_t)(prescaler - 1u);
    plan->reload = (uint16_t)(reload - 1u);
    return true;
}

void capture_controller_init(capture_controller_t *controller,
                             capture_driver_t driver, uint32_t timer_clock_hz)
{
    memset(controller, 0, sizeof(*controller));
    controller->driver = driver;
    controller->timer_clock_hz = timer_clock_hz;
}

bool capture_dispatch(capture_controller_t *c, capture_event_t event,
                      const capture_config_t *config)
{
    switch (event) {
    case CAPTURE_EVENT_RESET:
        c->driver.stop(c->driver.context);
        c->state = CAPTURE_IDLE;
        return true;
    case CAPTURE_EVENT_RUN:
        if (c->state != CAPTURE_IDLE) return false;
        if (!config || !capture_make_plan(config, c->timer_clock_hz, &c->plan) ||
            !c->driver.start(c->driver.context, &c->plan)) {
            c->driver.stop(c->driver.context);
            c->state = CAPTURE_ERROR;
            return false;
        }
        c->state = config->trigger_mask ? CAPTURE_ARMED : CAPTURE_RUNNING;
        return true;
    case CAPTURE_EVENT_TRIGGER:
        if (c->state != CAPTURE_ARMED) return false;
        c->state = CAPTURE_RUNNING;
        return true;
    case CAPTURE_EVENT_COMPLETE:
        /* The ISR may have both triggered and completed before the next poll. */
        if (c->state != CAPTURE_ARMED && c->state != CAPTURE_RUNNING) return false;
        c->state = CAPTURE_SENDING;
        return true;
    case CAPTURE_EVENT_SENT:
        if (c->state != CAPTURE_SENDING) return false;
        c->state = CAPTURE_IDLE;
        return true;
    case CAPTURE_EVENT_ERROR:
        c->driver.stop(c->driver.context);
        c->state = CAPTURE_ERROR;
        return true;
    }
    return false;
}

void capture_poll(capture_controller_t *c)
{
    if (c->state != CAPTURE_ARMED && c->state != CAPTURE_RUNNING) return;
    switch (c->driver.poll(c->driver.context)) {
    case CAPTURE_HW_TRIGGERED: capture_dispatch(c, CAPTURE_EVENT_TRIGGER, NULL); break;
    case CAPTURE_HW_COMPLETE: capture_dispatch(c, CAPTURE_EVENT_COMPLETE, NULL); break;
    case CAPTURE_HW_ERROR: capture_dispatch(c, CAPTURE_EVENT_ERROR, NULL); break;
    case CAPTURE_HW_NONE: break;
    }
}

size_t capture_read_reverse(capture_controller_t *c, uint32_t offset,
                            uint8_t *output, size_t capacity)
{
    uint32_t next = 0, count = c->plan.config.samples;
    if (c->state != CAPTURE_SENDING || offset >= count) return 0;
    const uint8_t *buffer = c->driver.buffer(c->driver.context, &next);
    if (!buffer || next > count) return 0;
    size_t length = count - offset;
    if (length > capacity) length = capacity;
    uint32_t index = (next + count - 1u - offset) % count;
    for (size_t i = 0; i < length; ++i) {
        output[i] = buffer[index];
        index = index ? index - 1u : count - 1u;
    }
    return length;
}
