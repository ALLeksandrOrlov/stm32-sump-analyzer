#ifndef CAPTURE_TASK_EVENTS_H
#define CAPTURE_TASK_EVENTS_H

/**
 * @brief Reserve TIM6/DAC as the capture interrupt-to-task bridge.
 * @pre Runtime tasks exist and capture has not started.
 * @note TIM6 is never enabled as a timer. Capture ISRs only pend its interrupt;
 *       this lower-priority handler is permitted to call FreeRTOS FromISR APIs.
 */
void capture_task_events_init(void);

/** @brief Notify the application task from the syscall-safe TIM6/DAC ISR. */
void capture_task_events_irq(void);

#endif
