#include "usb_transport_internal.h"

/** Shared lifecycle state; queue storage belongs to the individual channels. */
static usb_transport_context_t transport;

static void notify_tasks_from_isr(BaseType_t *higher_priority_task_woken)
{
    vTaskNotifyGiveIndexedFromISR(transport.application_task,
                                 ANALYZER_NOTIFICATION_INDEX,
                                 higher_priority_task_woken);
    vTaskNotifyGiveIndexedFromISR(transport.transmitter_task,
                                 ANALYZER_NOTIFICATION_INDEX,
                                 higher_priority_task_woken);
}

static void change_connection_from_isr(bool connected)
{
    BaseType_t higher_priority_task_woken = pdFALSE;

    /* The adapter closes the previous endpoint before reporting a new link. */
    ++transport.connection;
    ++transport.session;
    transport.connected = connected;
    usb_receive_channel_connection_from_isr(connected);
    notify_tasks_from_isr(&higher_priority_task_woken);
    portYIELD_FROM_ISR(higher_priority_task_woken);
}

void usb_messages_init(analyzer_usb_port_t port)
{
    transport = (usb_transport_context_t){.port = port};
    usb_receive_channel_init(&transport);
    usb_transmit_channel_init(&transport);
}

void usb_messages_bind_tasks(TaskHandle_t application, TaskHandle_t transmitter)
{
    transport.application_task = application;
    transport.transmitter_task = transmitter;
}

uint32_t usb_messages_session(void)
{
    return transport.session;
}

void analyzer_runtime_usb_connected_from_isr(void)
{
    change_connection_from_isr(true);
}

void analyzer_runtime_usb_disconnected_from_isr(void)
{
    change_connection_from_isr(false);
}

void analyzer_runtime_usb_receive_from_isr(const uint8_t *bytes, uint32_t length)
{
    BaseType_t higher_priority_task_woken = pdFALSE;
    bool accepted = usb_receive_channel_accept_from_isr(
        bytes, length, &higher_priority_task_woken);

    if (!accepted) {
        /* Parser recovery must not release memory still borrowed by USB IN. */
        ++transport.session;
        vTaskNotifyGiveIndexedFromISR(transport.transmitter_task,
                                     ANALYZER_NOTIFICATION_INDEX,
                                     &higher_priority_task_woken);
    }
    vTaskNotifyGiveIndexedFromISR(transport.application_task,
                                 ANALYZER_NOTIFICATION_INDEX,
                                 &higher_priority_task_woken);
    portYIELD_FROM_ISR(higher_priority_task_woken);
}

void analyzer_runtime_usb_resumed_from_isr(void)
{
    BaseType_t higher_priority_task_woken = pdFALSE;
    /* No session transition: a split command and an active IN remain valid. */
    notify_tasks_from_isr(&higher_priority_task_woken);
    portYIELD_FROM_ISR(higher_priority_task_woken);
}

void analyzer_runtime_usb_tx_complete_from_isr(void)
{
    BaseType_t higher_priority_task_woken = pdFALSE;
    vTaskNotifyGiveIndexedFromISR(transport.transmitter_task,
                                 ANALYZER_NOTIFICATION_INDEX,
                                 &higher_priority_task_woken);
    portYIELD_FROM_ISR(higher_priority_task_woken);
}

void usb_messages_wake_application_from_isr(void)
{
    BaseType_t higher_priority_task_woken = pdFALSE;
    vTaskNotifyGiveIndexedFromISR(transport.application_task,
                                 ANALYZER_NOTIFICATION_INDEX,
                                 &higher_priority_task_woken);
    portYIELD_FROM_ISR(higher_priority_task_woken);
}
