/**
 * @file test_runtime_capture.c
 * @brief Exercise real register access, capture IRQs and task wakeups together.
 *
 * Only USB and the input signal are fixtures. The stimulus never polls the
 * controller, so a missing deferred interrupt cannot be hidden by the test.
 */
#include "analyzer_runtime.h"
#include "capture_stm32.h"
#include "capture_task_events.h"
#include "FreeRTOS.h"
#include "task.h"
#include "sump_protocol.h"
#include "stm32f4xx_hal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define USB_TEST_IRQ ((IRQn_Type)0)
#define FIXTURE ((volatile uint32_t *)0x60000000u)
enum { FIXTURE_ID, FIXTURE_LEVEL, FIXTURE_STEP, FIXTURE_AUTO_INCREMENT,
       FIXTURE_TICKS };
enum {
    USB_CONNECT = 1u,
    USB_RECEIVE = 2u,
    USB_TRANSMIT = 4u,
    USB_DISCONNECT = 8u
};
#define TEST_TIMEOUT pdMS_TO_TICKS(3000)
#define WAIT_UNTIL(condition) \
    do { \
        TickType_t started = xTaskGetTickCount(); \
        while (!(condition)) { \
            assert((TickType_t)(xTaskGetTickCount() - started) < TEST_TIMEOUT); \
            vTaskDelay(1); \
        } \
    } while (0)

uint32_t SystemCoreClock = 168000000;
uint32_t HAL_RCC_GetPCLK2Freq(void)
{
    return 168000000u;
}
static StaticTask_t stimulus_control;
enum { STIMULUS_STACK_WORDS = 512 };
static StackType_t stimulus_stack[STIMULUS_STACK_WORDS];
static struct {
    bool activated;
    bool out_credit;
    bool active;
    const uint8_t *borrowed;
    size_t length;
    uint8_t snapshot[64];
    uint8_t wire[128];
    size_t wire_length;
} usb;
static volatile unsigned usb_events;
static const uint8_t *usb_received;
static size_t usb_received_length;
static volatile bool usb_irq_finished;
static volatile unsigned exti_interrupts;
static volatile unsigned dma_interrupts;
static volatile unsigned timer_interrupts;
static volatile unsigned deferred_interrupts;

static void assert_capture_interrupt(IRQn_Type interrupt)
{
    assert(__get_IPSR() == (uint32_t)interrupt + 16u);
    assert(NVIC_GetPriority(interrupt) == 1);
}

void EXTI0_IRQHandler(void)
{
    assert_capture_interrupt(EXTI0_IRQn);
    ++exti_interrupts;
    capture_stm32_exti_irq();
}

void EXTI1_IRQHandler(void)
{
    capture_stm32_exti_irq();
}
void EXTI2_IRQHandler(void)
{
    capture_stm32_exti_irq();
}
void EXTI3_IRQHandler(void)
{
    capture_stm32_exti_irq();
}
void EXTI4_IRQHandler(void)
{
    capture_stm32_exti_irq();
}
void EXTI9_5_IRQHandler(void)
{
    capture_stm32_exti_irq();
}

void DMA2_Stream5_IRQHandler(void)
{
    assert_capture_interrupt(DMA2_Stream5_IRQn);
    ++dma_interrupts;
    capture_stm32_dma2_stream5_irq();
}

void TIM8_UP_TIM13_IRQHandler(void)
{
    assert_capture_interrupt(TIM8_UP_TIM13_IRQn);
    ++timer_interrupts;
    capture_stm32_tim8_update_irq();
}

void TIM6_DAC_IRQHandler(void)
{
    assert(__get_IPSR() == 16u + TIM6_DAC_IRQn);
    assert(NVIC_GetPriority(TIM6_DAC_IRQn) == configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY);
    ++deferred_interrupts;
    capture_task_events_irq();
}

static void assert_borrowed_packet_unchanged(void)
{
    assert(usb.active);
    assert(memcmp(usb.borrowed, usb.snapshot, usb.length) == 0);
}

static void usb_activate(void *context)
{
    (void)context;
    assert(__get_IPSR() == 0);
    assert(xTaskGetSchedulerState() == taskSCHEDULER_RUNNING);
    assert(!usb.activated);
    usb.activated = true;
    NVIC_EnableIRQ(USB_TEST_IRQ);
}

static bool usb_write(void *context, const uint8_t *bytes, size_t length)
{
    (void)context;
    assert(__get_IPSR() == 0);
    assert(!usb.active && length > 0 && length <= sizeof(usb.snapshot));
    usb.borrowed = bytes;
    usb.length = length;
    memcpy(usb.snapshot, bytes, length);
    usb.active = true;
    return true;
}

static bool usb_idle(void *context)
{
    (void)context;
    assert(__get_IPSR() == 0);
    if (usb.active) {
        assert_borrowed_packet_unchanged();
    }
    return !usb.active;
}

static bool usb_rearm(void *context)
{
    (void)context;
    assert(__get_IPSR() == 0 && !usb.out_credit);
    usb.out_credit = true;
    return true;
}

