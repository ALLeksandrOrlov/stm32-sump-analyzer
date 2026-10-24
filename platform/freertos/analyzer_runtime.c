#include "analyzer_runtime.h"
#include "analyzer_app.h"
#include "usb_message_transport.h"
#include "FreeRTOS.h"
#include "task.h"

/** Bytes retained when response backpressure pauses parsing mid-packet. */
typedef struct {
    usb_rx_message_t packet;
    size_t length;
    size_t next_byte;
} pending_input_t;

enum {
    APPLICATION_STACK_WORDS = 768,
    TRANSMITTER_STACK_WORDS = 512,
    APPLICATION_PRIORITY = 2,
    TRANSMITTER_PRIORITY = 3
};

static analyzer_app_t application;
static uint32_t application_session;
static bool initialized;
static StaticTask_t application_control;
static StaticTask_t transmitter_control;
static StackType_t application_stack[APPLICATION_STACK_WORDS];
static StackType_t transmitter_stack[TRANSMITTER_STACK_WORDS];
_Static_assert(ANALYZER_TRANSFER_BYTES <= USB_MESSAGE_BYTES,
               "One application packet must fit the USB mailbox");

static bool enqueue_output(void *context, const uint8_t *bytes, size_t length)
{
    (void)context;
    return usb_messages_write(application_session, bytes, length);
}

static bool output_idle(void *context)
{
    (void)context;
    return usb_messages_idle();
}

static void cancel_output(void *context)
{
    (void)context;
    usb_messages_cancel();
}

static bool reconcile_session(pending_input_t *input)
{
    uint32_t current_session = usb_messages_session();
    if (application_session == current_session) {
        return false;
    }

    analyzer_app_reset(&application);
    application_session = current_session;
    input->length = 0u;
    input->next_byte = 0u;
    return true;
}

static bool receive_packet(pending_input_t *input)
{
    if (input->length != 0u) {
        return false;
    }
    input->length = usb_messages_receive(&input->packet);
    input->next_byte = 0u;
    if (input->length == 0u) {
        return false;
    }
    if (input->packet.session != application_session) {
        input->length = 0u;
    }
    return true;
}

static bool process_input(pending_input_t *input)
{
    bool progressed = false;

    /* At most one full-speed packet is examined per pass. A rejected byte
     * remains pending until output space is available; no command is lost. */
    while ((input->next_byte < input->length) &&
           (application_session == usb_messages_session())) {
        if (!analyzer_app_receive(&application,
                                  input->packet.bytes[input->next_byte])) {
            break;
        }
        ++input->next_byte;
        progressed = true;
    }
    if (input->next_byte == input->length) {
        input->length = 0u;
        input->next_byte = 0u;
    }
    return progressed;
}

static void application_task(void *argument)
{
    pending_input_t input = {0};
    (void)argument;

    for (;;) {
        bool progressed = reconcile_session(&input);
        progressed = receive_packet(&input) || progressed;
        usb_messages_rearm();
        progressed = process_input(&input) || progressed;

        /* A reset can preempt a command. Reconcile it before producing output;
         * the TX mailbox also checks the originating session atomically. */
        if (application_session != usb_messages_session()) {
            continue;
        }
        progressed = analyzer_app_poll(&application) || progressed;
        if (!progressed) {
            /* Notifications stay pending across the condition-to-wait gap.
             * Index zero belongs to the FreeRTOS message-buffer mechanism. */
            (void)ulTaskNotifyTakeIndexed(ANALYZER_NOTIFICATION_INDEX, pdTRUE,
                                          portMAX_DELAY);
        }
    }
}

bool analyzer_runtime_init(capture_driver_t capture, analyzer_usb_port_t usb,
                           uint32_t timer_clock_hz)
{
    TaskHandle_t application_handle;
    TaskHandle_t transmitter_handle;
    analyzer_transport_t output = {
        .context = NULL,
        .write = enqueue_output,
        .idle = output_idle,
        .cancel = cancel_output
    };

    if (initialized || (timer_clock_hz == 0u) || (usb.activate == NULL) ||
        (usb.write == NULL) || (usb.idle == NULL) || (usb.rearm == NULL) ||
        (capture.start == NULL) || (capture.stop == NULL) ||
        (capture.poll == NULL) || (capture.buffer == NULL)) {
        return false;
    }
    initialized = true;
    usb_messages_init(usb);
    analyzer_app_init(&application, capture, output, timer_clock_hz);
    application_handle = xTaskCreateStatic(
        application_task, "analyzer", APPLICATION_STACK_WORDS, NULL,
        APPLICATION_PRIORITY, application_stack, &application_control);
    transmitter_handle = xTaskCreateStatic(
        usb_messages_tx_task, "usb-tx", TRANSMITTER_STACK_WORDS, NULL,
        TRANSMITTER_PRIORITY, transmitter_stack, &transmitter_control);
    if ((application_handle == NULL) || (transmitter_handle == NULL)) {
        return false;
    }
    usb_messages_bind_tasks(application_handle, transmitter_handle);
    return true;
}

void analyzer_runtime_capture_event_from_isr(void)
{
    usb_messages_wake_application_from_isr();
}
