/** @file
 * @brief Task/ISR boundary around the ST USB CDC class.
 *
 * BASEPRI protects class pointers and endpoint ownership from USB reset and
 * completion. Priority-1 capture interrupts remain enabled during transport
 * calls. Generated callbacks delegate here and contain no protocol logic.
 */
#include "usb_cdc_port.h"
#include "analyzer_runtime.h"
#include "FreeRTOS.h"
#include "task.h"
#include "stm32f4xx_hal.h"
#include "usbd_cdc.h"
#include "usbd_cdc_if.h"
#include "usbd_ctlreq.h"
#include "usbd_core.h"
#include <string.h>

#define CDC_LINE_CODING_BYTES 7U
#define ENDPOINT_NUMBER_MASK UINT8_C(0x0f)

extern USBD_HandleTypeDef hUsbDeviceFS;
extern uint8_t UserRxBufferFS[];
extern uint8_t UserTxBufferFS[];

static USBD_ClassTypeDef cdc_class;
static uint8_t line_coding[CDC_LINE_CODING_BYTES];

static bool valid_class_request(const USBD_SetupReqTypedef *request)
{
    bool valid = false;
    switch (request->bRequest) {
    case CDC_SET_LINE_CODING:
        valid = (request->wLength == CDC_LINE_CODING_BYTES) &&
                ((request->bmRequest & UINT8_C(0x80)) == 0U);
        break;
    case CDC_GET_LINE_CODING:
        valid = (request->wLength == CDC_LINE_CODING_BYTES) &&
                ((request->bmRequest & UINT8_C(0x80)) != 0U);
        break;
    case CDC_SET_CONTROL_LINE_STATE:
    case CDC_SEND_BREAK:
        valid = (request->wLength == 0U) &&
                ((request->bmRequest & UINT8_C(0x80)) == 0U);
        break;
    default:
        /* No UART or encapsulated-command service is implemented. */
        break;
    }
    return valid;
}

static uint8_t handle_setup(USBD_HandleTypeDef *device, USBD_SetupReqTypedef *request)
{
    if (((request->bmRequest & USB_REQ_TYPE_MASK) == USB_REQ_TYPE_CLASS) &&
        ((device->pClassData == NULL) || !valid_class_request(request))) {
        /* Reject lengths before the middleware uses them to address its fixed
         * control buffer. The interface callback runs too late for this check. */
        USBD_CtlError(device, request);
        return USBD_FAIL;
    }
    return USBD_CDC.Setup(device, request);
}

static uint8_t handle_data_in(USBD_HandleTypeDef *device, uint8_t endpoint)
{
    const USBD_CDC_HandleTypeDef *before = device->pClassData;
    const bool was_transmitting = (endpoint == (CDC_IN_EP & ENDPOINT_NUMBER_MASK)) &&
                                  (before != NULL) && (before->TxState != 0U);
    const uint8_t result = USBD_CDC.DataIn(device, endpoint);
    const USBD_CDC_HandleTypeDef *after = device->pClassData;

    /* A packet-size multiple requires a final ZLP. The middleware keeps
     * TxState busy until that packet finishes; storage remains borrowed. */
    if (was_transmitting && (result == USBD_OK) &&
        (after != NULL) && (after->TxState == 0U)) {
        analyzer_runtime_usb_tx_complete_from_isr();
    }
    return result;
}

void usb_cdc_port_init(void)
{
    static const uint8_t default_line_coding[CDC_LINE_CODING_BYTES] = {
        UINT8_C(0x00), UINT8_C(0xc2), UINT8_C(0x01), UINT8_C(0x00), /* 115200 */
        UINT8_C(0x00), UINT8_C(0x00), UINT8_C(0x08), /* one stop, no parity, 8 bits */
    };
    (void)memcpy(line_coding, default_line_coding, sizeof(line_coding));
    cdc_class = USBD_CDC;
    cdc_class.Setup = handle_setup;
    cdc_class.DataIn = handle_data_in;
}

USBD_ClassTypeDef *usb_cdc_port_class(void)
{
    return &cdc_class;
}

