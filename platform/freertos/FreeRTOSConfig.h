#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <stdint.h>

/* The board and the QEMU capture machine run the Cortex-M4F at 168 MHz. */
#define configCPU_CLOCK_HZ                       168000000u
#define configTICK_RATE_HZ                       1000u
#define configTICK_TYPE_WIDTH_IN_BITS            TICK_TYPE_WIDTH_32_BITS
#define configUSE_PREEMPTION                     1
#define configUSE_TIME_SLICING                   1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION  1
#define configMAX_PRIORITIES                     5
#define configMINIMAL_STACK_SIZE                256u
#define configMAX_TASK_NAME_LEN                  16
#define configIDLE_SHOULD_YIELD                  1
#define configUSE_TICKLESS_IDLE                  0

/* All task control blocks, stacks and IPC storage belong to the application. */
#define configSUPPORT_STATIC_ALLOCATION          1
#define configSUPPORT_DYNAMIC_ALLOCATION         0
#define configKERNEL_PROVIDED_STATIC_MEMORY      0
#define configSTACK_DEPTH_TYPE                  uint32_t
#define configUSE_MINI_LIST_ITEM                 0
#define configENABLE_BACKWARD_COMPATIBILITY      0
#define configUSE_NEWLIB_REENTRANT               0

/* Message buffers use index 0; runtime wake-up events exclusively use index 1. */
#define configUSE_TASK_NOTIFICATIONS             1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES    2
#define configUSE_STREAM_BUFFERS                 1
#define configUSE_TIMERS                         0
#define configUSE_MUTEXES                        0
#define configUSE_RECURSIVE_MUTEXES              0
#define configUSE_COUNTING_SEMAPHORES            0
#define configUSE_QUEUE_SETS                     0
#define configQUEUE_REGISTRY_SIZE                0
#define configUSE_CO_ROUTINES                    0

#define configUSE_IDLE_HOOK                      1
#define configUSE_TICK_HOOK                      0
#define configUSE_MALLOC_FAILED_HOOK             0
#define configCHECK_FOR_STACK_OVERFLOW           2
#define configCHECK_HANDLER_INSTALLATION         1
#define configUSE_TRACE_FACILITY                 0
#define configGENERATE_RUN_TIME_STATS            0

/* Numerically smaller Cortex-M priorities are more urgent. IRQs above this
 * boundary must defer their work before calling any FreeRTOS FromISR API. */
#define configPRIO_BITS                          4
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY  15
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5
#define configKERNEL_INTERRUPT_PRIORITY \
    (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY \
    (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

/* The port owns these vector entries. SysTick is shared with the HAL through
 * freertos_systick_handler(), so it deliberately has no vector-name alias. */
#define vPortSVCHandler                          SVC_Handler
#define xPortPendSVHandler                       PendSV_Handler

#define INCLUDE_vTaskDelay                       1
#define INCLUDE_xTaskDelayUntil                  1
#define INCLUDE_vTaskSuspend                     1
#define INCLUDE_xTaskGetSchedulerState           1
#define INCLUDE_xTaskGetCurrentTaskHandle        1
#define INCLUDE_uxTaskGetStackHighWaterMark2      1
#define INCLUDE_eTaskGetState                    1
#define INCLUDE_vTaskDelete                      0
#define INCLUDE_vTaskPrioritySet                 0
#define INCLUDE_uxTaskPriorityGet                0

_Noreturn void freertos_assert_failed(const char *file, uint32_t line);
#define configASSERT(condition) \
    do { if (!(condition)) freertos_assert_failed(__FILE__, __LINE__); } while (0)

#endif
