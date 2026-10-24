#include "freertos_support.h"
#include "FreeRTOS.h"
#include "task.h"

#ifdef FREERTOS_QEMU_TEST
#include <stdio.h>
#include <stdlib.h>
#endif

/* Kernel-required idle storage is fixed at link time, like both user tasks. */
static StaticTask_t idle_task_control;
static StackType_t idle_task_stack[configMINIMAL_STACK_SIZE];

/* A debugger can inspect these after a fatal contract or stack violation. */
static const char *volatile failure_file;
static volatile uint32_t failure_line;

/** @brief Sleep until the next interrupt; no peripheral polling runs in idle. */
void vApplicationIdleHook(void)
{
    __asm volatile ("dsb\nwfi\nisb" ::: "memory");
}

/** @brief Supply the kernel's idle task with static control and stack storage. */
void vApplicationGetIdleTaskMemory(StaticTask_t **task, StackType_t **stack,
                                   configSTACK_DEPTH_TYPE *stack_words)
{
    *task = &idle_task_control;
    *stack = idle_task_stack;
    *stack_words = configMINIMAL_STACK_SIZE;
}

_Noreturn void freertos_assert_failed(const char *file, uint32_t line)
{
    /* Do not attempt USB, allocation, or scheduler services on a fault path. */
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

/** @brief Stop on stack corruption before another task can reuse its memory. */
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    (void)name;
    freertos_assert_failed("task stack overflow", 0u);
}

void freertos_systick_handler(void)
{
    extern void xPortSysTickHandler(void);
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        xPortSysTickHandler();
    }
}