void usb_cdc_port_configure_irq(void)
{
    HAL_NVIC_SetPriority(OTG_FS_IRQn,
                         configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY + 1U, 0U);
    /* FreeRTOS initializes FromISR priority checks when the scheduler starts.
     * Host traffic may stay pending until the TX task explicitly enables it. */
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
    const USBD_CDC_HandleTypeDef *cdc = hUsbDeviceFS.pClassData;
    return (hUsbDeviceFS.dev_state == USBD_STATE_CONFIGURED) &&
           (cdc != NULL) && (cdc->TxState == 0U);
}

bool usb_cdc_port_idle(void *context)
{
    (void)context;
    taskENTER_CRITICAL();
    const bool idle = configured_and_idle();
    taskEXIT_CRITICAL();
    return idle;
}

bool usb_cdc_port_write(void *context, const uint8_t *bytes, size_t length)
{
    (void)context;
    if ((bytes == NULL) || (length == 0U) || (length > UINT16_MAX)) {
        return false;
    }

    taskENTER_CRITICAL();
    /* The ST API predates const-correct buffers. It reads this memory until
     * completion; it must never modify the caller's sample/response bytes. */
    const bool accepted = configured_and_idle() &&
        (CDC_Transmit_FS((uint8_t *)bytes, (uint16_t)length) == USBD_OK);
    taskEXIT_CRITICAL();
    return accepted;
}

bool usb_cdc_port_rearm(void *context)
{
    bool armed = false;
    (void)context;
    taskENTER_CRITICAL();
    if ((hUsbDeviceFS.dev_state == USBD_STATE_CONFIGURED) &&
        (hUsbDeviceFS.pClassData != NULL)) {
        armed = (USBD_CDC_SetRxBuffer(&hUsbDeviceFS, UserRxBufferFS) == USBD_OK) &&
                (USBD_CDC_ReceivePacket(&hUsbDeviceFS) == USBD_OK);
    }
    taskEXIT_CRITICAL();
    return armed;
}

USBD_StatusTypeDef usb_cdc_port_resume_from_isr(USBD_HandleTypeDef *device)
{
    const USBD_StatusTypeDef result = USBD_LL_Resume(device);
    analyzer_runtime_usb_resumed_from_isr();
    return result;
}

uint8_t usb_cdc_port_open_from_isr(void)
{
    (void)USBD_CDC_SetTxBuffer(&hUsbDeviceFS, UserTxBufferFS, 0U);
    (void)USBD_CDC_SetRxBuffer(&hUsbDeviceFS, UserRxBufferFS);
    analyzer_runtime_usb_connected_from_isr();
    return USBD_OK;
}

uint8_t usb_cdc_port_close_from_isr(void)
{
    analyzer_runtime_usb_disconnected_from_isr();
    return USBD_OK;
}

uint8_t usb_cdc_port_control_from_isr(uint8_t command, uint8_t *data, uint16_t length)
{
    switch (command) {
    case CDC_SET_LINE_CODING:
    case CDC_GET_LINE_CODING:
        if ((data == NULL) || (length != CDC_LINE_CODING_BYTES)) {
            return USBD_FAIL;
        }
        if (command == CDC_SET_LINE_CODING) {
            (void)memcpy(line_coding, data, sizeof(line_coding));
        } else {
            (void)memcpy(data, line_coding, sizeof(line_coding));
        }
        return USBD_OK;
    case CDC_SET_CONTROL_LINE_STATE:
    case CDC_SEND_BREAK:
        return USBD_OK;
    default:
        return USBD_FAIL;
    }
}

uint8_t usb_cdc_port_receive_from_isr(const uint8_t *data, uint32_t length)
{
    /* The runtime also receives malformed notifications so it can cancel the
     * incomplete parser session and rearm the endpoint in task context. */
    analyzer_runtime_usb_receive_from_isr(data, length);
    return ((data != NULL) && (length <= CDC_DATA_FS_OUT_PACKET_SIZE)) ?
           USBD_OK : USBD_FAIL;
}
