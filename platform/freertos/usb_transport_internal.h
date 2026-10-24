#ifndef USB_TRANSPORT_INTERNAL_H
#define USB_TRANSPORT_INTERNAL_H

#include "usb_message_transport.h"

/**
 * @brief Shared state of the single USB connection.
 *
 * USB callbacks update the link fields at a FreeRTOS-safe IRQ priority. Tasks
 * use a critical section when combining a link check with an endpoint action.
 * The connection identifier changes only when the endpoint releases its buffer;
 * the session also changes when a malformed receive invalidates parser state.
 */
typedef struct {
    analyzer_usb_port_t port;
    TaskHandle_t application_task;
    TaskHandle_t transmitter_task;
    volatile uint32_t connection;
    volatile uint32_t session;
    volatile bool connected;
} usb_transport_context_t;

/** @brief Initialize static RX storage before interrupts and scheduling. */
void usb_receive_channel_init(usb_transport_context_t *context);
/** @brief Record whether CDC has armed its initial OUT packet. ISR only. */
void usb_receive_channel_connection_from_isr(bool connected);
/**
 * @brief Copy one OUT packet without interpreting its contents. ISR only.
 * @return False if the packet is malformed or cannot be stored.
 */
bool usb_receive_channel_accept_from_isr(const uint8_t *bytes, uint32_t length,
                                        BaseType_t *higher_priority_task_woken);
/** @brief Initialize static TX storage before interrupts and scheduling. */
void usb_transmit_channel_init(usb_transport_context_t *context);

#endif
