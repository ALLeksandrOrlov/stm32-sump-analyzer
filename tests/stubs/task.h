#ifndef TEST_TASK_H
#define TEST_TASK_H

#include "FreeRTOS.h"
#define taskSCHEDULER_NOT_STARTED 1
#define taskSCHEDULER_RUNNING 2
BaseType_t xTaskGetSchedulerState(void);

void test_enter_critical(void);
void test_exit_critical(void);
#define taskENTER_CRITICAL() test_enter_critical()
#define taskEXIT_CRITICAL() test_exit_critical()

#endif
