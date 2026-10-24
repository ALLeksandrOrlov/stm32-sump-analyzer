#include "freertos_support.h"
#include "FreeRTOS.h"
#include "task.h"

#ifdef FREERTOS_QEMU_TEST
#include <stdio.h>
#include <stdlib.h>
#endif

static StaticTask_t idle_task;
static StackType_t idle_stack[configMINIMAL_STACK_SIZE];

void vApplicationIdleHook(void)
{
    /* There is no polling work in idle. SysTick or a peripheral IRQ wakes the
     * core; all application and transmitter waits use task notifications. */
    __asm volatile ("dsb\nwfi\nisb" ::: "memory");
}

/* Retain the fault location for a debugger without using allocation or USB. */
static const char *volatile failure_file;
static volatile uint32_t failure_line;

void vApplicationGetIdleTaskMemory(StaticTask_t **task, StackType_t **stack,
                                   configSTACK_DEPTH_TYPE *stack_words)
{
    *task = &idle_task;
    *stack = idle_stack;
    *stack_words = configMINIMAL_STACK_SIZE;
}

_Noreturn void freertos_assert_failed(const char *file, uint32_t line)
{
    __asm volatile ("cpsid i" ::: "memory");
    failure_file = file;
    failure_line = line;
#ifdef FREERTOS_QEMU_TEST
    fprintf(stderr, "FreeRTOS assertion: %s:%lu\n", file, (unsigned long)line);
    exit(EXIT_FAILURE);
#else
    for (;;) {
        __asm volatile ("wfi");
    }
#endif
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    (void)name;
    freertos_assert_failed("task stack overflow", 0);
}

void freertos_systick_handler(void)
{
    extern void xPortSysTickHandler(void);
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        xPortSysTickHandler();
    }
}
