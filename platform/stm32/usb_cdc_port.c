#include "usb_cdc_port.h"
#include "analyzer_runtime.h"
#include "FreeRTOS.h"
#include "task.h"
#include "stm32f4xx_hal.h"
#include "usbd_cdc.h"
#include "usbd_cdc_if.h"

extern USBD_HandleTypeDef hUsbDeviceFS;
extern uint8_t UserRxBufferFS[];

static USBD_ClassTypeDef cdc_class;

static uint8_t cdc_data_in(USBD_HandleTypeDef *device, uint8_t endpoint)
{
    USBD_CDC_HandleTypeDef *cdc = device->pClassData;
    bool completing = endpoint == (CDC_IN_EP & 0x0fu) &&
                      cdc != NULL && cdc->TxState != 0;
    uint8_t result = USBD_CDC.DataIn(device, endpoint);

    /* A transfer whose length is a multiple of the endpoint packet size sends
     * a ZLP after its data. TxState stays busy until that ZLP has completed. */
    cdc = device->pClassData;
    if (completing && result == USBD_OK && cdc != NULL && cdc->TxState == 0) {
        analyzer_runtime_usb_tx_complete_from_isr();
    }
    return result;
}

void usb_cdc_port_init(void)
{
    cdc_class = USBD_CDC;
    cdc_class.DataIn = cdc_data_in;
}

USBD_ClassTypeDef *usb_cdc_port_class(void)
{
    return &cdc_class;
}

void usb_cdc_port_configure_irq(void)
{
    /* USB may call FreeRTOS FromISR APIs. Capture IRQs remain at priority 1. */
    HAL_NVIC_SetPriority(OTG_FS_IRQn,
                         configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY + 1u, 0);
    /* The port's FromISR priority validation is initialized by scheduler
     * startup. Keep host traffic pending until the TX task enables this IRQ. */
    HAL_NVIC_DisableIRQ(OTG_FS_IRQn);
}

void usb_cdc_port_activate(void *context)
{
    (void)context;
    configASSERT(xTaskGetSchedulerState() == taskSCHEDULER_RUNNING);
    HAL_NVIC_EnableIRQ(OTG_FS_IRQn);
}

static bool configured_and_idle(void)
{
    USBD_CDC_HandleTypeDef *cdc = hUsbDeviceFS.pClassData;
    return hUsbDeviceFS.dev_state == USBD_STATE_CONFIGURED &&
           cdc != NULL && cdc->TxState == 0;
}

bool usb_cdc_port_idle(void *context)
{
    (void)context;
    taskENTER_CRITICAL();
    bool idle = configured_and_idle();
    taskEXIT_CRITICAL();
    return idle;
}

bool usb_cdc_port_write(void *context, const uint8_t *bytes, size_t length)
{
    (void)context;
    if (bytes == NULL || length == 0 || length > UINT16_MAX) {
        return false;
    }

    /* BASEPRI excludes USB reset/completion while the class takes ownership.
     * Priority-1 capture interrupts stay enabled throughout this operation. */
    taskENTER_CRITICAL();
    bool accepted = configured_and_idle() &&
                    CDC_Transmit_FS((uint8_t *)bytes, (uint16_t)length) == USBD_OK;
    taskEXIT_CRITICAL();
    return accepted;
}

void usb_cdc_port_rearm(void *context)
{
    (void)context;
    taskENTER_CRITICAL();
    if (hUsbDeviceFS.dev_state == USBD_STATE_CONFIGURED &&
        hUsbDeviceFS.pClassData != NULL) {
        USBD_CDC_SetRxBuffer(&hUsbDeviceFS, UserRxBufferFS);
        USBD_CDC_ReceivePacket(&hUsbDeviceFS);
    }
    taskEXIT_CRITICAL();
}
