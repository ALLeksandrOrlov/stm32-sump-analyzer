#ifndef CAPTURE_STM32_H
#define CAPTURE_STM32_H

#include "capture.h"

/* Initialize before enabling USB. The driver owns PD0..7, EXTI0..7, TIM1,
 * TIM8 and DMA2 stream 5. Driver callbacks run in task context. Capture IRQs
 * run at priority 1 and must never call FreeRTOS APIs directly. */
void capture_stm32_init(void);
/* Optional wakeup hook, installed before starting a capture. It runs in the
 * priority-1 capture ISR and may only pend a lower-priority interrupt. */
void capture_stm32_set_event_callback(void (*callback)(void));
capture_driver_t capture_stm32_driver(void);
uint32_t capture_stm32_timer_clock_hz(void);
void capture_stm32_exti_irq(void);
void capture_stm32_tim8_update_irq(void);
void capture_stm32_dma2_stream5_irq(void);

#endif
