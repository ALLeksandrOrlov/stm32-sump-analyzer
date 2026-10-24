/** @file capture.c @brief Event-condition-action capture controller. */
#include "capture.h"

static bool capture_is_active(capture_state_t state)
{
    return (state == CAPTURE_ARMED) || (state == CAPTURE_RUNNING);
}

/** Keep state guards independent of hardware actions and error handling. */
static bool event_is_allowed(capture_state_t state, capture_event_t event)
{
    bool allowed = false;

    switch (event) {
    case CAPTURE_EVENT_RUN:
        allowed = (state == CAPTURE_IDLE);
        break;
    case CAPTURE_EVENT_TRIGGER:
        allowed = (state == CAPTURE_ARMED);
        break;
    case CAPTURE_EVENT_COMPLETE:
        allowed = capture_is_active(state);
        break;
    case CAPTURE_EVENT_SENT:
        allowed = (state == CAPTURE_SENDING);
        break;
    case CAPTURE_EVENT_RESET:
    case CAPTURE_EVENT_ERROR:
        allowed = true;
        break;
    default:
        break;
    }
    return allowed;
}

static void stop_capture(capture_controller_t *controller, capture_state_t state)
{
    controller->driver.stop(controller->driver.context);
    controller->state = state;
}

static bool start_capture(capture_controller_t *controller,
                          const capture_config_t *config)
{
    bool started = false;

    if ((config != NULL) &&
        capture_make_plan(config, controller->timer_clock_hz, &controller->plan)) {
        started = controller->driver.start(controller->driver.context,
                                            &controller->plan);
    }

    if (started) {
        controller->state = (controller->plan.config.trigger_mask == 0U)
            ? CAPTURE_RUNNING : CAPTURE_ARMED;
    } else {
        /* start() can fail after enabling DMA or an interrupt. */
        stop_capture(controller, CAPTURE_ERROR);
    }
    return started;
}

void capture_controller_init(capture_controller_t *controller,
                             capture_driver_t driver, uint32_t timer_clock_hz)
{
    *controller = (capture_controller_t){
        .state = CAPTURE_IDLE,
        .driver = driver,
        .timer_clock_hz = timer_clock_hz
    };
}

bool capture_dispatch(capture_controller_t *controller, capture_event_t event,
                      const capture_config_t *config)
{
    bool applied = false;

    if (!event_is_allowed(controller->state, event)) {
        return false;
    }

    switch (event) {
    case CAPTURE_EVENT_RUN:
        applied = start_capture(controller, config);
        break;
    case CAPTURE_EVENT_RESET:
        stop_capture(controller, CAPTURE_IDLE);
        applied = true;
        break;
    case CAPTURE_EVENT_TRIGGER:
        controller->state = CAPTURE_RUNNING;
        applied = true;
        break;
    case CAPTURE_EVENT_COMPLETE:
        controller->state = CAPTURE_SENDING;
        applied = true;
        break;
    case CAPTURE_EVENT_SENT:
        controller->state = CAPTURE_IDLE;
        applied = true;
        break;
    case CAPTURE_EVENT_ERROR:
        stop_capture(controller, CAPTURE_ERROR);
        applied = true;
        break;
    default:
        break;
    }
    return applied;
}

void capture_poll(capture_controller_t *controller)
{
    capture_hw_event_t event;

    if (!capture_is_active(controller->state)) {
        return;
    }

    event = controller->driver.poll(controller->driver.context);
    switch (event) {
    case CAPTURE_HW_NONE:
        break;
    case CAPTURE_HW_TRIGGERED:
        (void)capture_dispatch(controller, CAPTURE_EVENT_TRIGGER, NULL);
        break;
    case CAPTURE_HW_COMPLETE:
        (void)capture_dispatch(controller, CAPTURE_EVENT_COMPLETE, NULL);
        break;
    case CAPTURE_HW_ERROR:
    default:
        /* An invalid driver event is a contract failure: stop the hardware. */
        (void)capture_dispatch(controller, CAPTURE_EVENT_ERROR, NULL);
        break;
    }
}

size_t capture_read_reverse(capture_controller_t *controller, uint32_t offset,
                            uint8_t *output, size_t capacity)
{
    const uint32_t sample_count = controller->plan.config.samples;
    const uint8_t *samples;
    uint32_t next_write = 0U;
    uint32_t read_index;
    size_t copy_length;

    if ((controller->state != CAPTURE_SENDING) || (offset >= sample_count) ||
        (capacity == 0U)) {
        return 0U;
    }

    samples = controller->driver.buffer(controller->driver.context, &next_write);
    if ((samples == NULL) || (next_write > sample_count)) {
        return 0U;
    }

    copy_length = (size_t)(sample_count - offset);
    if (copy_length > capacity) {
        copy_length = capacity;
    }

    /* The validated count is small enough that this sum cannot overflow.
     * One modulo finds the start; advancing each sample uses only a branch. */
    read_index = (next_write + sample_count - 1U - offset) % sample_count;
    for (size_t copied = 0U; copied < copy_length; ++copied) {
        output[copied] = samples[read_index];
        read_index = (read_index == 0U) ? (sample_count - 1U) : (read_index - 1U);
    }
    return copy_length;
}
