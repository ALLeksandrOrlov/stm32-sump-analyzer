/**
 * @file test_analyzer_runtime.c
 * @brief Black-box scheduling and ownership tests for the production runtime.
 *
 * A higher-priority task drives real interrupts and delayed endpoint completions.
 * The fixtures reject driver calls from ISR context and detect reuse of a USB
 * buffer before its completion. All waits have a deadline.
 */
#include "analyzer_runtime.h"
#include "FreeRTOS.h"
#include "task.h"
#include "sump_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NVIC_ENABLE_IRQ0 (*(volatile uint32_t *)0xe000e100u)
#define NVIC_DISABLE_IRQ0 (*(volatile uint32_t *)0xe000e180u)
#define NVIC_PEND_IRQ0 (*(volatile uint32_t *)0xe000e200u)
#define NVIC_IRQ0_PRIORITY (*(volatile uint8_t *)0xe000e400u)
#define SCB_PRIORITY_GROUPING (*(volatile uint32_t *)0xe000ed0cu)
#define TEST_TIMEOUT pdMS_TO_TICKS(3000)
#define WAIT_UNTIL(condition) \
    do { \
        TickType_t started = xTaskGetTickCount(); \
        while (!(condition)) { \
            assert((TickType_t)(xTaskGetTickCount() - started) < TEST_TIMEOUT); \
            vTaskDelay(1); \
        } \
    } while (0)

enum {
    IRQ_CONNECT = 1u,
    IRQ_DISCONNECT = 2u,
    IRQ_RECEIVE = 4u,
    IRQ_TRANSMIT = 8u,
    IRQ_CAPTURE = 16u,
    IRQ_RESUME = 32u
};

typedef struct {
    uint8_t samples[1024];
    capture_plan_t plan;
    volatile capture_hw_event_t event;
    unsigned starts;
    unsigned stops;
    unsigned polls;
} capture_fake_t;

typedef struct {
    bool out_credit;
    bool active;
    bool suspended;
    const uint8_t *borrowed;
    uint8_t snapshot[64];
    size_t active_length;
    uint8_t wire[2048];
    size_t wire_length;
    unsigned activations;
    unsigned attempts;
    unsigned accepted;
    unsigned rearms;
    unsigned rejected_rearms;
    unsigned idle_checks;
    unsigned reject_writes;
} usb_fake_t;

static capture_fake_t capture;
static usb_fake_t usb;
static StaticTask_t stimulus_control;
static StackType_t stimulus_stack[1024];
static volatile unsigned irq_events;
static const uint8_t *irq_bytes;
static size_t irq_length;
static volatile bool irq_finished;

static uint32_t interrupt_number(void)
{
    uint32_t result;
    __asm volatile ("mrs %0, ipsr" : "=r"(result));
    return result;
}

static bool capture_start(void *context, const capture_plan_t *plan)
{
    capture_fake_t *fake = context;
    assert(interrupt_number() == 0);
    assert(plan->config.samples <= sizeof(fake->samples));
    fake->plan = *plan;
    fake->event = CAPTURE_HW_NONE;
    ++fake->starts;
    for (uint32_t i = 0; i < plan->config.samples; ++i) {
        fake->samples[i] = (uint8_t)i;
    }
    return true;
}

static void capture_stop(void *context)
{
    capture_fake_t *fake = context;
    assert(interrupt_number() == 0);
    fake->event = CAPTURE_HW_NONE;
    ++fake->stops;
}

static capture_hw_event_t capture_poll_event(void *context)
{
    capture_fake_t *fake = context;
    assert(interrupt_number() == 0);
    ++fake->polls;
    capture_hw_event_t result = fake->event;
    fake->event = CAPTURE_HW_NONE;
    return result;
}

static const uint8_t *capture_buffer(void *context, uint32_t *next)
{
    capture_fake_t *fake = context;
    assert(interrupt_number() == 0);
    *next = fake->plan.config.samples;
    return fake->samples;
}

static void assert_active_bytes_unchanged(void)
{
    assert(usb.active);
    assert(memcmp(usb.borrowed, usb.snapshot, usb.active_length) == 0);
}

