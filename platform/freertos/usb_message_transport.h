#ifndef USB_MESSAGE_TRANSPORT_H
#define USB_MESSAGE_TRANSPORT_H

#include "analyzer_runtime.h"
#include "FreeRTOS.h"
#include "task.h"

#define USB_MESSAGE_BYTES 64u
#define ANALYZER_NOTIFICATION_INDEX 1u

/** @brief One copied USB OUT packet and the parser session that owns it. */
typedef struct {
    uint32_t session;
    uint8_t bytes[USB_MESSAGE_BYTES];
} usb_rx_message_t;

/** @brief Construct both static single-writer/single-reader mailboxes. */
void usb_messages_init(analyzer_usb_port_t port);
/** @brief Bind notification destinations before USB interrupts are enabled. */
void usb_messages_bind_tasks(TaskHandle_t application, TaskHandle_t transmitter);
/** @brief Read the current parser session; application task only. */
uint32_t usb_messages_session(void);
/** @brief Take one copied packet without waiting; application task only. */
size_t usb_messages_receive(usb_rx_message_t *message);
/** @brief Grant one OUT transfer if a full packet fits; application task only. */
void usb_messages_rearm(void);
/**
 * @brief Copy a complete output packet or reject it without consuming bytes.
 * @param session Parser session that produced this output.
 * @return True once the caller may reuse its source buffer.
 * @pre Called only by the application task; length is in [1, 64].
 */
bool usb_messages_write(uint32_t session, const uint8_t *bytes, size_t length);
/** @brief True after all accepted packets complete or are discarded. */
bool usb_messages_idle(void);
/**
 * @brief Invalidate queued output; application task only.
 *
 * An endpoint-owned packet remains intact until completion or disconnect.
 */
void usb_messages_cancel(void);
/** @brief Transmitter task entry; owns the buffer borrowed by the USB driver. */
void usb_messages_tx_task(void *argument);
/** @brief Notify the application from a FreeRTOS-safe interrupt priority. */
void usb_messages_wake_application_from_isr(void);

#endif