void ANALYZER_TEST_IRQHandler(void)
{
    assert(__get_IPSR() == 16);
    assert(NVIC_GetPriority(USB_TEST_IRQ) == 6);
    if ((usb_events & USB_DISCONNECT) != 0u) {
        analyzer_runtime_usb_disconnected_from_isr();
    }
    if ((usb_events & USB_CONNECT) != 0u) {
        analyzer_runtime_usb_connected_from_isr();
    }
    if ((usb_events & USB_RECEIVE) != 0u) {
        analyzer_runtime_usb_receive_from_isr(usb_received, usb_received_length);
    }
    if ((usb_events & USB_TRANSMIT) != 0u) {
        analyzer_runtime_usb_tx_complete_from_isr();
    }
    usb_irq_finished = true;
}

static void usb_interrupt(unsigned events)
{
    usb_irq_finished = false;
    usb_events = events;
    NVIC_SetPendingIRQ(USB_TEST_IRQ);
    __DSB();
    __ISB();
    WAIT_UNTIL(usb_irq_finished);
}

static void command(uint8_t opcode, uint32_t value)
{
    uint8_t bytes[] = {opcode, (uint8_t)value, (uint8_t)(value >> 8),
                       (uint8_t)(value >> 16), (uint8_t)(value >> 24)};
    WAIT_UNTIL(usb.out_credit);
    usb.out_credit = false;
    usb_received = bytes;
    usb_received_length = (opcode & 0x80u) ? sizeof(bytes) : 1;
    usb_interrupt(USB_RECEIVE);
}

static void drain(size_t total)
{
    while (usb.wire_length < total) {
        WAIT_UNTIL(usb.active);
        assert_borrowed_packet_unchanged();
        assert(usb.wire_length + usb.length <= sizeof(usb.wire));
        memcpy(usb.wire + usb.wire_length, usb.borrowed, usb.length);
        usb.wire_length += usb.length;
        usb.active = false;
        usb.borrowed = NULL;
        usb_interrupt(USB_TRANSMIT);
    }
    assert(usb.wire_length == total);
    vTaskDelay(2);
    assert(!usb.active);
}

static bool hardware_stopped(void)
{
    return !(TIM1->CR1 & TIM_CR1_CEN) && !(TIM8->CR1 & TIM_CR1_CEN) &&
           !(DMA2_Stream5->CR & DMA_SxCR_EN) && !(EXTI->IMR & 0xffu);
}

static void configure(uint32_t count, uint32_t post_trigger, uint8_t trigger)
{
    command(SUMP_RESET, 0);
    WAIT_UNTIL(hardware_stopped());
    FIXTURE[FIXTURE_AUTO_INCREMENT] = 0;
    FIXTURE[FIXTURE_LEVEL] = 0;
    FIXTURE[FIXTURE_STEP] = 1;
    FIXTURE[FIXTURE_TICKS] = 0;
    usb.wire_length = 0;
    /* A 1 kHz sample clock gives the task time to block between hardware IRQs. */
    command(SUMP_DIVIDER, 99999);
    command(SUMP_COUNTS, ((post_trigger / 4 - 1) << 16) | (count / 4 - 1));
    command(SUMP_TRIGGER_MASK, trigger);
    command(SUMP_TRIGGER_VALUE, trigger);
}

static void test_dma_completion_wakes_blocked_application(void)
{
    configure(68, 68, 0);
    FIXTURE[FIXTURE_AUTO_INCREMENT] = 1;
    unsigned previous_dma = dma_interrupts;
    unsigned previous_deferred = deferred_interrupts;
    command(SUMP_RUN, 0);
    WAIT_UNTIL(TIM1->CR1 & TIM_CR1_CEN);
    NVIC_DisableIRQ(TIM6_DAC_IRQn);
    WAIT_UNTIL(hardware_stopped());
    assert(dma_interrupts == previous_dma + 1);
    vTaskDelay(2);
    assert(deferred_interrupts == previous_deferred);
    assert(!usb.active); /* No periodic polling can replace the deferred IRQ. */

    NVIC_EnableIRQ(TIM6_DAC_IRQn);
    WAIT_UNTIL(usb.active);
    assert(deferred_interrupts == previous_deferred + 1);
    assert(usb.length == 64);
    drain(68);
    for (unsigned i = 0; i < 68; ++i) {
        assert(usb.wire[i] == (uint8_t)(68 - i));
    }
}

