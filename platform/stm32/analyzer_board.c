#include "analyzer_board.h"
#include "analyzer_runtime.h"
#include "capture_stm32.h"
#include "capture_task_events.h"
#include "usb_cdc_port.h"
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"

bool analyzer_board_init(void)
{
    capture_stm32_init();
    usb_cdc_port_init();
    analyzer_usb_port_t usb = {
        .context = NULL,
        .activate = usb_cdc_port_activate,
        .write = usb_cdc_port_write,
        .idle = usb_cdc_port_idle,
        .rearm = usb_cdc_port_rearm,
    };
    uint32_t initial_basepri = __get_BASEPRI();
    bool initialized = analyzer_runtime_init(capture_stm32_driver(), usb,
                                             capture_stm32_timer_clock_hz());
    /* Task creation before scheduler start leaves the port's interrupt mask
     * raised. USB hardware initialization needs the HAL SysTick for delays. */
    __set_BASEPRI(initial_basepri);
    __DSB();
    __ISB();
    if (!initialized) {
        return false;
    }

    capture_task_events_init();
    return true;
}

void analyzer_board_start(void)
{
    vTaskStartScheduler();
    Error_Handler();
}
