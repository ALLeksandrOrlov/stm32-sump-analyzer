#ifndef TEST_FREERTOS_H
#define TEST_FREERTOS_H

#include <assert.h>
typedef int BaseType_t;
#define configASSERT(condition) assert(condition)

#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5u
#define configPRIO_BITS 4u

#endif