static bool usb_write(void *context, const uint8_t *bytes, size_t length)
{
    usb_fake_t *fake = context;
    assert(interrupt_number() == 0);
    assert(length > 0 && length <= sizeof(fake->snapshot));
    ++fake->attempts;
    if (fake->suspended) {
        return false;
    }
    if (fake->reject_writes) {
        --fake->reject_writes;
        return false;
    }
    assert(!fake->active);
    fake->active = true;
    fake->borrowed = bytes;
    fake->active_length = length;
    memcpy(fake->snapshot, bytes, length);
    ++fake->accepted;
    return true;
}

static void usb_activate(void *context)
{
    usb_fake_t *fake = context;
    assert(interrupt_number() == 0);
    assert(xTaskGetSchedulerState() == taskSCHEDULER_RUNNING);
    assert(fake->activations == 0);
    ++fake->activations;
    NVIC_ENABLE_IRQ0 = 1;
    __asm volatile ("dsb\nisb" ::: "memory");
}

static bool usb_idle(void *context)
{
    usb_fake_t *fake = context;
    assert(interrupt_number() == 0);
    ++fake->idle_checks;
    if (fake->active) {
        assert_active_bytes_unchanged();
    }
    return !fake->active && !fake->suspended;
}

static bool usb_rearm(void *context)
{
    usb_fake_t *fake = context;
    assert(interrupt_number() == 0);
    assert(!fake->out_credit);
    if (fake->suspended) {
        ++fake->rejected_rearms;
        return false;
    }
    fake->out_credit = true;
    ++fake->rearms;
    return true;
}

void ANALYZER_TEST_IRQHandler(void)
{
    assert(interrupt_number() == 16);
    unsigned starts = capture.starts;
    unsigned stops = capture.stops;
    unsigned polls = capture.polls;
    unsigned attempts = usb.attempts;
    unsigned rearms = usb.rearms;
    unsigned idle_checks = usb.idle_checks;
    unsigned events = irq_events;
    if ((events & IRQ_DISCONNECT) != 0u) {
        analyzer_runtime_usb_disconnected_from_isr();
    }
    if ((events & IRQ_CONNECT) != 0u) {
        analyzer_runtime_usb_connected_from_isr();
    }
    if ((events & IRQ_RECEIVE) != 0u) {
        analyzer_runtime_usb_receive_from_isr(irq_bytes, irq_length);
    }
    if ((events & IRQ_TRANSMIT) != 0u) {
        analyzer_runtime_usb_tx_complete_from_isr();
    }
    if ((events & IRQ_CAPTURE) != 0u) {
        analyzer_runtime_capture_event_from_isr();
    }
    if ((events & IRQ_RESUME) != 0u) {
        analyzer_runtime_usb_resumed_from_isr();
    }
    /* The callbacks may copy input and wake tasks, but must not run protocol,
     * capture-driver operations, or task-only USB functions in interrupt context. */
    assert(capture.starts == starts && capture.stops == stops && capture.polls == polls);
    assert(usb.attempts == attempts && usb.rearms == rearms && usb.idle_checks == idle_checks);
    irq_finished = true;
}

static void interrupt(unsigned events)
{
    irq_finished = false;
    irq_events = events;
    NVIC_PEND_IRQ0 = 1;
    __asm volatile ("dsb\nisb" ::: "memory");
    WAIT_UNTIL(irq_finished);
}

static void receive(const uint8_t *bytes, size_t length)
{
    assert(length <= 64);
    WAIT_UNTIL(usb.out_credit);
    usb.out_credit = false;
    irq_bytes = bytes;
    irq_length = length;
    interrupt(IRQ_RECEIVE);
}

static void command(uint8_t opcode, uint32_t value)
{
    uint8_t bytes[5] = {opcode, (uint8_t)value, (uint8_t)(value >> 8),
                        (uint8_t)(value >> 16), (uint8_t)(value >> 24)};
    receive(bytes, (opcode & 0x80u) != 0u ? sizeof(bytes) : 1u);
}

static void complete_transmit(void)
{
    assert_active_bytes_unchanged();
    assert(usb.wire_length + usb.active_length <= sizeof(usb.wire));
    memcpy(usb.wire + usb.wire_length, usb.snapshot, usb.active_length);
    usb.wire_length += usb.active_length;
    usb.active = false;
    usb.borrowed = NULL;
    interrupt(IRQ_TRANSMIT);
}

