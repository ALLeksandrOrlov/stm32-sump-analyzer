#include "capture_stm32.h"
#include "stm32f4xx_hal.h"
#include <string.h>

/* This adapter exclusively owns PD0..7, EXTI0..7, TIM1, TIM8, DMA2 stream 5. */
/* RM0090 section 8.4.5 requires word accesses to GPIO IDR. DMA writes whole
 * words; the application task compacts their low bytes after capture stops. */
static uint32_t sample_buffer[CAPTURE_BUFFER_SAMPLES];
static volatile capture_hw_event_t pending_event;
static volatile bool capture_active;
static volatile bool waiting_for_trigger;
static uint32_t sample_count;
static volatile uint32_t next_write_index;
static uint8_t trigger_mask;
static void (*event_callback)(void);

#define SAMPLE_PIN_MASK 0xffu
#define DMA_ERROR_FLAGS (DMA_HISR_TEIF5 | DMA_HISR_DMEIF5 | DMA_HISR_FEIF5)
#define DMA_CLEAR_FLAGS (DMA_HIFCR_CTCIF5 | DMA_HIFCR_CHTIF5 | DMA_HIFCR_CTEIF5 | \
                         DMA_HIFCR_CDMEIF5 | DMA_HIFCR_CFEIF5)

static const IRQn_Type exti_irqs[] = {
    EXTI0_IRQn, EXTI1_IRQn, EXTI2_IRQn, EXTI3_IRQn, EXTI4_IRQn, EXTI9_5_IRQn
};

static uint32_t disable_interrupts(void)
{
    uint32_t mask = __get_PRIMASK();
    __disable_irq();
    return mask;
}

static bool stop_hardware(void)
{
    capture_active = false;
    waiting_for_trigger = false;
    EXTI->IMR &= ~SAMPLE_PIN_MASK;
    TIM1->CR1 &= ~TIM_CR1_CEN;
    TIM1->DIER = 0;
    TIM8->CR1 &= ~TIM_CR1_CEN;
    TIM8->DIER = 0;
    DMA2_Stream5->CR &= ~DMA_SxCR_EN;
    /* EN clears after the last in-flight bus transfer. Never spin forever in
     * an ISR if the peripheral is faulty. */
    uint32_t timeout = 10000;
    while ((DMA2_Stream5->CR & DMA_SxCR_EN) && --timeout) {}
    DMA2_Stream5->CR &= ~(DMA_SxCR_TCIE | DMA_SxCR_TEIE | DMA_SxCR_DMEIE);
    __DSB();
    return timeout != 0;
}

static void stop(void *context)
{
    (void)context;
    uint32_t mask = disable_interrupts();
    stop_hardware();
    pending_event = CAPTURE_HW_NONE;
    __set_PRIMASK(mask);
}

static bool start(void *context, const capture_plan_t *plan)
{
    (void)context;
    stop(NULL);
    if (DMA2_Stream5->CR & DMA_SxCR_EN) return false;
    memset(sample_buffer, 0, sizeof(sample_buffer));
    uint32_t mask = disable_interrupts();
    sample_count = plan->config.samples;
    next_write_index = 0;
    trigger_mask = plan->config.trigger_mask;
    TIM1->CR1 = TIM_CR1_URS;
    TIM1->CR2 = 0;
    TIM1->SMCR = 0;
    TIM1->PSC = plan->prescaler;
    TIM1->ARR = plan->reload;
    TIM1->RCR = 0;
    TIM1->CNT = 0;
    TIM1->EGR = TIM_EGR_UG;
    TIM1->SR = 0;
    /* TIM1 update drives both GPIO DMA and the post-trigger sample counter.
     * RM0090: TIM8 ITR0 is TIM1 TRGO. Both timers therefore share the same
     * sampling period. */
    TIM1->CR2 = TIM_CR2_MMS_1;

    TIM8->CR1 = TIM_CR1_URS | TIM_CR1_OPM;
    TIM8->CR2 = 0;
    TIM8->SMCR = 0;
    TIM8->PSC = 0;
    TIM8->ARR = plan->config.post_trigger - 1u;
    TIM8->RCR = 0;
    TIM8->CNT = 0;
    TIM8->EGR = TIM_EGR_UG;
    TIM8->SR = 0;
    TIM8->SMCR = TIM_SMCR_SMS; /* external clock mode 1, TS=ITR0 */
    TIM8->DIER = TIM_DIER_UIE;

    DMA2->HIFCR = DMA_CLEAR_FLAGS;
    DMA2_Stream5->PAR = (uint32_t)&GPIOD->IDR;
    DMA2_Stream5->M0AR = (uint32_t)sample_buffer;
    DMA2_Stream5->NDTR = sample_count;
    DMA2_Stream5->FCR = 0; /* Direct words: one NDTR item is one GPIO sample. */
    DMA2_Stream5->CR = DMA_SxCR_CHSEL_1 | DMA_SxCR_CHSEL_2 |
        DMA_SxCR_PSIZE_1 | DMA_SxCR_MSIZE_1 | DMA_SxCR_MINC |
        DMA_SxCR_PL_1 | DMA_SxCR_TEIE | DMA_SxCR_DMEIE |
        (trigger_mask ? DMA_SxCR_CIRC : DMA_SxCR_TCIE);

    EXTI->RTSR = (EXTI->RTSR & ~SAMPLE_PIN_MASK) | (trigger_mask & plan->config.trigger_value);
    EXTI->FTSR = (EXTI->FTSR & ~SAMPLE_PIN_MASK) | (trigger_mask & ~plan->config.trigger_value);
    EXTI->PR = SAMPLE_PIN_MASK;
    HAL_NVIC_ClearPendingIRQ(TIM8_UP_TIM13_IRQn);
    HAL_NVIC_ClearPendingIRQ(DMA2_Stream5_IRQn);
    for (size_t i = 0; i < sizeof(exti_irqs) / sizeof(exti_irqs[0]); ++i)
        HAL_NVIC_ClearPendingIRQ(exti_irqs[i]);
    capture_active = true;
    waiting_for_trigger = trigger_mask != 0;
    pending_event = CAPTURE_HW_NONE;
    DMA2_Stream5->CR |= DMA_SxCR_EN;
    TIM1->DIER = TIM_DIER_UDE;
    TIM1->CR1 |= TIM_CR1_CEN;
    EXTI->IMR = (EXTI->IMR & ~SAMPLE_PIN_MASK) | trigger_mask;
    __set_PRIMASK(mask);
    return true;
}

