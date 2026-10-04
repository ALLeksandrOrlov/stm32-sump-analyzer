#ifndef CAPTURE_STM32_H
#define CAPTURE_STM32_H

#include "capture.h"

void capture_stm32_init(void);
capture_driver_t capture_stm32_driver(void);
uint32_t capture_stm32_timer_clock(void);
void capture_stm32_exti_irq(void);
void capture_stm32_timer_irq(void);
void capture_stm32_dma_irq(void);

#endif