static void session_reset(void)
{
    /* Physical disconnect releases the endpoint before its callback. */
    usb.active = false;
    usb.borrowed = NULL;
    usb.reject_writes = 0;
    usb.suspended = false;
    usb.out_credit = true; /* CDC arms the first OUT packet during connection. */
    unsigned stops = capture.stops;
    interrupt(IRQ_DISCONNECT | IRQ_CONNECT);
    WAIT_UNTIL(capture.stops > stops);
    vTaskDelay(2);
    assert(!usb.active);
    usb.wire_length = 0;
}

static void drain(size_t total)
{
    while (usb.wire_length < total) {
        WAIT_UNTIL(usb.active);
        complete_transmit();
    }
    assert(usb.wire_length == total);
    vTaskDelay(2);
    assert(!usb.active);
}

static void test_fragmented_commands_and_capture(size_t count)
{
    session_reset();
    unsigned starts = capture.starts;
    uint8_t encoded = (uint8_t)(count / 4 - 1);
    uint8_t first[] = {SUMP_COUNTS, encoded};
    uint8_t last[] = {0, encoded, 0, SUMP_RUN};
    receive(first, sizeof(first));
    memset(first, 0xff, sizeof(first)); /* The callback must have copied input. */
    receive(last, sizeof(last));
    WAIT_UNTIL(capture.starts == starts + 1);
    assert(capture.plan.config.samples == count);
    assert(capture.plan.config.post_trigger == count);
    assert(capture.plan.config.sample_period_ns == 1000);
    vTaskDelay(2);

    capture.event = CAPTURE_HW_COMPLETE;
    vTaskDelay(2);
    assert(!usb.active); /* Capture completion needs its deferred IRQ wake-up. */
    interrupt(IRQ_CAPTURE);
    WAIT_UNTIL(usb.active);
    assert(usb.active_length == 64);
    command(SUMP_RUN, 0);
    vTaskDelay(2);
    assert(capture.starts == starts + 1);
    assert_active_bytes_unchanged();

    if (count > 64) {
        complete_transmit();
        WAIT_UNTIL(usb.active);
        assert(usb.active_length == count - 64);
        command(SUMP_RUN, 0);
        vTaskDelay(2);
        assert(capture.starts == starts + 1);
    }
    complete_transmit();
    vTaskDelay(2);
    assert(usb.wire_length == count && !usb.active);
    for (size_t i = 0; i < count; ++i) {
        assert(usb.wire[i] == (uint8_t)(count - 1 - i));
    }
    command(SUMP_RUN, 0);
    WAIT_UNTIL(capture.starts == starts + 2);
}

static void test_burst_backpressure(void)
{
    session_reset();
    size_t packets = 0;
    do {
        uint8_t ids[64];
        memset(ids, SUMP_ID, sizeof(ids));
        receive(ids, sizeof(ids));
        memset(ids, SUMP_RESET, sizeof(ids));
        ++packets;
        vTaskDelay(2);
        assert(packets <= 6); /* Bounded RX/TX storage must eventually NAK. */
    } while (usb.out_credit);
    WAIT_UNTIL(usb.active);
    assert(usb.active_length == 4);
    assert(memcmp(usb.snapshot, "1ALS", 4) == 0);
    unsigned attempts = usb.attempts;
    vTaskDelay(5);
    assert(!usb.out_credit && usb.attempts == attempts);
    assert_active_bytes_unchanged();
    drain(packets * 64 * 4);
    for (size_t i = 0; i < usb.wire_length; ++i) {
        assert(usb.wire[i] == (uint8_t)"1ALS"[i % 4]);
    }
    WAIT_UNTIL(usb.out_credit);
}

static void test_reset_keeps_only_active_packet(void)
{
    session_reset();
    unsigned starts = capture.starts;
    command(SUMP_COUNTS, (127u << 16) | 127u); /* 512 samples fill the TX queue. */
    command(SUMP_RUN, 0);
    WAIT_UNTIL(capture.starts == starts + 1);
    capture.event = CAPTURE_HW_COMPLETE;
    interrupt(IRQ_CAPTURE);
    WAIT_UNTIL(usb.active);
    vTaskDelay(2);
    unsigned stops = capture.stops;
    command(SUMP_RESET, 0);
    WAIT_UNTIL(capture.stops > stops);
    assert_active_bytes_unchanged();
    assert(usb.active_length == 64);
    complete_transmit();
    vTaskDelay(3);
    assert(!usb.active && usb.wire_length == 64);
    for (size_t i = 0u; i < 64u; ++i) {
        assert(usb.wire[i] == (uint8_t)(511u - i));
    }
    command(SUMP_ID, 0);
    WAIT_UNTIL(usb.active);
    complete_transmit();
    assert(usb.wire_length == 68);
    assert(memcmp(usb.wire + 64, "1ALS", 4) == 0);
}

