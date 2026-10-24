#include <stdint.h>
#include <stdlib.h>
extern uint32_t _estack, _sidata, _sdata, _edata, _sbss, _ebss;
extern void initialise_monitor_handles(void);
extern int main(void);
void Reset_Handler(void);
static void fault(void) { for (;;) {} }
#ifdef FREERTOS_QEMU_TEST
#include "freertos_support.h"
extern void SVC_Handler(void), PendSV_Handler(void);
#endif
#ifdef ANALYZER_RUNTIME_TEST
extern void ANALYZER_TEST_IRQHandler(void);
#endif
#ifdef RTOS_CAPTURE_TEST
extern void TIM6_DAC_IRQHandler(void);
#endif
#ifdef STM32_CAPTURE_TEST
extern void EXTI0_IRQHandler(void), EXTI1_IRQHandler(void), EXTI2_IRQHandler(void);
extern void EXTI3_IRQHandler(void), EXTI4_IRQHandler(void), EXTI9_5_IRQHandler(void);
extern void TIM8_UP_TIM13_IRQHandler(void), DMA2_Stream5_IRQHandler(void);
#endif
__attribute__((section(".vectors"), used))
const uintptr_t vectors[16 + 96] = {
    (uintptr_t)&_estack, (uintptr_t)Reset_Handler,
    (uintptr_t)fault, (uintptr_t)fault, (uintptr_t)fault,
    (uintptr_t)fault, (uintptr_t)fault,
#ifdef FREERTOS_QEMU_TEST
    [11] = (uintptr_t)SVC_Handler,
    [14] = (uintptr_t)PendSV_Handler,
    [15] = (uintptr_t)freertos_systick_handler,
#endif
#ifdef ANALYZER_RUNTIME_TEST
    [16] = (uintptr_t)ANALYZER_TEST_IRQHandler,
#endif
#ifdef RTOS_CAPTURE_TEST
    [16 + 54] = (uintptr_t)TIM6_DAC_IRQHandler,
#endif
#ifdef STM32_CAPTURE_TEST
    [16 + 6] = (uintptr_t)EXTI0_IRQHandler,
    [16 + 7] = (uintptr_t)EXTI1_IRQHandler,
    [16 + 8] = (uintptr_t)EXTI2_IRQHandler,
    [16 + 9] = (uintptr_t)EXTI3_IRQHandler,
    [16 + 10] = (uintptr_t)EXTI4_IRQHandler,
    [16 + 23] = (uintptr_t)EXTI9_5_IRQHandler,
    [16 + 44] = (uintptr_t)TIM8_UP_TIM13_IRQHandler,
    [16 + 68] = (uintptr_t)DMA2_Stream5_IRQHandler,
#endif
};
void Reset_Handler(void)
{
    uint32_t *source = &_sidata;
    for (uint32_t *dest = &_sdata; dest < &_edata;) *dest++ = *source++;
    for (uint32_t *dest = &_sbss; dest < &_ebss;) *dest++ = 0;
    initialise_monitor_handles();
    exit(main());
}
