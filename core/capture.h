/**
 * @file capture.h
 * @brief Capture configuration, hardware port, and event-driven controller.
 *
 * This module has no dependency on the wire protocol, USB, or an RTOS. One
 * application task owns each controller. Hardware interrupt handlers publish
 * events through the driver; they never call the controller directly.
 */
#ifndef CAPTURE_H
#define CAPTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CAPTURE_BUFFER_SAMPLES (28U * 1024U)
#define CAPTURE_MAX_SAMPLE_RATE_HZ 20000000U

/** Physical settings for one capture; a successful RUN takes a snapshot. */
typedef struct {
    uint32_t sample_period_ns; /**< Requested period, rounded upward by the timer. */
    uint32_t samples;          /**< Number of samples, from 1 to the buffer limit. */
    uint32_t post_trigger;     /**< Samples after the edge, from 1 to samples. */
    uint32_t trigger_mask;     /**< Zero, or exactly one bit selecting channel 0..7. */
    uint8_t trigger_value;     /**< Selected bit: one for rising, zero for falling. */
} capture_config_t;

/** Validated configuration and the values written to the timer registers. */
typedef struct {
    capture_config_t config;
    uint16_t prescaler; /**< PSC; the effective division factor is PSC + 1. */
    uint16_t reload;    /**< ARR; the effective division factor is ARR + 1. */
} capture_plan_t;

typedef enum {
    CAPTURE_IDLE,
    CAPTURE_ARMED,
    CAPTURE_RUNNING,
    CAPTURE_SENDING,
    CAPTURE_ERROR
} capture_state_t;

typedef enum {
    CAPTURE_EVENT_RUN,
    CAPTURE_EVENT_RESET,
    CAPTURE_EVENT_TRIGGER,
    CAPTURE_EVENT_COMPLETE,
    CAPTURE_EVENT_SENT,
    CAPTURE_EVENT_ERROR
} capture_event_t;

/** A driver reports at most one event per poll, with errors taking priority. */
typedef enum {
    CAPTURE_HW_NONE,
    CAPTURE_HW_TRIGGERED,
    CAPTURE_HW_COMPLETE,
    CAPTURE_HW_ERROR
} capture_hw_event_t;

/**
 * @brief Nonblocking hardware port. Every callback is required.
 *
 * start() may fail after partially enabling hardware. stop() must be safe in
 * every state and remove pending events. After COMPLETE, buffer() exposes a
 * stopped, immutable byte buffer until the next start(). Its next index is the
 * next write position; both zero and the sample count denote a wrapped buffer.
 */
typedef struct {
    void *context;
    bool (*start)(void *context, const capture_plan_t *plan);
    void (*stop)(void *context);
    capture_hw_event_t (*poll)(void *context);
    const uint8_t *(*buffer)(void *context, uint32_t *next);
} capture_driver_t;

/** Caller-owned controller storage. Modify it only through this module. */
typedef struct {
    capture_state_t state;
    capture_plan_t plan;
    capture_driver_t driver;
    uint32_t timer_clock_hz;
} capture_controller_t;

/** @brief Select 1 MHz, 1024 samples, and capture without an edge trigger. */
void capture_config_default(capture_config_t *config);

/**
 * @brief Validate settings and calculate a pair of 16-bit timer dividers.
 * @param config Requested physical settings; must not be NULL.
 * @param timer_clock_hz Input clock of the sampling timer, before PSC.
 * @param plan Output, unchanged on failure; must not be NULL.
 * @return true if the configuration fits the hardware and capture buffer.
 *
 * Rounding may reduce the sample rate; it never exceeds either the requested
 * rate or CAPTURE_MAX_SAMPLE_RATE_HZ. Calculation time is independent of the
 * requested divider. There is no register access here.
 */
bool capture_make_plan(const capture_config_t *config, uint32_t timer_clock_hz,
                       capture_plan_t *plan);

/** @brief Initialize caller-owned storage; this does not touch the hardware. */
void capture_controller_init(capture_controller_t *controller,
                             capture_driver_t driver, uint32_t timer_clock_hz);

/**
 * @brief Apply one event through its state condition and action.
 * @param config Settings for RUN only; NULL rejects a RUN from IDLE.
 * @return true when the event was applied successfully.
 *
 * An event rejected by its state guard has no side effects. A failed RUN stops
 * partially configured hardware and enters ERROR; RESET is required to retry.
 * RESET and ERROR stop hardware in every state. COMPLETE is accepted directly
 * from ARMED because a trigger and completion can occur between task polls.
 */
bool capture_dispatch(capture_controller_t *controller, capture_event_t event,
                      const capture_config_t *config);

/** @brief Consume at most one hardware event, only while armed or running. */
void capture_poll(capture_controller_t *controller);

/**
 * @brief Copy a bounded portion of a finished capture, newest sample first.
 * @param offset Number of samples already consumed by the caller.
 * @param output Destination with at least capacity bytes; must not be NULL.
 * @param capacity Maximum number of bytes to copy.
 * @return Bytes copied; zero for exhaustion, wrong state, or invalid hardware data.
 *
 * This is a read-only view: retrying the same offset produces the same data.
 * The application advances its offset only after the transport accepts a copy.
 */
size_t capture_read_reverse(capture_controller_t *controller, uint32_t offset,
                            uint8_t *output, size_t capacity);

#endif