static void test_reconnect_drops_stale_session(void)
{
    session_reset();
    uint8_t request[] = {SUMP_ID, SUMP_ID, SUMP_DIVIDER, 0x55};
    receive(request, sizeof(request));
    WAIT_UNTIL(usb.active);
    WAIT_UNTIL(usb.out_credit);
    vTaskDelay(2); /* Leave one ID active, another queued and a partial divider. */
    usb.active = false;
    usb.borrowed = NULL;
    usb.out_credit = false; /* First packet consumes CDC's initial OUT credit. */
    uint8_t first = SUMP_ID;
    irq_bytes = &first;
    irq_length = 1;
    interrupt(IRQ_DISCONNECT | IRQ_CONNECT | IRQ_RECEIVE);
    first = SUMP_RESET;
    WAIT_UNTIL(usb.active);
    assert(usb.active_length == 4 && memcmp(usb.snapshot, "1ALS", 4) == 0);
    drain(4);
    unsigned starts = capture.starts;
    command(SUMP_RUN, 0);
    WAIT_UNTIL(capture.starts == starts + 1);
    assert(capture.plan.config.samples == 1024);
    assert(capture.plan.config.sample_period_ns == 1000);
}

static void test_write_retry_and_order(void)
{
    session_reset();
    unsigned attempts = usb.attempts;
    usb.reject_writes = 2;
    command(SUMP_ID, 0);
    WAIT_UNTIL(usb.active);
    assert(usb.attempts == attempts + 3);
    command(SUMP_META, 0);
    vTaskDelay(3);
    assert(usb.active_length == 4 && memcmp(usb.snapshot, "1ALS", 4) == 0);
    assert_active_bytes_unchanged();
    complete_transmit();
    WAIT_UNTIL(usb.active);
    assert(usb.active_length > 4 && usb.snapshot[0] == 1);
    complete_transmit();
    vTaskDelay(2);
    assert(!usb.active && memcmp(usb.wire, "1ALS", 4) == 0);
}

static void test_receive_fault_preserves_active_transfer(void)
{
    const uint32_t invalid_lengths[] = {65, 1};
    for (size_t i = 0; i < sizeof(invalid_lengths) / sizeof(invalid_lengths[0]); ++i) {
        session_reset();
        command(SUMP_ID, 0);
        WAIT_UNTIL(usb.active);
        uint8_t partial[] = {SUMP_DIVIDER, 0x12};
        receive(partial, sizeof(partial));
        WAIT_UNTIL(usb.out_credit);
        vTaskDelay(2);
        unsigned stops = capture.stops;
        usb.out_credit = false;
        irq_bytes = NULL;
        irq_length = invalid_lengths[i];
        interrupt(IRQ_RECEIVE);
        WAIT_UNTIL(capture.stops > stops);
        vTaskDelay(2);
        /* A bad callback resets protocol state, not the physical USB endpoint. */
        assert_active_bytes_unchanged();
        assert(usb.active_length == 4 && memcmp(usb.snapshot, "1ALS", 4) == 0);
        complete_transmit();
        command(SUMP_ID, 0);
        WAIT_UNTIL(usb.active);
        complete_transmit();
        assert(usb.wire_length == 8 && memcmp(usb.wire, "1ALS1ALS", 8) == 0);

        stops = capture.stops;
        receive(NULL, 0);
        WAIT_UNTIL(usb.out_credit);
        assert(capture.stops == stops); /* A zero-length USB packet is valid. */
    }
}


