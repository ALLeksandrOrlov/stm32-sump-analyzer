/** @file
 * @brief GPIO sampling through TIM1, DMA2 and a TIM8 post-trigger counter.
 *
 * The application task owns start/stop/poll. Priority-1 interrupts stop the
 * peripherals and publish one event; they never call the scheduler. A terminal
 * event may replace an unconsumed trigger event because it describes the final
 * hardware state. Buffer packing runs only after DMA has stopped.
 */
#include "capture_stm32.h"
#include "stm32f4xx_hal.h"
#include <string.h>

#define SAMPLE_PIN_MASK UINT32_C(0xff)
#define CAPTURE_IRQ_PRIORITY UINT32_C(1)
#define DMA_STOP_POLL_LIMIT UINT32_C(10000)
#define DMA_ERROR_FLAGS (DMA_HISR_TEIF5 | DMA_HISR_DMEIF5 | DMA_HISR_FEIF5)
#define DMA_CLEAR_FLAGS (DMA_HIFCR_CTCIF5 | DMA_HIFCR_CHTIF5 | DMA_HIFCR_CTEIF5 | \
                         DMA_HIFCR_CDMEIF5 | DMA_HIFCR_CFEIF5)

/* RM0090 requires word reads of GPIO IDR. One DMA item is one sample, even
 * though only the low eight bits are exposed to the analyzer. */
static uint32_t sample_words[CAPTURE_BUFFER_SAMPLES];
static uint32_t sample_count;
static uint32_t selected_trigger;
static volatile uint32_t next_write_index;
static volatile capture_hw_event_t pending_event;
static volatile bool capture_active;
static volatile bool waiting_for_trigger;
static void (*event_callback)(void);

static const IRQn_Type trigger_interrupts[] = {
    EXTI0_IRQn, EXTI1_IRQn, EXTI2_IRQn, EXTI3_IRQn, EXTI4_IRQn, EXTI9_5_IRQn
};

static uint32_t lock_capture_interrupts(void)
{
    const uint32_t previous_mask = __get_PRIMASK();
    __disable_irq();
    return previous_mask;
}

/** Stop request generation before waiting for the final DMA bus transaction. */
static bool stop_peripherals(void)
{
    capture_active = false;
    waiting_for_trigger = false;
    EXTI->IMR &= ~SAMPLE_PIN_MASK;
    TIM1->CR1 &= ~TIM_CR1_CEN;
    TIM1->DIER = 0U;
    TIM8->CR1 &= ~TIM_CR1_CEN;
    TIM8->DIER = 0U;
    DMA2_Stream5->CR &= ~DMA_SxCR_EN;

    uint32_t remaining_polls = DMA_STOP_POLL_LIMIT;
    while (((DMA2_Stream5->CR & DMA_SxCR_EN) != 0U) && (remaining_polls > 0U)) {
        --remaining_polls;
    }
    DMA2_Stream5->CR &= ~(DMA_SxCR_TCIE | DMA_SxCR_TEIE | DMA_SxCR_DMEIE);
    __DSB();
    return (DMA2_Stream5->CR & DMA_SxCR_EN) == 0U;
}

static void stop_capture(void *context)
{
    (void)context;
    const uint32_t previous_mask = lock_capture_interrupts();
    (void)stop_peripherals();
    pending_event = CAPTURE_HW_NONE;
    __set_PRIMASK(previous_mask);
}

static bool plan_fits_peripherals(const capture_plan_t *plan)
{
    if (plan == NULL) {
        return false;
    }

    const capture_config_t *config = &plan->config;
    const uint32_t trigger = config->trigger_mask;
    const bool single_channel = (trigger == 0U) || ((trigger & (trigger - 1U)) == 0U);
    return (config->samples > 0U) && (config->samples <= CAPTURE_BUFFER_SAMPLES) &&
           (config->post_trigger > 0U) && (config->post_trigger <= config->samples) &&
           ((trigger & ~SAMPLE_PIN_MASK) == 0U) && single_channel;
}

static void configure_sample_timer(const capture_plan_t *plan)
{
    TIM1->CR1 = TIM_CR1_URS;
    TIM1->CR2 = 0U;
    TIM1->SMCR = 0U;
    TIM1->PSC = plan->prescaler;
    TIM1->ARR = plan->reload;
    TIM1->RCR = 0U;
    TIM1->CNT = 0U;
    TIM1->EGR = TIM_EGR_UG;
    TIM1->SR = 0U;

    /* TIM1 update drives both the GPIO DMA request and TIM8 ITR0. */
    TIM1->CR2 = TIM_CR2_MMS_1;
}