static capture_hw_event_t poll(void *context)
{
    (void)context;
    uint32_t mask = disable_interrupts();
    capture_hw_event_t result = pending_event;
    pending_event = CAPTURE_HW_NONE;
    __set_PRIMASK(mask);
    if (result == CAPTURE_HW_COMPLETE) {
        /* This task owns start/stop/poll. The completion ISR has already
         * stopped DMA, so interrupts stay enabled during compaction. Forward
         * writes only replace words already read, preserving ring positions.
         * Consuming the event above ensures this happens exactly once. */
        uint8_t *packed = (uint8_t *)sample_buffer;
        for (uint32_t i = 0; i < sample_count; ++i) {
            packed[i] = (uint8_t)sample_buffer[i];
        }
    }
    return result;
}

static const uint8_t *buffer(void *context, uint32_t *next)
{
    (void)context;
    *next = next_write_index;
    return (const uint8_t *)sample_buffer;
}

void capture_stm32_init(void)
{
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    __HAL_RCC_DMA2_CLK_ENABLE();
    __HAL_RCC_TIM1_CLK_ENABLE();
    __HAL_RCC_TIM8_CLK_ENABLE();
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin = SAMPLE_PIN_MASK;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_PULLDOWN;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(GPIOD, &gpio);
    SYSCFG->EXTICR[0] = 0x3333;
    SYSCFG->EXTICR[1] = 0x3333;
    stop(NULL);
    for (size_t i = 0; i < sizeof(exti_irqs) / sizeof(exti_irqs[0]); ++i) {
        HAL_NVIC_SetPriority(exti_irqs[i], 1, 0);
        HAL_NVIC_EnableIRQ(exti_irqs[i]);
    }
    HAL_NVIC_SetPriority(TIM8_UP_TIM13_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(TIM8_UP_TIM13_IRQn);
    HAL_NVIC_SetPriority(DMA2_Stream5_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(DMA2_Stream5_IRQn);
}

uint32_t capture_stm32_timer_clock_hz(void)
{
    uint32_t clock = HAL_RCC_GetPCLK2Freq();
    /* PPRE2 encodings 0xx all mean divide by one. Timers double PCLK2 only
     * for the divided 1xx encodings. */
    return (RCC->CFGR & RCC_CFGR_PPRE2_2) ? 2u * clock : clock;
}

capture_driver_t capture_stm32_driver(void)
{
    return (capture_driver_t){NULL, start, stop, poll, buffer};
}

void capture_stm32_set_event_callback(void (*callback)(void))
{
    uint32_t mask = disable_interrupts();
    event_callback = callback;
    __set_PRIMASK(mask);
}

static void publish_event(capture_hw_event_t event)
{
    pending_event = event;
    __DMB();
    if (event_callback != NULL) {
        event_callback();
    }
}

void capture_stm32_exti_irq(void)
{
    uint32_t pending = EXTI->PR & EXTI->IMR & trigger_mask;
    EXTI->PR = SAMPLE_PIN_MASK;
    if (!capture_active || !waiting_for_trigger || !pending) return;
    EXTI->IMR &= ~SAMPLE_PIN_MASK;
    waiting_for_trigger = false;
    TIM8->CNT = 0;
    TIM8->SR = 0;
    TIM8->CR1 |= TIM_CR1_CEN;
    publish_event(CAPTURE_HW_TRIGGERED);
}

static void complete(void)
{
    bool stopped = stop_hardware();
    bool dma_failed = (DMA2->HISR & DMA_ERROR_FLAGS) != 0;
    next_write_index = sample_count - DMA2_Stream5->NDTR;
    __DMB();
    /* TIM8's IRQ can run before an already-pending DMA error IRQ. Inspect
     * status after the last transfer stops, before publishing the buffer. */
    publish_event(stopped && !dma_failed ? CAPTURE_HW_COMPLETE : CAPTURE_HW_ERROR);
}

void capture_stm32_tim8_update_irq(void)
{
    if (!(TIM8->SR & TIM_SR_UIF)) return;
    TIM8->SR = 0;
    if (capture_active) complete();
}

void capture_stm32_dma2_stream5_irq(void)
{
    uint32_t status = DMA2->HISR;
    DMA2->HIFCR = DMA_CLEAR_FLAGS;
    if (!capture_active) return;
    if (status & DMA_ERROR_FLAGS) {
        stop_hardware();
        publish_event(CAPTURE_HW_ERROR);
    } else if (!trigger_mask && (status & DMA_HISR_TCIF5)) complete();
}
