#ifndef FREERTOS_SUPPORT_H
#define FREERTOS_SUPPORT_H

/* Called by SysTick after the HAL tick. Safe before vTaskStartScheduler(). */
void freertos_systick_handler(void);

#endif
