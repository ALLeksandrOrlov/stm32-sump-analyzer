#include "usb_transport_internal.h"
#include "message_buffer.h"
#include <stddef.h>
#include <string.h>

/** Each queued packet records both endpoint ownership and logical validity. */
typedef struct {
    uint32_t connection;
    uint32_t session;
    uint32_t generation;
    uint8_t bytes[USB_MESSAGE_BYTES];
} transmit_message_t;

/** This storage is never changed while the endpoint borrows its bytes. */
typedef struct {
    transmit_message_t message;
    size_t length;
    bool endpoint_owns_packet;
} pending_transmission_t;

enum { TX_FULL_PACKET_CAPACITY = 4, TX_RETRY_DELAY_TICKS = 1 };
static uint8_t transmit_storage[
    TX_FULL_PACKET_CAPACITY * (sizeof(transmit_message_t) + sizeof(size_t)) + 1u];
static StaticMessageBuffer_t transmit_control;
static MessageBufferHandle_t transmit_buffer;
static usb_transport_context_t *transport;
/* Both fields are protected by task critical sections. The generation changes
 * on protocol RESET; outstanding includes queued, pending and active packets. */
static uint32_t output_generation;
static uint32_t outstanding_packets;

static void notify_application(void)
{
    xTaskNotifyGiveIndexed(transport->application_task,
                          ANALYZER_NOTIFICATION_INDEX);
}

static void notify_transmitter(void)
{
    xTaskNotifyGiveIndexed(transport->transmitter_task,
                          ANALYZER_NOTIFICATION_INDEX);
}

static void wait_for_event(TickType_t timeout)
{
    /* Message buffers reserve index zero. A pending event at this index stays
     * latched between checking a condition and blocking on the notification. */
    (void)ulTaskNotifyTakeIndexed(ANALYZER_NOTIFICATION_INDEX, pdTRUE, timeout);
}

static void release_packet(pending_transmission_t *pending)
{
    pending->length = 0u;
    pending->endpoint_owns_packet = false;
    taskENTER_CRITICAL();
    configASSERT(outstanding_packets > 0u);
    --outstanding_packets;
    notify_application();
    taskEXIT_CRITICAL();
}

static bool endpoint_released_packet(const pending_transmission_t *pending)
{
    bool released;
    taskENTER_CRITICAL();
    /* Session cancellation alone cannot release a live USB transfer. The
     * adapter guarantees endpoint shutdown before a connection change. */
    released = (pending->message.connection != transport->connection) ||
               !transport->connected ||
               transport->port.idle(transport->port.context);
    taskEXIT_CRITICAL();
    return released;
}

static bool dequeue_packet(pending_transmission_t *pending)
{
    size_t message_size = xMessageBufferReceive(transmit_buffer,
                                                &pending->message,
                                                sizeof(pending->message), 0u);
    if (message_size == 0u) {
        return false;
    }
    configASSERT(message_size > offsetof(transmit_message_t, bytes));
    pending->length = message_size - offsetof(transmit_message_t, bytes);
    /* Queue space is usable immediately, even if the endpoint is still busy. */
    notify_application();
    return true;
}

static void submit_packet(pending_transmission_t *pending)
{
    bool current;
    taskENTER_CRITICAL();
    current = transport->connected &&
              (pending->message.connection == transport->connection) &&
              (pending->message.session == transport->session) &&
              (pending->message.generation == output_generation);
    if (current) {
        pending->endpoint_owns_packet = transport->port.write(
            transport->port.context, pending->message.bytes, pending->length);
    }
    taskEXIT_CRITICAL();

    if (!current) {
        release_packet(pending);
    } else if (!pending->endpoint_owns_packet) {
        /* A BUSY return need not be followed by an IN completion. Sleep one
         * scheduler tick before retrying; never spin on the USB controller. */
        wait_for_event(TX_RETRY_DELAY_TICKS);
    } else {
        /* The next iteration observes ownership before touching the bytes. */
    }
}

void usb_transmit_channel_init(usb_transport_context_t *context)
{
    transport = context;
    output_generation = 0u;
    outstanding_packets = 0u;
    transmit_buffer = xMessageBufferCreateStatic(sizeof(transmit_storage),
                                                 transmit_storage,
                                                 &transmit_control);
    configASSERT(transmit_buffer != NULL);
}

bool usb_messages_write(uint32_t session, const uint8_t *bytes, size_t length)
{
    transmit_message_t message;
    size_t message_size;
    bool accepted = false;

    configASSERT(bytes != NULL);
    configASSERT((length > 0u) && (length <= USB_MESSAGE_BYTES));
    message.session = session;
    memcpy(message.bytes, bytes, length);
    message_size = offsetof(transmit_message_t, bytes) + length;

    /* Link validation and enqueue form one transaction relative to USB reset.
     * There is exactly one producer, the analyzer application task. */
    taskENTER_CRITICAL();
    if (transport->connected && (session == transport->session)) {
        message.connection = transport->connection;
        message.generation = output_generation;
        accepted = xMessageBufferSend(transmit_buffer, &message,
                                      message_size, 0u) == message_size;
        if (accepted) {
            ++outstanding_packets;
            notify_transmitter();
        }
    }
    taskEXIT_CRITICAL();
    return accepted;
}

bool usb_messages_idle(void)
{
    bool idle;
    taskENTER_CRITICAL();
    idle = outstanding_packets == 0u;
    taskEXIT_CRITICAL();
    return idle;
}

void usb_messages_cancel(void)
{
    taskENTER_CRITICAL();
    ++output_generation;
    notify_transmitter();
    taskEXIT_CRITICAL();
}

void usb_messages_tx_task(void *argument)
{
    pending_transmission_t pending = {0};
    (void)argument;

    /* FreeRTOS has initialized priority validation and PendSV before USB can
     * invoke a FromISR API. The prepared IRQ stays masked until this point. */
    transport->port.activate(transport->port.context);
    for (;;) {
        if (pending.endpoint_owns_packet) {
            if (!endpoint_released_packet(&pending)) {
                wait_for_event(portMAX_DELAY);
                continue;
            }
            release_packet(&pending);
        }
        if ((pending.length == 0u) && !dequeue_packet(&pending)) {
            wait_for_event(portMAX_DELAY);
            continue;
        }
        submit_packet(&pending);
    }
}