static void test_bus_reset_releases_endpoint_and_partial_command(void)
{
    session_reset();
    const uint8_t requests[] = {SUMP_ID, SUMP_ID, SUMP_DIVIDER, 0x55};
    receive(requests, sizeof(requests));
    WAIT_UNTIL(usb.active);
    vTaskDelay(2);
    unsigned previous_stops = capture.stops;

    /* Reconfiguration may report Init without a separate DeInit callback. The
     * adapter must first release the endpoint, including an unfinished IN. */
    usb.active = false;
    usb.borrowed = NULL;
    usb.out_credit = true;
    interrupt(IRQ_CONNECT);
    WAIT_UNTIL(capture.stops > previous_stops);
    vTaskDelay(2);
    assert(!usb.active);

    unsigned previous_starts = capture.starts;
    command(SUMP_RUN, 0);
    WAIT_UNTIL(capture.starts == previous_starts + 1u);
    assert(capture.plan.config.samples == 1024);
    assert(capture.plan.config.sample_period_ns == 1000);
    command(SUMP_ID, 0);
    drain(4);
    assert(memcmp(usb.wire, "1ALS", 4) == 0);
}

static void test_reconnect_discards_packet_rejected_by_endpoint(void)
{
    session_reset();
    unsigned previous_attempts = usb.attempts;
    usb.reject_writes = 100;
    command(SUMP_META, 0);
    WAIT_UNTIL(usb.attempts > previous_attempts);
    assert(!usb.active);

    /* The TX task has dequeued a packet but owns no endpoint buffer yet. */
    session_reset();
    command(SUMP_ID, 0);
    drain(4);
    assert(memcmp(usb.wire, "1ALS", 4) == 0);
}

static void test_receive_overflow_invalidates_all_queued_commands(void)
{
    session_reset();
    command(SUMP_ID, 0);
    WAIT_UNTIL(usb.active);
    unsigned previous_stops = capture.stops;
    uint8_t requests[64];
    memset(requests, SUMP_RUN, sizeof(requests));
    unsigned previous_starts = capture.starts;

    /* Deliberately violate OUT credit to exercise recovery from an adapter
     * fault. Priority 4 keeps the application from draining between callbacks. */
    for (unsigned packet = 0u; packet < 5u; ++packet) {
        usb.out_credit = false;
        irq_bytes = requests;
        irq_length = sizeof(requests);
        interrupt(IRQ_RECEIVE);
    }
    WAIT_UNTIL(capture.stops > previous_stops);
    WAIT_UNTIL(usb.out_credit);
    assert(capture.starts == previous_starts);
    assert_active_bytes_unchanged();
    complete_transmit();
    command(SUMP_ID, 0);
    drain(8);
    assert(memcmp(usb.wire, "1ALS1ALS", 8) == 0);
}

static void test_disconnect_stops_capture_and_ignores_late_out(void)
{
    session_reset();
    unsigned previous_starts = capture.starts;
    command(SUMP_RUN, 0);
    WAIT_UNTIL(capture.starts == previous_starts + 1u);
    unsigned previous_stops = capture.stops;
    usb.out_credit = false;
    interrupt(IRQ_DISCONNECT);
    WAIT_UNTIL(capture.stops > previous_stops);

    const uint8_t request = SUMP_ID;
    irq_bytes = &request;
    irq_length = 1u;
    interrupt(IRQ_RECEIVE | IRQ_TRANSMIT | IRQ_CAPTURE);
    vTaskDelay(2);
    assert(!usb.active);
    assert(!usb.out_credit);
    assert(capture.starts == previous_starts + 1u);

    usb.out_credit = true;
    interrupt(IRQ_CONNECT);
    command(SUMP_ID, 0);
    drain(4);
    assert(memcmp(usb.wire, "1ALS", 4) == 0);
}


static void test_resume_retries_out_arm_without_resetting_partial_command(void)
{
    session_reset();
    unsigned previous_rejections = usb.rejected_rearms;
    unsigned previous_stops = capture.stops;
    const uint8_t first[] = {SUMP_DIVIDER, 199u};

    /* OUT completed just before suspension, but the application has not run
     * yet. No new OUT can arrive until the failed rearm is retried on resume. */
    usb.suspended = true;
    receive(first, sizeof(first));
    WAIT_UNTIL(usb.rejected_rearms > previous_rejections);
    vTaskDelay(2);
    assert(!usb.out_credit);
    assert(capture.stops == previous_stops);

    usb.suspended = false;
    interrupt(IRQ_RESUME);
    WAIT_UNTIL(usb.out_credit);
    const uint8_t last[] = {0u, 0u, 0u, SUMP_RUN};
    unsigned previous_starts = capture.starts;
    receive(last, sizeof(last));
    WAIT_UNTIL(capture.starts == previous_starts + 1u);
    assert(capture.plan.config.sample_period_ns == 2000u);
    assert(capture.stops == previous_stops);
}

