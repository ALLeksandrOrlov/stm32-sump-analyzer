#include "capture.h"
#include <string.h>

enum {
    TIMER_REGISTER_VALUES = 65536u,
    NANOSECONDS_PER_SECOND = 1000000000u
};

void capture_config_default(capture_config_t *config)
{
    *config = (capture_config_t){
        .sample_period_ns = 1000, /* 1 MHz. */
        .samples = 1024,
        .post_trigger = 1024,
        .trigger_mask = 0,
        .trigger_value = 0
    };
}

static uint64_t divide_round_up(uint64_t numerator, uint64_t denominator)
{
    return (numerator + denominator - 1u) / denominator;
}

static bool config_is_valid(const capture_config_t *config)
{
    bool multiple_trigger_channels =
        (config->trigger_mask & (config->trigger_mask - 1u)) != 0;
    return config->sample_period_ns > 0 &&
        config->samples > 0 && config->samples <= CAPTURE_BUFFER_SAMPLES &&
        config->post_trigger > 0 && config->post_trigger <= config->samples &&
        config->trigger_mask <= UINT8_MAX &&
        !multiple_trigger_channels;
}

bool capture_make_plan(const capture_config_t *config, uint32_t timer_clock_hz,
                       capture_plan_t *plan)
{
    if (timer_clock_hz == 0 || !config_is_valid(config)) {
        return false;
    }

    uint64_t ticks = divide_round_up(
        (uint64_t)timer_clock_hz * config->sample_period_ns, NANOSECONDS_PER_SECOND);
    uint64_t minimum = divide_round_up(timer_clock_hz, CAPTURE_MAX_SAMPLE_RATE_HZ);
    if (ticks < minimum) {
        ticks = minimum;
    }
    if (ticks > (uint64_t)TIMER_REGISTER_VALUES * TIMER_REGISTER_VALUES) {
        return false;
    }

    /* Choose the smallest prescaler that fits the reload register, rounding
     * both divisions upward so quantization can only slow the sample clock. */
    uint64_t prescaler = divide_round_up(ticks, TIMER_REGISTER_VALUES);
    uint64_t reload = divide_round_up(ticks, prescaler);
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

bool capture_dispatch(capture_controller_t *controller, capture_event_t event,
                      const capture_config_t *config)
{
    switch (event) {
    case CAPTURE_EVENT_RESET:
        controller->driver.stop(controller->driver.context);
        controller->state = CAPTURE_IDLE;
        return true;
    case CAPTURE_EVENT_RUN:
        if (controller->state != CAPTURE_IDLE) {
            return false;
        }
        if (!config || !capture_make_plan(config, controller->timer_clock_hz, &controller->plan) ||
            !controller->driver.start(controller->driver.context, &controller->plan)) {
            controller->driver.stop(controller->driver.context);
            controller->state = CAPTURE_ERROR;
            return false;
        }
        controller->state = config->trigger_mask ? CAPTURE_ARMED : CAPTURE_RUNNING;
        return true;
    case CAPTURE_EVENT_TRIGGER:
        if (controller->state != CAPTURE_ARMED) {
            return false;
        }
        controller->state = CAPTURE_RUNNING;
        return true;
    case CAPTURE_EVENT_COMPLETE:
        /* The ISR may have both triggered and completed before the next poll. */
        if (controller->state != CAPTURE_ARMED && controller->state != CAPTURE_RUNNING) {
            return false;
        }
        controller->state = CAPTURE_SENDING;
        return true;
    case CAPTURE_EVENT_SENT:
        if (controller->state != CAPTURE_SENDING) {
            return false;
        }
        controller->state = CAPTURE_IDLE;
        return true;
    case CAPTURE_EVENT_ERROR:
        controller->driver.stop(controller->driver.context);
        controller->state = CAPTURE_ERROR;
        return true;
    }
    return false;
}

void capture_poll(capture_controller_t *controller)
{
    if (controller->state != CAPTURE_ARMED && controller->state != CAPTURE_RUNNING) {
        return;
    }
    switch (controller->driver.poll(controller->driver.context)) {
    case CAPTURE_HW_TRIGGERED:
        capture_dispatch(controller, CAPTURE_EVENT_TRIGGER, NULL);
        break;
    case CAPTURE_HW_COMPLETE:
        capture_dispatch(controller, CAPTURE_EVENT_COMPLETE, NULL);
        break;
    case CAPTURE_HW_ERROR:
        capture_dispatch(controller, CAPTURE_EVENT_ERROR, NULL);
        break;
    case CAPTURE_HW_NONE:
        break;
    }
}

size_t capture_read_reverse(capture_controller_t *controller, uint32_t offset,
                            uint8_t *output, size_t capacity)
{
    uint32_t next = 0;
    uint32_t count = controller->plan.config.samples;
    if (controller->state != CAPTURE_SENDING || offset >= count || capacity == 0) {
        return 0;
    }
    const uint8_t *buffer = controller->driver.buffer(controller->driver.context, &next);
    if (!buffer || next > count) {
        return 0;
    }
    size_t length = count - offset;
    if (length > capacity) {
        length = capacity;
    }
    uint32_t index = (next + count - 1u - offset) % count;
    for (size_t i = 0; i < length; ++i) {
        output[i] = buffer[index];
        index = index ? index - 1u : count - 1u;
    }
    return length;
}
