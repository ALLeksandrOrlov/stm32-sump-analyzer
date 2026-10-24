#include "capture_task_events.h"
#include "analyzer_runtime.h"
#include "capture_stm32.h"
#include "FreeRTOS.h"
#include "stm32f4xx_hal.h"

static void defer_capture_event(void)
{
    /* Priority-1 capture IRQs may preempt FreeRTOS critical sections. Their
     * only scheduler interaction is pending this syscall-safe interrupt. */
    NVIC_SetPendingIRQ(TIM6_DAC_IRQn);
}

void capture_task_events_init(void)
{
    HAL_NVIC_SetPriority(TIM6_DAC_IRQn,
                         configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY, 0);
    HAL_NVIC_ClearPendingIRQ(TIM6_DAC_IRQn);
    capture_stm32_set_event_callback(defer_capture_event);
    HAL_NVIC_EnableIRQ(TIM6_DAC_IRQn);
}

void capture_task_events_irq(void)
{
    analyzer_runtime_capture_event_from_isr();
}
