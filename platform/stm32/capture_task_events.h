#ifndef CAPTURE_TASK_EVENTS_H
#define CAPTURE_TASK_EVENTS_H

/* Install after the runtime tasks exist and before any capture can start.
 * TIM6/DAC is reserved as a software interrupt; its timer is not enabled. */
void capture_task_events_init(void);

/* Called by the TIM6/DAC vector at the FreeRTOS syscall priority boundary. */
void capture_task_events_irq(void);

#endif
