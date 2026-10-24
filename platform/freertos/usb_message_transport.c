#include "usb_message_transport.h"
#include "message_buffer.h"
#include <stddef.h>
#include <string.h>

/* The connection owns the hardware transfer. A session invalidates RX on
 * reconnect or malformed input; the generation cancels queued application
 * output within a session, for example on SUMP RESET. */
typedef struct {
    uint32_t connection;
    uint32_t session;
    uint32_t generation;
    uint8_t bytes[USB_MESSAGE_BYTES];
} tx_message_t;

enum { BUFFER_PACKETS = 4 };
static uint8_t rx_storage[BUFFER_PACKETS * (sizeof(usb_rx_message_t) + sizeof(size_t)) + 1];
static uint8_t tx_storage[BUFFER_PACKETS * (sizeof(tx_message_t) + sizeof(size_t)) + 1];
static StaticMessageBuffer_t rx_control, tx_control;
static MessageBufferHandle_t rx_buffer, tx_buffer;
static TaskHandle_t application_task, transmitter_task;
static analyzer_usb_port_t usb;
static volatile uint32_t connection_session;
static volatile uint32_t physical_connection;
static volatile bool connected, rx_armed;
static uint32_t tx_generation;
static uint32_t accepted_messages, released_messages;

static void notify_from_isr(TaskHandle_t task, BaseType_t *woken)
{
    vTaskNotifyGiveIndexedFromISR(task, ANALYZER_NOTIFICATION_INDEX, woken);
}

static void yield_from_isr(BaseType_t woken)
{
    portYIELD_FROM_ISR(woken);
}

static void wake_transmitter(void)
{
    xTaskNotifyGiveIndexed(transmitter_task, ANALYZER_NOTIFICATION_INDEX);
}

static void wake_application(void)
{
    xTaskNotifyGiveIndexed(application_task, ANALYZER_NOTIFICATION_INDEX);
}

void usb_messages_init(analyzer_usb_port_t port)
{
    usb = port;
    rx_buffer = xMessageBufferCreateStatic(sizeof(rx_storage), rx_storage, &rx_control);
    tx_buffer = xMessageBufferCreateStatic(sizeof(tx_storage), tx_storage, &tx_control);
    configASSERT(rx_buffer != NULL && tx_buffer != NULL);
}

void usb_messages_bind_tasks(TaskHandle_t application, TaskHandle_t transmitter)
{
    application_task = application;
    transmitter_task = transmitter;
}

uint32_t usb_messages_session(void)
{
    return connection_session;
}

static void connection_changed_from_isr(bool available)
{
    BaseType_t woken = pdFALSE;
    ++physical_connection;
    ++connection_session;
    connected = available;
    /* CDC Init arms its initial OUT transfer; DeInit closes the endpoint. */
    rx_armed = available;
    notify_from_isr(application_task, &woken);
    notify_from_isr(transmitter_task, &woken);
    yield_from_isr(woken);
}

void analyzer_runtime_usb_connected_from_isr(void)
{
    connection_changed_from_isr(true);
}

void analyzer_runtime_usb_disconnected_from_isr(void)
{
    connection_changed_from_isr(false);
}

void analyzer_runtime_usb_receive_from_isr(const uint8_t *bytes, uint32_t length)
{
    BaseType_t woken = pdFALSE;
    rx_armed = false;
    if (!connected) {
        return;
    }
    bool invalid = length > USB_MESSAGE_BYTES || (length != 0 && bytes == NULL);
    if (!invalid && length != 0) {
        usb_rx_message_t message;
        message.session = connection_session;
        memcpy(message.bytes, bytes, length);
        size_t size = offsetof(usb_rx_message_t, bytes) + length;
        invalid = xMessageBufferSendFromISR(rx_buffer, &message, size, &woken) != size;
    }
    if (invalid) {
        /* Normal traffic cannot overflow: OUT is only armed with space for a
         * full packet. A malformed callback invalidates the parser session. */
        ++connection_session;
        notify_from_isr(transmitter_task, &woken);
    }
    notify_from_isr(application_task, &woken);
    yield_from_isr(woken);
}

void analyzer_runtime_usb_tx_complete_from_isr(void)
{
    BaseType_t woken = pdFALSE;
    notify_from_isr(transmitter_task, &woken);
    yield_from_isr(woken);
}

