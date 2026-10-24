#include "usb_transport_internal.h"
#include "message_buffer.h"
#include <stddef.h>
#include <string.h>

/* Capacity for four maximum-length packets, including FreeRTOS framing. Short
 * packets share this byte budget. Only USB IRQ writes; only the app reads. */
enum { RX_FULL_PACKET_CAPACITY = 4 };
static uint8_t receive_storage[
    RX_FULL_PACKET_CAPACITY * (sizeof(usb_rx_message_t) + sizeof(size_t)) + 1u];
static StaticMessageBuffer_t receive_control;
static MessageBufferHandle_t receive_buffer;
static usb_transport_context_t *transport;
static volatile bool receive_armed;

void usb_receive_channel_init(usb_transport_context_t *context)
{
    transport = context;
    receive_armed = false;
    receive_buffer = xMessageBufferCreateStatic(sizeof(receive_storage),
                                                receive_storage,
                                                &receive_control);
    configASSERT(receive_buffer != NULL);
}

void usb_receive_channel_connection_from_isr(bool connected)
{
    /* CDC initialization arms one OUT packet; deinitialization closes it. */
    receive_armed = connected;
}

bool usb_receive_channel_accept_from_isr(const uint8_t *bytes, uint32_t length,
                                        BaseType_t *higher_priority_task_woken)
{
    usb_rx_message_t message;
    size_t message_size;

    receive_armed = false;
    if (!transport->connected) {
        return true;
    }
    if ((length > USB_MESSAGE_BYTES) || ((length != 0u) && (bytes == NULL))) {
        return false;
    }
    if (length == 0u) {
        return true;
    }

    message.session = transport->session;
    memcpy(message.bytes, bytes, length);
    message_size = offsetof(usb_rx_message_t, bytes) + length;
    return xMessageBufferSendFromISR(receive_buffer, &message, message_size,
                                    higher_priority_task_woken) == message_size;
}

size_t usb_messages_receive(usb_rx_message_t *message)
{
    size_t message_size = xMessageBufferReceive(receive_buffer, message,
                                               sizeof(*message), 0u);
    if (message_size == 0u) {
        return 0u;
    }
    configASSERT(message_size > offsetof(usb_rx_message_t, bytes));
    return message_size - offsetof(usb_rx_message_t, bytes);
}

void usb_messages_rearm(void)
{
    const size_t required_space = sizeof(usb_rx_message_t) + sizeof(size_t);

    /* Reserve room and arm atomically relative to USB reset. High-priority
     * capture interrupts remain enabled because FreeRTOS uses BASEPRI. */
    taskENTER_CRITICAL();
    if (transport->connected && !receive_armed &&
        (xMessageBufferSpaceAvailable(receive_buffer) >= required_space)) {
        /* Suspend may make the endpoint unavailable without ending the
         * session. Keep the credit unclaimed so resume can retry this arm. */
        receive_armed = transport->port.rearm(transport->port.context);
    }
    taskEXIT_CRITICAL();
}