static void test_trigger_and_timer_completion_use_deferred_irq(void)
{
    configure(64, 16, 1);
    command(SUMP_RUN, 0);
    WAIT_UNTIL(FIXTURE[FIXTURE_TICKS] >= 80); /* Fill and wrap the pretrigger ring. */
    assert((EXTI->IMR & 1u) != 0 && !usb.active);
    unsigned previous_exti = exti_interrupts;
    unsigned previous_timer = timer_interrupts;
    unsigned previous_deferred = deferred_interrupts;
    NVIC_DisableIRQ(TIM6_DAC_IRQn);
    FIXTURE[FIXTURE_LEVEL] = 1;
    WAIT_UNTIL(exti_interrupts == previous_exti + 1);
    assert(TIM8->CR1 & TIM_CR1_CEN);
    vTaskDelay(2);
    assert(!usb.active && deferred_interrupts == previous_deferred);
    NVIC_EnableIRQ(TIM6_DAC_IRQn);
    WAIT_UNTIL(deferred_interrupts == previous_deferred + 1);
    WAIT_UNTIL(usb.active);
    assert(timer_interrupts == previous_timer + 1);
    assert(deferred_interrupts == previous_deferred + 2);
    assert(hardware_stopped());
    drain(64);
    unsigned post = 0;
    while (post < 64 && usb.wire[post] == 1) {
        ++post;
    }
    assert(post >= 16 && post <= 17);
    for (unsigned i = post; i < 64; ++i) {
        assert(usb.wire[i] == 0);
    }
}

static void test_reset_stops_armed_capture_without_spurious_event(void)
{
    configure(64, 16, 1);
    unsigned previous_deferred = deferred_interrupts;
    command(SUMP_RUN, 0);
    WAIT_UNTIL(FIXTURE[FIXTURE_TICKS] >= 8);
    assert((EXTI->IMR & 1u) != 0 && !usb.active);
    command(SUMP_RESET, 0);
    WAIT_UNTIL(hardware_stopped());
    vTaskDelay(2);
    assert(!usb.active && deferred_interrupts == previous_deferred);
    command(SUMP_ID, 0);
    drain(4);
    assert(memcmp(usb.wire, "1ALS", 4) == 0);
}


static void test_disconnect_stops_armed_capture_and_allows_fresh_session(void)
{
    configure(64, 16, 1);
    command(SUMP_RUN, 0);
    WAIT_UNTIL(FIXTURE[FIXTURE_TICKS] >= 8u);
    assert(!hardware_stopped());

    usb.out_credit = false;
    usb_interrupt(USB_DISCONNECT);
    WAIT_UNTIL(hardware_stopped());
    unsigned previous_deferred = deferred_interrupts;
    FIXTURE[FIXTURE_LEVEL] = 1u;
    vTaskDelay(2);
    assert(deferred_interrupts == previous_deferred);
    assert(!usb.active && !usb.out_credit);

    usb.out_credit = true;
    usb_interrupt(USB_CONNECT);
    command(SUMP_ID, 0);
    drain(4);
    assert(memcmp(usb.wire, "1ALS", 4) == 0);
    assert(hardware_stopped());
}

static void test_completed_capture_stays_immutable_during_host_pause(void)
{
    configure(68, 68, 0);
    FIXTURE[FIXTURE_AUTO_INCREMENT] = 1u;
    command(SUMP_RUN, 0);
    WAIT_UNTIL(usb.active);
    assert(hardware_stopped());
    assert(usb.length == 64u);

    /* A new RUN cannot reuse capture memory until both USB packets complete. */
    FIXTURE[FIXTURE_LEVEL] = 0xffu;
    command(SUMP_RUN, 0);
    vTaskDelay(4);
    assert(hardware_stopped());
    assert_borrowed_packet_unchanged();
    drain(68);
    for (unsigned index = 0u; index < 68u; ++index) {
        assert(usb.wire[index] == (uint8_t)(68u - index));
    }
}

static void stimulus(void *argument)
{
    (void)argument;
    WAIT_UNTIL(usb.activated && usb_irq_finished);
    test_dma_completion_wakes_blocked_application();
    test_trigger_and_timer_completion_use_deferred_irq();
    test_reset_stops_armed_capture_without_spurious_event();
    test_disconnect_stops_armed_capture_and_allows_fresh_session();
    test_completed_capture_stays_immutable_during_host_pause();
    puts("ALL RTOS CAPTURE TESTS PASSED");
    /* Keep a measured margin after all test paths, including semihost output. */
    assert(uxTaskGetStackHighWaterMark2(NULL) >= 128);
    exit(0);
}

int main(void)
{
    assert(FIXTURE[FIXTURE_ID] == 0x534c4131);
    HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);
    capture_stm32_init();
    analyzer_usb_port_t port = {
        .activate = usb_activate, .write = usb_write,
        .idle = usb_idle, .rearm = usb_rearm,
    };
    assert(analyzer_runtime_init(capture_stm32_driver(), port,
                                 capture_stm32_timer_clock_hz()));
    capture_task_events_init();
    NVIC_SetPriority(USB_TEST_IRQ, 6);
    NVIC_DisableIRQ(USB_TEST_IRQ);
    usb.out_credit = true;
    usb_events = USB_CONNECT;
    NVIC_SetPendingIRQ(USB_TEST_IRQ);
    assert(xTaskCreateStatic(stimulus, "capture-test", STIMULUS_STACK_WORDS, NULL, 4,
                             stimulus_stack, &stimulus_control));
    vTaskStartScheduler();
    return 1;
}
