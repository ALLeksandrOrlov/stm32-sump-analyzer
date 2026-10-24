#ifndef FREERTOS_SUPPORT_H
#define FREERTOS_SUPPORT_H

/**
 * @brief Advance the RTOS tick after the HAL tick from the SysTick handler.
 *
 * Before vTaskStartScheduler(), this is a no-op. The scheduler owns SysTick
 * configuration and the SVC/PendSV handlers once it starts.
 */
void freertos_systick_handler(void);

#endif