void usb_messages_wake_application_from_isr(void)
{
    BaseType_t woken = pdFALSE;
    notify_from_isr(application_task, &woken);
    yield_from_isr(woken);
}

size_t usb_messages_receive(usb_rx_message_t *message)
{
    size_t size = xMessageBufferReceive(rx_buffer, message, sizeof(*message), 0);
    return size ? size - offsetof(usb_rx_message_t, bytes) : 0;
}

void usb_messages_rearm(void)
{
    /* Reserve capacity before granting another OUT transfer. This check and
     * the endpoint operation exclude USB reset, but leave capture IRQs enabled. */
    taskENTER_CRITICAL();
    if (connected && !rx_armed &&
        xMessageBufferSpaceAvailable(rx_buffer) >= sizeof(usb_rx_message_t) + sizeof(size_t)) {
        usb.rearm(usb.context);
        rx_armed = true;
    }
    taskEXIT_CRITICAL();
}

bool usb_messages_write(uint32_t session, const uint8_t *bytes, size_t length)
{
    configASSERT(length > 0 && length <= USB_MESSAGE_BYTES);
    tx_message_t message = {.session = session};
    memcpy(message.bytes, bytes, length);
    size_t size = offsetof(tx_message_t, bytes) + length;
    bool accepted = false;
    taskENTER_CRITICAL();
    if (connected && session == connection_session) {
        message.connection = physical_connection;
        message.generation = tx_generation;
        accepted = xMessageBufferSend(tx_buffer, &message, size, 0) == size;
        if (accepted) {
            ++accepted_messages;
            wake_transmitter();
        }
    }
    taskEXIT_CRITICAL();
    return accepted;
}

bool usb_messages_idle(void)
{
    taskENTER_CRITICAL();
    bool idle = accepted_messages == released_messages;
    taskEXIT_CRITICAL();
    return idle;
}

void usb_messages_cancel(void)
{
    taskENTER_CRITICAL();
    ++tx_generation;
    wake_transmitter();
    taskEXIT_CRITICAL();
}

static bool message_is_current(const tx_message_t *message)
{
    return connected && message->session == connection_session &&
           message->generation == tx_generation;
}

static void release_message(void)
{
    taskENTER_CRITICAL();
    ++released_messages;
    wake_application();
    taskEXIT_CRITICAL();
}

void usb_messages_tx_task(void *argument)
{
    (void)argument;
    /* Only this task touches the packet borrowed by USB. Cancel changes its
     * eligibility, never its storage, while the controller is still using it. */
    tx_message_t message;
    size_t length = 0;
    bool in_flight = false;
    usb.activate(usb.context);
    for (;;) {
        if (in_flight) {
            taskENTER_CRITICAL();
            /* A protocol fault invalidates queued commands, but only an actual
             * USB disconnect closes the endpoint and releases a borrowed buffer. */
            bool completed = message.connection != physical_connection || !connected || usb.idle(usb.context);
            taskEXIT_CRITICAL();
            if (!completed) {
                ulTaskNotifyTakeIndexed(ANALYZER_NOTIFICATION_INDEX, pdTRUE, portMAX_DELAY);
                continue;
            }
            in_flight = false;
            length = 0;
            release_message();
        }
        if (length == 0) {
            size_t size = xMessageBufferReceive(tx_buffer, &message, sizeof(message), 0);
            if (!size) {
                ulTaskNotifyTakeIndexed(ANALYZER_NOTIFICATION_INDEX, pdTRUE, portMAX_DELAY);
                continue;
            }
            length = size - offsetof(tx_message_t, bytes);
            /* Space became available even if USB still rejects submission. */
            wake_application();
        }
        taskENTER_CRITICAL();
        bool current = message_is_current(&message);
        if (current) {
            in_flight = usb.write(usb.context, message.bytes, length);
        }
        taskEXIT_CRITICAL();
        if (!current) {
            length = 0;
            release_message();
        } else if (!in_flight) {
            /* A transient BUSY needs a bounded retry even without an IN IRQ.
             * The task sleeps between attempts; it never spins on the driver. */
            ulTaskNotifyTakeIndexed(ANALYZER_NOTIFICATION_INDEX, pdTRUE, pdMS_TO_TICKS(1));
        }
    }
}
