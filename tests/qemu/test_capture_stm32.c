/* Runs the production STM32 adapter, CMSIS and HAL GPIO/NVIC code on the
 * extended QEMU machine. Only clock discovery and the USB transport are fake. */
#include "analyzer_app.h"
#include "capture_stm32.h"
#include "stm32f4xx_hal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Register offsets of the model's signal source and fault injector. */
enum { FIXTURE_ID, FIXTURE_LEVEL, FIXTURE_STEP, FIXTURE_AUTO_INCREMENT,
       FIXTURE_TICKS, FIXTURE_DMA_ERROR };
#define FIXTURE ((volatile uint32_t *)0x60000000u)
static analyzer_app_t app;
/* Small captures are retained for edge assertions. Full captures are checked
 * as packets arrive, leaving RAM for the production DMA buffer. */
static uint8_t wire[128];
static size_t wire_length;
static uint32_t expected_ramp_samples;
uint32_t SystemCoreClock = 168000000;
/* The model has a fixed timer clock. PLL/RCC startup is outside these tests. */
uint32_t HAL_RCC_GetPCLK2Freq(void)
{
    return 168000000U;
}

void EXTI0_IRQHandler(void)
{
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

void TIM8_UP_TIM13_IRQHandler(void)
{
    capture_stm32_tim8_update_irq();
}

void DMA2_Stream5_IRQHandler(void)
{
    capture_stm32_dma2_stream5_irq();
}

static bool idle(void *context)
{
    (void)context;
    return true;
}

/* The memory transport delivers synchronously, so cancellation has no queue. */
static void cancel(void *context)
{
    (void)context;
}
static bool send_bytes(void *ctx, const uint8_t *data, size_t size)
{
    (void)ctx;
    if (expected_ramp_samples != 0) {
        assert(wire_length + size <= expected_ramp_samples);
        for (size_t i = 0; i < size; ++i) {
            assert(data[i] == (uint8_t)(expected_ramp_samples - wire_length - i));
        }
    } else {
        assert(wire_length + size <= sizeof(wire));
        memcpy(wire + wire_length, data, size);
    }
    wire_length += size;
    return true;
}
static void command(uint8_t opcode, uint32_t value)
{
    assert(analyzer_app_receive(&app, opcode));
    if ((opcode & 0x80U) != 0U) {
        for (unsigned byte = 0U; byte < 4U; ++byte) {
            assert(analyzer_app_receive(&app, (uint8_t)(value >> (8U * byte))));
        }
    }
}
static void setup(uint32_t count, uint8_t mask, uint8_t value)
{
    analyzer_app_reset(&app);
    wire_length = 0;
    expected_ramp_samples = 0;
    FIXTURE[FIXTURE_AUTO_INCREMENT] = 0;
    FIXTURE[FIXTURE_LEVEL] = 0;
    FIXTURE[FIXTURE_STEP] = 1;
    FIXTURE[FIXTURE_TICKS] = 0;
    command(SUMP_DIVIDER, 999); /* 100 kHz, IRQ latency well below one sample. */
    command(SUMP_COUNTS, ((count / 4 - 1) << 16) | (count / 4 - 1));
    command(SUMP_TRIGGER_MASK, mask);
    command(SUMP_TRIGGER_VALUE, value);
}
static void wait_complete(void)
{
    for (unsigned i = 0; i < 50000000; ++i) {
        analyzer_app_poll(&app);
        if (app.capture.state == CAPTURE_IDLE) {
            return;
        }
        assert(app.capture.state != CAPTURE_ERROR);
    }
    assert(!"capture timed out");
}
static void wait_for_sample_ticks(uint32_t count)
{
    for (uint32_t poll = 0U; poll < 50000000U; ++poll) {
        analyzer_app_poll(&app);
        if (FIXTURE[FIXTURE_TICKS] >= count) {
            return;
        }
    }
    assert(!"sample timer did not progress");
}

static void wait_for_capture_state(capture_state_t expected)
{
    for (uint32_t poll = 0U; poll < 50000000U; ++poll) {
        analyzer_app_poll(&app);
        if (app.capture.state == expected) {
            return;
        }
    }
    assert(!"capture did not reach expected state");
}

static void assert_stopped(void)
{
    assert(!(TIM1->CR1 & TIM_CR1_CEN));
    assert(!(TIM8->CR1 & TIM_CR1_CEN));
    assert(!(DMA2_Stream5->CR & DMA_SxCR_EN));
    assert(!(EXTI->IMR & 0xff));
}
static void test_untriggered(uint32_t count)
{
    setup(count, 0, 0);
    expected_ramp_samples = count;
    FIXTURE[FIXTURE_AUTO_INCREMENT] = 1;
    command(SUMP_RUN, 0);
    wait_complete();
    assert(wire_length == count);
    assert_stopped();
}

static void test_word_capture_compacts_once(void)
{
    setup(4, 0, 0);
    capture_driver_t driver = capture_stm32_driver();
    capture_config_t config;
    capture_config_default(&config);
    /* An odd sample count ends partway through a packed word. The GPIO high
     * byte is deliberately nonzero and must not appear in the eight channels. */
    config.samples = 7;
    config.post_trigger = 7;
    config.sample_period_ns = 10000;
    capture_plan_t plan;
    assert(capture_make_plan(&config, capture_stm32_timer_clock_hz(), &plan));
    FIXTURE[FIXTURE_LEVEL] = 0xa5f8;
    FIXTURE[FIXTURE_STEP] = 0x101;
    FIXTURE[FIXTURE_AUTO_INCREMENT] = 1;
    assert(driver.start(driver.context, &plan));
    uint32_t widths = DMA2_Stream5->CR & (DMA_SxCR_PSIZE | DMA_SxCR_MSIZE);
    assert(widths == (DMA_SxCR_PSIZE_1 | DMA_SxCR_MSIZE_1));
    volatile const uint32_t *raw = (volatile const uint32_t *)DMA2_Stream5->M0AR;
    for (unsigned spin = 0; TIM1->CR1 & TIM_CR1_CEN; ++spin) {
        assert(spin < 1000000);
    }
    assert_stopped();
    assert(DMA2_Stream5->NDTR == 0);
    for (unsigned i = 0; i < config.samples; ++i) {
        /* The ISR must leave word samples intact until task context polls. */
        assert(raw[i] == (0xa5f8u + (i + 1u) * 0x101u));
    }
    assert(raw[config.samples] == 0); /* DMA stopped at the requested boundary. */

    assert(driver.poll(driver.context) == CAPTURE_HW_COMPLETE);
    uint32_t next;
    const uint8_t *packed = driver.buffer(driver.context, &next);
    assert(next == config.samples);
    for (unsigned repetition = 0; repetition < 3; ++repetition) {
        assert(driver.poll(driver.context) == CAPTURE_HW_NONE);
        for (unsigned i = 0; i < config.samples; ++i) {
            assert(packed[i] == (uint8_t)(0xf9u + i));
        }
    }
    driver.stop(driver.context);
}
static void test_edge(uint8_t pin, bool rising)
{
    uint8_t before = rising ? 0 : pin;
    uint8_t after = rising ? pin : 0;
    setup(64, pin, after);
    FIXTURE[FIXTURE_LEVEL] = before;
    command(SUMP_COUNTS, (3u << 16) | 15u); /* 64 total, 16 post-trigger. */
    command(SUMP_RUN, 0);
    assert(app.capture.state == CAPTURE_ARMED);
    wait_for_sample_ticks(80U); /* Wrap the DMA ring before triggering. */
    assert(app.capture.state == CAPTURE_ARMED);
    FIXTURE[FIXTURE_LEVEL] = after;
    wait_complete();
    assert(wire_length == 64);
    /* The newest post-trigger samples have the new level. Older pre-trigger
     * samples remain at the old level; this exercises actual circular DMA. */
    unsigned post = 0;
    while ((post < 64U) && (wire[post] == after)) {
        ++post;
    }
    assert(post >= 16 && post <= 17); /* one sample tolerance for IRQ phase */
    for (unsigned index = post; index < 64U; ++index) {
        assert(wire[index] == before);
    }
    assert_stopped();
}
static void test_trigger_ignores_other_edges(void)
{
    setup(64, 1, 1);
    FIXTURE[FIXTURE_LEVEL] = 1;
    command(SUMP_RUN, 0);
    /* A falling edge on the selected pin and a rising edge on another pin
     * must leave a rising-edge capture armed. */
    FIXTURE[FIXTURE_LEVEL] = 2;
    wait_for_sample_ticks(8U);
    assert(app.capture.state == CAPTURE_ARMED && wire_length == 0);
    FIXTURE[FIXTURE_LEVEL] = 3;
    wait_complete();
    assert(wire_length == 64);
    assert_stopped();
}
static void test_reset_and_error(void)
{
    setup(64, 1, 1);
    command(SUMP_RUN, 0);
    wait_for_sample_ticks(8U);
    command(SUMP_RESET, 0);
    assert_stopped();
    assert(app.capture.state == CAPTURE_IDLE && wire_length == 0);
    setup(64, 0, 0);
    FIXTURE[FIXTURE_DMA_ERROR] = 1;
    command(SUMP_RUN, 0);
    wait_for_capture_state(CAPTURE_ERROR);
    assert(app.capture.state == CAPTURE_ERROR);
    assert_stopped();
    assert(wire_length == 0);
    test_untriggered(4); /* reset recovers from DMA error */
}
/* TIM8 and DMA have equal interrupt priority. When their IRQs are pending
 * together, the lower TIM8 IRQ number runs first. DMA failure must still win
 * over completion so a partial capture can never reach the transport. */
static void test_dma_error_at_completion(void)
{
    setup(64, 1, 1);
    command(SUMP_RUN, 0);
    FIXTURE[FIXTURE_LEVEL] = 1;
    wait_for_capture_state(CAPTURE_RUNNING);
    assert(app.capture.state == CAPTURE_RUNNING);

    __disable_irq();
    TIM8->CNT = TIM8->ARR;
    FIXTURE[FIXTURE_DMA_ERROR] = 1;
    uint32_t ticks = FIXTURE[FIXTURE_TICKS];
    for (uint32_t poll = 0U; FIXTURE[FIXTURE_TICKS] == ticks; ++poll) {
        assert(poll < 50000000U);
    }
    __enable_irq();

    analyzer_app_poll(&app);
    assert(app.capture.state == CAPTURE_ERROR);
    assert(wire_length == 0);
    assert_stopped();
    test_untriggered(4);
}
static void test_invalid_dma_configuration(uint32_t clear, uint32_t set)
{
    setup(4, 0, 0);
    command(SUMP_RUN, 0);
    /* Configure an invalid stream while disabled, then verify the model
     * reports a transfer error instead of accepting unsupported transfers. */
    __disable_irq();
    TIM1->CR1 &= ~TIM_CR1_CEN;
    DMA2_Stream5->CR &= ~DMA_SxCR_EN;
    DMA2_Stream5->CR = (DMA2_Stream5->CR & ~clear) | set;
    DMA2_Stream5->CR |= DMA_SxCR_EN;
    TIM1->CR1 |= TIM_CR1_CEN;
    __enable_irq();
    wait_for_capture_state(CAPTURE_ERROR);
    assert(app.capture.state == CAPTURE_ERROR);
    assert(wire_length == 0);
    assert_stopped();
    test_untriggered(4);
}

static void test_prescaled_sample_clock(void)
{
    setup(4, 0, 0);
    command(SUMP_DIVIDER, 99999); /* One sample per millisecond. */
    FIXTURE[FIXTURE_AUTO_INCREMENT] = 1;
    command(SUMP_RUN, 0);
    assert(TIM1->PSC > 0);
    assert((TIM1->PSC + 1u) * (TIM1->ARR + 1u) == 168000u);
    wait_complete();
    assert(wire_length == 4);
    for (unsigned index = 0U; index < 4U; ++index) {
        assert(wire[index] == 4U - index);
    }
    assert_stopped();
}

static void test_gpio(void)
{
    FIXTURE[FIXTURE_LEVEL] = 0xa55a;
    assert(GPIOD->IDR == 0xa55a);
    GPIOD->IDR = 0; /* read-only */
    assert(GPIOD->IDR == 0xa55a);
    GPIOD->ODR = 0;
    GPIOD->BSRR = 3;
    assert(GPIOD->ODR == 3);
    GPIOD->BSRR = 1u << 16;
    assert(GPIOD->ODR == 2);
}
static void test_invalid_plan_stops_without_starting_dma(void)
{
    setup(4U, 0U, 0U);
    const capture_driver_t driver = capture_stm32_driver();
    capture_config_t config;
    capture_config_default(&config);
    config.samples = 8U;
    config.post_trigger = 8U;
    config.sample_period_ns = 10000U;
    capture_plan_t valid;
    assert(capture_make_plan(&config, capture_stm32_timer_clock_hz(), &valid));

    capture_plan_t invalid[] = {valid, valid, valid, valid, valid, valid};
    invalid[0].config.samples = 0U;
    invalid[1].config.samples = CAPTURE_BUFFER_SAMPLES + 1U;
    invalid[2].config.post_trigger = 0U;
    invalid[3].config.post_trigger = 9U;
    invalid[4].config.trigger_mask = 0x100U;
    invalid[5].config.trigger_mask = 0x03U;
    for (size_t index = 0U; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
        assert(!driver.start(driver.context, &invalid[index]));
        assert_stopped();
        assert(driver.poll(driver.context) == CAPTURE_HW_NONE);
    }
    assert(!driver.start(driver.context, NULL));
    assert_stopped();
    test_untriggered(4U);
}

static void test_stop_discards_pending_completion(void)
{
    setup(4U, 0U, 0U);
    const capture_driver_t driver = capture_stm32_driver();
    capture_config_t config;
    capture_config_default(&config);
    config.samples = 4U;
    config.post_trigger = 4U;
    config.sample_period_ns = 10000U;
    capture_plan_t plan;
    assert(capture_make_plan(&config, capture_stm32_timer_clock_hz(), &plan));
    assert(driver.start(driver.context, &plan));
    /* Let DMA finish without consuming its completion through the task. */
    for (uint32_t poll = 0U; (TIM1->CR1 & TIM_CR1_CEN) != 0U; ++poll) {
        assert(poll < 50000000U);
    }
    driver.stop(driver.context);
    capture_stm32_exti_irq();
    capture_stm32_tim8_update_irq();
    capture_stm32_dma2_stream5_irq();
    assert(driver.poll(driver.context) == CAPTURE_HW_NONE);
    assert_stopped();
    test_untriggered(4U);
}

int main(void)
{
    assert(FIXTURE[FIXTURE_ID] == 0x534c4131);
    capture_stm32_init();
    analyzer_app_init(&app, capture_stm32_driver(),
                      (analyzer_transport_t){.context = NULL, .write = send_bytes, .idle = idle, .cancel = cancel},
                      capture_stm32_timer_clock_hz());
    test_gpio();
    test_invalid_plan_stops_without_starting_dma();
    test_stop_discards_pending_completion();
    test_word_capture_compacts_once();
    test_untriggered(4);
    test_untriggered(68);
    test_untriggered(CAPTURE_BUFFER_SAMPLES);
    for (unsigned pin = 0; pin < 8; ++pin) {
        test_edge((uint8_t)(1u << pin), true);
        test_edge((uint8_t)(1u << pin), false);
    }
    test_trigger_ignores_other_edges();
    test_reset_and_error();
    test_dma_error_at_completion();
    test_prescaled_sample_clock();
    test_invalid_dma_configuration(0, DMA_SxCR_PINC);
    test_invalid_dma_configuration(DMA_SxCR_PSIZE | DMA_SxCR_MSIZE, 0); /* byte */
    test_invalid_dma_configuration(DMA_SxCR_PSIZE | DMA_SxCR_MSIZE,
                                    DMA_SxCR_PSIZE_0 | DMA_SxCR_MSIZE_0); /* half-word */
    puts("ALL STM32 CAPTURE TESTS PASSED");
    return 0;
}
