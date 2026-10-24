/** @file
 * @brief Composition root for the capture driver, USB port and runtime.
 */
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
    const analyzer_usb_port_t usb_port = {
        .context = NULL,
        .activate = usb_cdc_port_activate,
        .write = usb_cdc_port_write,
        .idle = usb_cdc_port_idle,
        .rearm = usb_cdc_port_rearm,
    };

    const uint32_t previous_basepri = __get_BASEPRI();
    const bool initialized = analyzer_runtime_init(capture_stm32_driver(), usb_port,
                                                  capture_stm32_timer_clock_hz());
    /* Creating tasks before scheduler startup raises BASEPRI. Restore its
     * caller-owned value so subsequent HAL initialization can use SysTick. */
    __set_BASEPRI(previous_basepri);
    __DSB();
    __ISB();
    if (initialized) {
        capture_task_events_init();
    }
    return initialized;
}

void analyzer_board_start(void)
{
    vTaskStartScheduler();
    Error_Handler();
}
