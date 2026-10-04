#ifndef CAPTURE_H
#define CAPTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CAPTURE_BUFFER_SAMPLES (28u * 1024u)
#define CAPTURE_MAX_SAMPLE_RATE_HZ 20000000u
#define CAPTURE_SUMP_CLOCK_HZ 100000000u

typedef struct {
    uint32_t divider;
    uint32_t samples;
    uint32_t post_trigger;
    uint8_t trigger_mask;
    uint8_t trigger_value;
    uint16_t flags;
} capture_config_t;

typedef struct {
    capture_config_t config;
    uint16_t prescaler;
    uint16_t reload;
} capture_plan_t;

typedef enum { CAPTURE_IDLE, CAPTURE_ARMED, CAPTURE_RUNNING,
               CAPTURE_SENDING, CAPTURE_ERROR } capture_state_t;
typedef enum { CAPTURE_EVENT_RUN, CAPTURE_EVENT_RESET, CAPTURE_EVENT_TRIGGER,
               CAPTURE_EVENT_COMPLETE, CAPTURE_EVENT_SENT,
               CAPTURE_EVENT_ERROR } capture_event_t;
typedef enum { CAPTURE_HW_NONE, CAPTURE_HW_TRIGGERED,
               CAPTURE_HW_COMPLETE, CAPTURE_HW_ERROR } capture_hw_event_t;

typedef struct {
    void *context;
    bool (*start)(void *context, const capture_plan_t *plan);
    void (*stop)(void *context);
    capture_hw_event_t (*poll)(void *context);
    /* Valid until the next start: byte samples, next write index in the ring. */
    const uint8_t *(*buffer)(void *context, uint32_t *next);
} capture_driver_t;

typedef struct {
    capture_state_t state;
    capture_plan_t plan;
    capture_driver_t driver;
    uint32_t timer_clock_hz;
} capture_controller_t;

void capture_config_default(capture_config_t *config);
bool capture_make_plan(const capture_config_t *config, uint32_t timer_clock_hz,
                       capture_plan_t *plan);
void capture_controller_init(capture_controller_t *controller,
                             capture_driver_t driver, uint32_t timer_clock_hz);
/* ECA dispatcher: events act only when their state/configuration guards hold. */
bool capture_dispatch(capture_controller_t *controller, capture_event_t event,
                      const capture_config_t *config);
void capture_poll(capture_controller_t *controller);
size_t capture_read_reverse(capture_controller_t *controller, uint32_t offset,
                            uint8_t *output, size_t capacity);

#endif
