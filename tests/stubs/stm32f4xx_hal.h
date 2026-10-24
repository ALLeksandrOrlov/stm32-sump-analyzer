#ifndef TEST_STM32F4XX_HAL_H
#define TEST_STM32F4XX_HAL_H

#include <stdint.h>
typedef enum { OTG_FS_IRQn = 67 } IRQn_Type;
void HAL_NVIC_EnableIRQ(IRQn_Type interrupt);
void HAL_NVIC_DisableIRQ(IRQn_Type interrupt);
void HAL_NVIC_SetPriority(IRQn_Type interrupt, uint32_t priority, uint32_t subpriority);

#endif
