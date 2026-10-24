/** @file capture_plan.c @brief Pure, bounded conversion from settings to timer values. */
#include "capture.h"

#define TIMER_DIVIDER_LIMIT UINT64_C(65536)
#define NANOSECONDS_PER_SECOND UINT64_C(1000000000)

static uint64_t divide_round_up(uint64_t numerator, uint64_t denominator)
{
    const uint64_t quotient = numerator / denominator;
    const uint64_t remainder = numerator % denominator;

    /* Quotient/remainder avoids overflowing numerator + denominator - 1. */
    return quotient + ((remainder == 0U) ? UINT64_C(0) : UINT64_C(1));
}

static bool trigger_is_supported(uint32_t mask)
{
    if (mask == 0U) {
        return true;
    }
    return (mask <= UINT8_MAX) && ((mask & (mask - 1U)) == 0U);
}

static bool configuration_is_valid(const capture_config_t *config)
{
    return (config->sample_period_ns > 0U) &&
        (config->samples > 0U) && (config->samples <= CAPTURE_BUFFER_SAMPLES) &&
        (config->post_trigger > 0U) && (config->post_trigger <= config->samples) &&
        trigger_is_supported(config->trigger_mask);
}

void capture_config_default(capture_config_t *config)
{
    *config = (capture_config_t){
        .sample_period_ns = 1000U,
        .samples = 1024U,
        .post_trigger = 1024U,
        .trigger_mask = 0U,
        .trigger_value = 0U
    };
}

bool capture_make_plan(const capture_config_t *config, uint32_t timer_clock_hz,
                       capture_plan_t *plan)
{
    uint64_t requested_ticks;
    uint64_t minimum_ticks;
    uint64_t prescaler_divisor;
    uint64_t reload_divisor;

    if ((timer_clock_hz == 0U) || !configuration_is_valid(config)) {
        return false;
    }

    requested_ticks = divide_round_up(
        (uint64_t)timer_clock_hz * (uint64_t)config->sample_period_ns,
        NANOSECONDS_PER_SECOND);
    minimum_ticks = divide_round_up(timer_clock_hz, CAPTURE_MAX_SAMPLE_RATE_HZ);
    if (requested_ticks < minimum_ticks) {
        requested_ticks = minimum_ticks;
    }
    if (requested_ticks > (TIMER_DIVIDER_LIMIT * TIMER_DIVIDER_LIMIT)) {
        return false;
    }

    /* Use the smallest prescaler that fits ARR. Both roundings increase the
     * period, and each effective divisor lies in the inclusive range 1..65536. */
    prescaler_divisor = divide_round_up(requested_ticks, TIMER_DIVIDER_LIMIT);
    reload_divisor = divide_round_up(requested_ticks, prescaler_divisor);

    /* Publish only after every check, leaving the caller's plan intact on failure. */
    plan->config = *config;
    plan->prescaler = (uint16_t)(prescaler_divisor - 1U);
    plan->reload = (uint16_t)(reload_divisor - 1U);
    return true;
}
