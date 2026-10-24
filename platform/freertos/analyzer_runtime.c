#include "analyzer_runtime.h"
#include "analyzer_app.h"
#include "usb_message_transport.h"
#include "FreeRTOS.h"
#include "task.h"

/* The application owns parser, capture state and command processing. The USB
 * task owns transmission. Their stacks and queues never use the heap. */
static analyzer_app_t app;
static uint32_t application_session;
enum { APPLICATION_STACK_WORDS = 768, TRANSMITTER_STACK_WORDS = 512 };
static StaticTask_t application_control, transmitter_control;
static StackType_t application_stack[APPLICATION_STACK_WORDS];
static StackType_t transmitter_stack[TRANSMITTER_STACK_WORDS];
_Static_assert(ANALYZER_TRANSFER_BYTES <= USB_MESSAGE_BYTES, "USB queue must fit an application packet");

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

static void application_task(void *argument)
{
    (void)argument;
    usb_rx_message_t received;
    size_t length = 0, offset = 0;
    for (;;) {
        bool progress = false;
        uint32_t session = usb_messages_session();
        if (application_session != session) {
            analyzer_app_reset(&app);
            application_session = session;
            length = offset = 0;
            progress = true;
        }
        if (length == 0) {
            length = usb_messages_receive(&received);
            offset = 0;
            if (length) {
                progress = true;
                if (received.session != application_session) {
                    length = 0;
                }
            }
        }
        usb_messages_rearm();
        while (offset < length && application_session == usb_messages_session()) {
            if (!analyzer_app_receive(&app, received.bytes[offset])) {
                break;
            }
            ++offset;
            progress = true;
        }
        if (offset == length) {
            length = offset = 0;
        }
        /* A USB reset may interrupt command execution. Reconcile it before
         * producing any output; the transport also checks the session atomically. */
        if (application_session != usb_messages_session()) {
            continue;
        }
        capture_state_t state = app.capture.state;
        uint32_t samples = app.samples_sent;
        size_t reply = app.reply_length;
        analyzer_app_poll(&app);
        progress |= state != app.capture.state || samples != app.samples_sent || reply != app.reply_length;
        if (!progress) {
            /* Notifications use index 1; message buffers reserve index 0.
             * An event arriving before this wait remains pending, so no wakeup
             * can be lost between inspecting state and blocking the task. */
            ulTaskNotifyTakeIndexed(ANALYZER_NOTIFICATION_INDEX, pdTRUE, portMAX_DELAY);
        }
    }
}

bool analyzer_runtime_init(capture_driver_t capture, analyzer_usb_port_t usb,
                           uint32_t timer_clock_hz)
{
    usb_messages_init(usb);
    analyzer_app_init(&app, capture, (analyzer_transport_t){
        .context = NULL, .write = enqueue_output, .idle = output_idle, .cancel = cancel_output
    }, timer_clock_hz);
    TaskHandle_t application = xTaskCreateStatic(application_task, "analyzer", APPLICATION_STACK_WORDS, NULL, 2,
                                                application_stack, &application_control);
    TaskHandle_t transmitter = xTaskCreateStatic(usb_messages_tx_task, "usb-tx", TRANSMITTER_STACK_WORDS, NULL, 3,
                                                transmitter_stack, &transmitter_control);
    if (!application || !transmitter) {
        return false;
    }
    usb_messages_bind_tasks(application, transmitter);
    return true;
}

void analyzer_runtime_capture_event_from_isr(void)
{
    usb_messages_wake_application_from_isr();
}
