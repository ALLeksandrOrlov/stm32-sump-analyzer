/** @file
 * @brief Defer priority-1 capture events to a syscall-safe interrupt.
 */
#include "capture_task_events.h"
#include "analyzer_runtime.h"
#include "capture_stm32.h"
#include "FreeRTOS.h"
#include "stm32f4xx_hal.h"

static void pend_task_notification(void)
{
    /* Priority-1 interrupts may preempt a FreeRTOS critical section. Pending
     * an NVIC interrupt is their only permitted scheduler interaction. */
    NVIC_SetPendingIRQ(TIM6_DAC_IRQn);
}

void capture_task_events_init(void)
{
    HAL_NVIC_SetPriority(TIM6_DAC_IRQn,
                         configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY, 0U);
    HAL_NVIC_ClearPendingIRQ(TIM6_DAC_IRQn);
    capture_stm32_set_event_callback(pend_task_notification);
    HAL_NVIC_EnableIRQ(TIM6_DAC_IRQn);
}

void capture_task_events_irq(void)
{
    analyzer_runtime_capture_event_from_isr();
}