static void configure_post_trigger_timer(uint32_t post_trigger_samples)
{
    TIM8->CR1 = TIM_CR1_URS | TIM_CR1_OPM;
    TIM8->CR2 = 0U;
    TIM8->SMCR = 0U;
    TIM8->PSC = 0U;
    TIM8->ARR = post_trigger_samples - 1U;
    TIM8->RCR = 0U;
    TIM8->CNT = 0U;
    TIM8->EGR = TIM_EGR_UG;
    TIM8->SR = 0U;
    TIM8->SMCR = TIM_SMCR_SMS; /* External clock mode 1, source ITR0. */
    TIM8->DIER = TIM_DIER_UIE;
}

static void configure_sample_dma(void)
{
    DMA2->HIFCR = DMA_CLEAR_FLAGS;
    DMA2_Stream5->PAR = (uint32_t)(uintptr_t)&GPIOD->IDR;
    DMA2_Stream5->M0AR = (uint32_t)(uintptr_t)sample_words;
    DMA2_Stream5->NDTR = sample_count;
    DMA2_Stream5->FCR = 0U;
    DMA2_Stream5->CR = DMA_SxCR_CHSEL_1 | DMA_SxCR_CHSEL_2 |
        DMA_SxCR_PSIZE_1 | DMA_SxCR_MSIZE_1 | DMA_SxCR_MINC |
        DMA_SxCR_PL_1 | DMA_SxCR_TEIE | DMA_SxCR_DMEIE |
        ((selected_trigger != 0U) ? DMA_SxCR_CIRC : DMA_SxCR_TCIE);
}

static void configure_trigger_edges(uint8_t trigger_value)
{
    const uint32_t rising_edges = selected_trigger & (uint32_t)trigger_value;
    const uint32_t falling_edges = selected_trigger & ~(uint32_t)trigger_value;
    EXTI->RTSR = (EXTI->RTSR & ~SAMPLE_PIN_MASK) | rising_edges;
    EXTI->FTSR = (EXTI->FTSR & ~SAMPLE_PIN_MASK) | falling_edges;
    EXTI->PR = SAMPLE_PIN_MASK;
    HAL_NVIC_ClearPendingIRQ(TIM8_UP_TIM13_IRQn);
    HAL_NVIC_ClearPendingIRQ(DMA2_Stream5_IRQn);
    for (size_t index = 0U; index < (sizeof(trigger_interrupts) / sizeof(trigger_interrupts[0])); ++index) {
        HAL_NVIC_ClearPendingIRQ(trigger_interrupts[index]);
    }
}

static bool start_capture(void *context, const capture_plan_t *plan)
{
    stop_capture(context);
    if (!plan_fits_peripherals(plan) || ((DMA2_Stream5->CR & DMA_SxCR_EN) != 0U)) {
        return false;
    }

    /* Early triggers may precede a full ring: unwritten history is zero. This
     * task-only operation does not need to mask USB or capture interrupts. */
    (void)memset(sample_words, 0, sizeof(sample_words));
    const uint32_t previous_mask = lock_capture_interrupts();
    sample_count = plan->config.samples;
    selected_trigger = plan->config.trigger_mask;
    next_write_index = 0U;
    configure_sample_timer(plan);
    configure_post_trigger_timer(plan->config.post_trigger);
    configure_sample_dma();
    configure_trigger_edges(plan->config.trigger_value);

    capture_active = true;
    waiting_for_trigger = selected_trigger != 0U;
    pending_event = CAPTURE_HW_NONE;
    DMA2_Stream5->CR |= DMA_SxCR_EN;
    TIM1->DIER = TIM_DIER_UDE;
    TIM1->CR1 |= TIM_CR1_CEN;
    EXTI->IMR = (EXTI->IMR & ~SAMPLE_PIN_MASK) | selected_trigger;
    __set_PRIMASK(previous_mask);
    return true;
}

static capture_hw_event_t poll_capture(void *context)
{
    (void)context;
    const uint32_t previous_mask = lock_capture_interrupts();
    const capture_hw_event_t event = pending_event;
    pending_event = CAPTURE_HW_NONE;
    __set_PRIMASK(previous_mask);

    if (event == CAPTURE_HW_COMPLETE) {
        /* Access through uint8_t is the deliberate object-representation
         * conversion. Forward packing overwrites only words already read. */
        uint8_t *packed_samples = (uint8_t *)sample_words;
        for (uint32_t index = 0U; index < sample_count; ++index) {
            packed_samples[index] = (uint8_t)sample_words[index];
        }
    }
    return event;
}

