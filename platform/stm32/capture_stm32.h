#ifndef CAPTURE_STM32_H
#define CAPTURE_STM32_H

#include "capture.h"

/**
 * @brief Configure PD0..PD7, EXTI0..7, TIM1, TIM8 and DMA2 stream 5.
 * @pre Call once at startup, before starting application tasks or USB.
 * @note This module owns these peripherals. All driver operations run in the
 *       application task. Capture interrupts have priority 1 and cannot call
 *       FreeRTOS APIs, including their FromISR variants.
 */
void capture_stm32_init(void);

/**
 * @brief Install the interrupt-to-task wakeup bridge before a capture starts.
 * @param callback Called in a priority-1 ISR; NULL disables notification.
 * @note The callback may only pend a syscall-safe interrupt. It must not block,
 *       access capture buffers or call the scheduler.
 */
void capture_stm32_set_event_callback(void (*callback)(void));

/** @return Driver borrowing the module's static capture buffer until restart. */
capture_driver_t capture_stm32_driver(void);

/** @return TIM1/TIM8 input frequency, accounting for the APB2 timer multiplier. */
uint32_t capture_stm32_timer_clock_hz(void);

/** @brief Service selected EXTI edges; call from EXTI0..4 or EXTI9_5 ISR. */
void capture_stm32_exti_irq(void);

/** @brief Stop a triggered capture at the TIM8 update interrupt. */
void capture_stm32_tim8_update_irq(void);

/** @brief Service DMA completion and transfer faults from DMA2 stream 5 ISR. */
void capture_stm32_dma2_stream5_irq(void);

#endif
