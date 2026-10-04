/* Runs the production STM32 adapter, CMSIS and HAL GPIO/NVIC code on the
 * extended QEMU machine. Only clock discovery and the USB transport are fake. */
#include "analyzer_app.h"
#include "capture_stm32.h"
#include "stm32f4xx_hal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define FIXTURE ((volatile uint32_t *)0x60000000u)
static analyzer_app_t app;
static uint8_t wire[CAPTURE_BUFFER_SAMPLES];
static size_t wire_length;
uint32_t SystemCoreClock = 168000000;
/* The model has a fixed timer clock. PLL/RCC startup is outside these tests. */
uint32_t HAL_RCC_GetPCLK2Freq(void) { return 168000000; }

void EXTI0_IRQHandler(void) { capture_stm32_exti_irq(); }
void EXTI1_IRQHandler(void) { capture_stm32_exti_irq(); }
void EXTI2_IRQHandler(void) { capture_stm32_exti_irq(); }
void EXTI3_IRQHandler(void) { capture_stm32_exti_irq(); }
void EXTI4_IRQHandler(void) { capture_stm32_exti_irq(); }
void EXTI9_5_IRQHandler(void) { capture_stm32_exti_irq(); }
void TIM8_UP_TIM13_IRQHandler(void) { capture_stm32_tim8_update_irq(); }
void DMA2_Stream5_IRQHandler(void) { capture_stm32_dma2_stream5_irq(); }
static bool ready(void *ctx) { (void)ctx; return true; }
static bool send_bytes(void *ctx, const uint8_t *data, size_t size)
{
    (void)ctx;
    assert(wire_length + size <= sizeof(wire));
    memcpy(wire + wire_length, data, size);
    wire_length += size;
    return true;
}
static void command(uint8_t opcode, uint32_t value)
{
    assert(analyzer_app_receive(&app, opcode));
    if (opcode & 0x80)
        for (unsigned i = 0; i < 4; ++i)
            assert(analyzer_app_receive(&app, (uint8_t)(value >> (8 * i))));
}
static void setup(uint32_t count, uint8_t mask, uint8_t value)
{
    analyzer_app_reset(&app);
    wire_length = 0;
    FIXTURE[3] = 0;
    FIXTURE[1] = 0;
    FIXTURE[2] = 1;
    FIXTURE[4] = 0;
    command(SUMP_DIVIDER, 999); /* 100 kHz, IRQ latency well below one sample. */
    command(SUMP_COUNTS, ((count / 4 - 1) << 16) | (count / 4 - 1));
    command(SUMP_TRIGGER_MASK, mask);
    command(SUMP_TRIGGER_VALUE, value);
}
static void wait_complete(void)
{
    for (unsigned i = 0; i < 50000000; ++i) {
        analyzer_app_poll(&app);
        if (app.capture.state == CAPTURE_IDLE) return;
        assert(app.capture.state != CAPTURE_ERROR);
    }
    assert(!"capture timed out");
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
    FIXTURE[3] = 1;
    command(SUMP_RUN, 0);
    wait_complete();
    assert(wire_length == count);
    for (uint32_t i = 0; i < count; ++i) assert(wire[i] == (uint8_t)(count - i));
    assert_stopped();
}
static void test_edge(bool rising)
{
    setup(64, 0x80, rising ? 0x80 : 0);
    FIXTURE[1] = rising ? 0 : 0x80;
    command(SUMP_COUNTS, (3u << 16) | 15u); /* 64 total, 16 post-trigger. */
    command(SUMP_RUN, 0);
    assert(app.capture.state == CAPTURE_ARMED);
    while (FIXTURE[4] < 80) analyzer_app_poll(&app); /* wrap the DMA ring */
    assert(app.capture.state == CAPTURE_ARMED);
    FIXTURE[1] = rising ? 0x80 : 0;
    wait_complete();
    assert(wire_length == 64);
    /* The newest post-trigger samples have the new level. Older pre-trigger
     * samples remain at the old level; this exercises actual circular DMA. */
    unsigned post = 0;
    while (post < 64 && wire[post] == (rising ? 0x80 : 0)) ++post;
    assert(post >= 16 && post <= 17); /* one sample tolerance for IRQ phase */
    for (unsigned i = post; i < 64; ++i) assert(wire[i] == (rising ? 0 : 0x80));
    assert_stopped();
}
static void test_reset_and_error(void)
{
    setup(64, 1, 1);
    command(SUMP_RUN, 0);
    while (FIXTURE[4] < 8) analyzer_app_poll(&app);
    command(SUMP_RESET, 0);
    assert_stopped();
    assert(app.capture.state == CAPTURE_IDLE && wire_length == 0);
    setup(64, 0, 0);
    FIXTURE[5] = 1;
    command(SUMP_RUN, 0);
    for (unsigned i = 0; i < 1000000 && app.capture.state != CAPTURE_ERROR; ++i)
        analyzer_app_poll(&app);
    assert(app.capture.state == CAPTURE_ERROR);
    assert_stopped();
    assert(wire_length == 0);
    test_untriggered(4); /* reset recovers from DMA error */
}
static void test_gpio(void)
{
    FIXTURE[1] = 0x5a;
    assert((GPIOD->IDR & 0xff) == 0x5a);
    GPIOD->IDR = 0; /* read-only */
    assert((GPIOD->IDR & 0xff) == 0x5a);
    GPIOD->ODR = 0;
    GPIOD->BSRR = 3;
    assert(GPIOD->ODR == 3);
    GPIOD->BSRR = 1u << 16;
    assert(GPIOD->ODR == 2);
}
int main(void)
{
    assert(FIXTURE[0] == 0x534c4131);
    capture_stm32_init();
    analyzer_app_init(&app, capture_stm32_driver(),
                      (analyzer_transport_t){NULL, ready, send_bytes},
                      capture_stm32_timer_clock_hz());
    test_gpio();
    test_untriggered(4);
    test_untriggered(68);
    test_untriggered(CAPTURE_BUFFER_SAMPLES);
    test_edge(true);
    test_edge(false);
    test_reset_and_error();
    puts("ALL STM32 CAPTURE TESTS PASSED");
    return 0;
}