static void test_resume_releases_completed_in_and_preserves_queued_reply(void)
{
    session_reset();
    const uint8_t requests[] = {SUMP_ID, SUMP_META};
    receive(requests, sizeof(requests));
    WAIT_UNTIL(usb.active);
    vTaskDelay(2);
    assert(usb.active_length == 4u);
    unsigned previous_idle_checks = usb.idle_checks;

    /* The completion notification and suspend can precede the TX task. Its
     * idle check remains false while suspended, even though IN has completed. */
    usb.suspended = true;
    complete_transmit();
    WAIT_UNTIL(usb.idle_checks > previous_idle_checks);
    vTaskDelay(2);
    assert(!usb.active);
    assert(usb.wire_length == 4u);

    usb.suspended = false;
    interrupt(IRQ_RESUME);
    WAIT_UNTIL(usb.active);
    assert(usb.active_length > 4u && usb.snapshot[0] == 1u);
    complete_transmit();
    vTaskDelay(2);
    assert(!usb.active);
    assert(memcmp(usb.wire, "1ALS", 4u) == 0);
}

static void stimulus(void *argument)
{
    (void)argument;
    /* The pending connection IRQ must remain gated until the Cortex-M port has
     * initialized its interrupt-priority validation and scheduler state. */
    WAIT_UNTIL(usb.activations == 1 && irq_finished);
    command(SUMP_ID, 0);
    drain(4);
    assert(memcmp(usb.wire, "1ALS", 4) == 0);
    test_fragmented_commands_and_capture(64);
    test_fragmented_commands_and_capture(68);
    test_burst_backpressure();
    test_reset_keeps_only_active_packet();
    test_reconnect_drops_stale_session();
    test_write_retry_and_order();
    test_receive_fault_preserves_active_transfer();
    test_bus_reset_releases_endpoint_and_partial_command();
    test_reconnect_discards_packet_rejected_by_endpoint();
    test_receive_overflow_invalidates_all_queued_commands();
    test_disconnect_stops_capture_and_ignores_late_out();
    test_resume_retries_out_arm_without_resetting_partial_command();
    test_resume_releases_completed_in_and_preserves_queued_reply();
    assert(usb.activations == 1);
    puts("ALL RTOS USB TESTS PASSED");
    exit(0);
}

int main(void)
{
    capture_driver_t driver = {&capture, capture_start, capture_stop,
                               capture_poll_event, capture_buffer};
    analyzer_usb_port_t port = {
        .context = &usb, .write = usb_write, .idle = usb_idle,
        .rearm = usb_rearm, .activate = usb_activate
    };
    analyzer_usb_port_t invalid_port = port;
    invalid_port.write = NULL;
    assert(!analyzer_runtime_init(driver, invalid_port, 168000000));
    assert(!analyzer_runtime_init(driver, port, 0));
    capture_driver_t invalid_driver = driver;
    invalid_driver.poll = NULL;
    assert(!analyzer_runtime_init(invalid_driver, port, 168000000));
    assert(analyzer_runtime_init(driver, port, 168000000));
    assert(!analyzer_runtime_init(driver, port, 168000000));
    /* Match HAL_Init's NVIC_PRIORITYGROUP_4. An early FromISR call would fail
     * the port's assertion before xPortStartScheduler initializes its limits. */
    SCB_PRIORITY_GROUPING = 0x05fa0000u | (3u << 8);
    NVIC_IRQ0_PRIORITY = 6u << 4;
    NVIC_DISABLE_IRQ0 = 1;
    usb.out_credit = true;
    irq_events = IRQ_CONNECT;
    NVIC_PEND_IRQ0 = 1;
    assert(xTaskCreateStatic(stimulus, "stimulus", 1024, NULL, 4,
                             stimulus_stack, &stimulus_control));
    vTaskStartScheduler();
    return 1;
}
