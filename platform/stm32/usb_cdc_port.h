#ifndef USB_CDC_PORT_H
#define USB_CDC_PORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "usbd_def.h"

/* Prepare the CDC class adapter before MX_USB_DEVICE_Init(). Its DataIn hook
 * reports completion after the final packet, including a terminating ZLP. */
void usb_cdc_port_init(void);
USBD_ClassTypeDef *usb_cdc_port_class(void);
/* Configure while USB is initialized, but leave its IRQ masked until the TX
 * task activates it after the FreeRTOS interrupt-priority checks are ready. */
void usb_cdc_port_configure_irq(void);
void usb_cdc_port_activate(void *context);

/* Task-only transport operations. The buffer passed to write remains borrowed
 * until TX completion or disconnect. False leaves ownership with the caller.
 * idle is true only while configured and able to accept a new transfer.
 * The context argument is unused; it matches analyzer_usb_port_t callbacks. */
bool usb_cdc_port_write(void *context, const uint8_t *bytes, size_t length);
bool usb_cdc_port_idle(void *context);
void usb_cdc_port_rearm(void *context);

#endif
