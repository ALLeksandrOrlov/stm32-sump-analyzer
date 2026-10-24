#ifndef USB_MESSAGE_TRANSPORT_H
#define USB_MESSAGE_TRANSPORT_H

#include "analyzer_runtime.h"
#include "FreeRTOS.h"
#include "task.h"

#define USB_MESSAGE_BYTES 64u
#define ANALYZER_NOTIFICATION_INDEX 1u

typedef struct {
    uint32_t session;
    uint8_t bytes[USB_MESSAGE_BYTES];
} usb_rx_message_t;

/* The RX buffer has one writer (USB IRQ) and one reader (application task).
 * The TX buffer has one writer (application task) and one reader (TX task).
 * Neither live buffer is ever reset from another execution context. */
void usb_messages_init(analyzer_usb_port_t port);
void usb_messages_bind_tasks(TaskHandle_t application, TaskHandle_t transmitter);
uint32_t usb_messages_session(void);
size_t usb_messages_receive(usb_rx_message_t *message);
void usb_messages_rearm(void);
bool usb_messages_write(uint32_t session, const uint8_t *bytes, size_t length);
bool usb_messages_idle(void);
void usb_messages_cancel(void);
void usb_messages_tx_task(void *argument);
void usb_messages_wake_application_from_isr(void);

#endif