static const uint8_t *capture_buffer(void *context, uint32_t *next)
{
    (void)context;
    if (next != NULL) {
        *next = next_write_index;
    }
    return (const uint8_t *)sample_words;
}

static void enable_capture_interrupt(IRQn_Type interrupt)
{
    HAL_NVIC_ClearPendingIRQ(interrupt);
    HAL_NVIC_SetPriority(interrupt, CAPTURE_IRQ_PRIORITY, 0U);
    HAL_NVIC_EnableIRQ(interrupt);
}

void capture_stm32_init(void)
{
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    __HAL_RCC_DMA2_CLK_ENABLE();
    __HAL_RCC_TIM1_CLK_ENABLE();
    __HAL_RCC_TIM8_CLK_ENABLE();

    GPIO_InitTypeDef inputs = {0};
    inputs.Pin = SAMPLE_PIN_MASK;
    inputs.Mode = GPIO_MODE_INPUT;
    inputs.Pull = GPIO_PULLDOWN;
    inputs.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(GPIOD, &inputs);
    SYSCFG->EXTICR[0] = UINT32_C(0x3333);
    SYSCFG->EXTICR[1] = UINT32_C(0x3333);
    stop_capture(NULL);
    for (size_t index = 0U; index < (sizeof(trigger_interrupts) / sizeof(trigger_interrupts[0])); ++index) {
        enable_capture_interrupt(trigger_interrupts[index]);
    }
    enable_capture_interrupt(TIM8_UP_TIM13_IRQn);
    enable_capture_interrupt(DMA2_Stream5_IRQn);
}

uint32_t capture_stm32_timer_clock_hz(void)
{
    const uint32_t peripheral_clock = HAL_RCC_GetPCLK2Freq();
    /* PPRE2 0xx means divide by one; the timer doubles divided 1xx clocks. */
    return ((RCC->CFGR & RCC_CFGR_PPRE2_2) != 0U) ?
           (2U * peripheral_clock) : peripheral_clock;
}

capture_driver_t capture_stm32_driver(void)
{
    const capture_driver_t driver = {
        .context = NULL,
        .start = start_capture,
        .stop = stop_capture,
        .poll = poll_capture,
        .buffer = capture_buffer,
    };
    return driver;
}

void capture_stm32_set_event_callback(void (*callback)(void))
{
    const uint32_t previous_mask = lock_capture_interrupts();
    event_callback = callback;
    __set_PRIMASK(previous_mask);
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
    const uint32_t edges = EXTI->PR & EXTI->IMR & selected_trigger;
    EXTI->PR = SAMPLE_PIN_MASK;
    if (capture_active && waiting_for_trigger && (edges != 0U)) {
        EXTI->IMR &= ~SAMPLE_PIN_MASK;
        waiting_for_trigger = false;
        TIM8->CNT = 0U;
        TIM8->SR = 0U;
        TIM8->CR1 |= TIM_CR1_CEN;
        publish_event(CAPTURE_HW_TRIGGERED);
    }
}

static void finish_capture(void)
{
    const bool stopped = stop_peripherals();
    const bool dma_failed = (DMA2->HISR & DMA_ERROR_FLAGS) != 0U;
    const uint32_t remaining = DMA2_Stream5->NDTR;
    if (stopped && !dma_failed && (remaining <= sample_count)) {
        next_write_index = sample_count - remaining;
        __DMB();
        publish_event(CAPTURE_HW_COMPLETE);
    } else {
        publish_event(CAPTURE_HW_ERROR);
    }
}

void capture_stm32_tim8_update_irq(void)
{
    if ((TIM8->SR & TIM_SR_UIF) != 0U) {
        TIM8->SR = 0U;
        if (capture_active) {
            /* TIM8 can preempt a pending DMA error handler. Inspect DMA
             * status after stopping before exposing any samples. */
            finish_capture();
        }
    }
}

void capture_stm32_dma2_stream5_irq(void)
{
    const uint32_t status = DMA2->HISR;
    DMA2->HIFCR = DMA_CLEAR_FLAGS;
    if (capture_active) {
        if ((status & DMA_ERROR_FLAGS) != 0U) {
            (void)stop_peripherals();
            publish_event(CAPTURE_HW_ERROR);
        } else if ((selected_trigger == 0U) && ((status & DMA_HISR_TCIF5) != 0U)) {
            finish_capture();
        } else {
            /* Circular DMA completion does not end a triggered capture. */
        }
    }
}
