#include "usb_cdc_port.h"
#include "usb_cdc_storage.h"
#include "analyzer_runtime.h"
#include "usbd_ctlreq.h"
#include "usbd_core.h"
#include "FreeRTOS.h"
#include "task.h"
#include "stm32f4xx_hal.h"
#include "usbd_cdc.h"
#include "usbd_cdc_if.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

USBD_HandleTypeDef hUsbDeviceFS;
uint8_t UserRxBufferFS[CDC_DATA_FS_OUT_PACKET_SIZE];
uint8_t UserTxBufferFS[CDC_DATA_FS_IN_PACKET_SIZE];
static USBD_CDC_HandleTypeDef cdc;
static struct {
    struct { uint32_t maxpacket; } in_endpoint[16];
} pcd;
static struct {
    bool in_interrupt;
    bool irq_enabled;
    bool scheduler_running;
    uint8_t transmit_result;
    uint8_t data_in_result;
    uint8_t rx_buffer_result;
    uint8_t rearm_result;
    unsigned transmit_calls;
    unsigned data_in_calls;
    unsigned zlp_transmissions;
    unsigned completions;
    unsigned rearm_calls;
    unsigned resumes;
    unsigned connections;
    unsigned disconnections;
    unsigned received_packets;
    unsigned setup_calls;
    unsigned setup_stalls;
    const uint8_t *received_bytes;
    uint32_t received_length;
    const uint8_t *borrowed;
    uint16_t borrowed_length;
    uint8_t *rx_buffer;
    IRQn_Type configured_irq;
    uint32_t configured_priority;
    uint32_t configured_subpriority;
} usb;
static unsigned critical_nesting;
static uint32_t basepri;

void test_enter_critical(void)
{
    ++critical_nesting;
    basepri = configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS);
}

void test_exit_critical(void)
{
    assert(critical_nesting != 0);
    if (--critical_nesting == 0) {
        basepri = 0;
    }
}

static void assert_usb_excluded_and_capture_enabled(void)
{
    assert(critical_nesting != 0);
    uint32_t usb_priority = 6u << (8 - configPRIO_BITS);
    uint32_t capture_priority = 1u << (8 - configPRIO_BITS);
    assert(basepri != 0 && usb_priority >= basepri);
    assert(capture_priority < basepri);
}

BaseType_t xTaskGetSchedulerState(void)
{
    return usb.scheduler_running ? taskSCHEDULER_RUNNING : taskSCHEDULER_NOT_STARTED;
}

void HAL_NVIC_EnableIRQ(IRQn_Type interrupt)
{
    assert(interrupt == OTG_FS_IRQn && usb.scheduler_running);
    usb.irq_enabled = true;
}

void HAL_NVIC_DisableIRQ(IRQn_Type interrupt)
{
    assert(interrupt == OTG_FS_IRQn);
    usb.irq_enabled = false;
}

void HAL_NVIC_SetPriority(IRQn_Type interrupt, uint32_t priority, uint32_t subpriority)
{
    usb.configured_irq = interrupt;
    usb.configured_priority = priority;
    usb.configured_subpriority = subpriority;
}

uint8_t CDC_Transmit_FS(uint8_t *bytes, uint16_t length)
{
    assert_usb_excluded_and_capture_enabled();
    ++usb.transmit_calls;
    if (usb.transmit_result != USBD_OK) {
        return usb.transmit_result;
    }
    usb.borrowed = bytes;
    usb.borrowed_length = length;
    cdc.TxState = 1;
    hUsbDeviceFS.ep_in[CDC_IN_EP & 0x0fu].total_length = length;
    return USBD_OK;
}

uint8_t USBD_CDC_SetRxBuffer(USBD_HandleTypeDef *device, uint8_t *bytes)
{
    if (!usb.in_interrupt) {
        assert_usb_excluded_and_capture_enabled();
    }
    assert(device == &hUsbDeviceFS);
    if (usb.rx_buffer_result == USBD_OK) {
        usb.rx_buffer = bytes;
    }
    return usb.rx_buffer_result;
}

uint8_t USBD_CDC_ReceivePacket(USBD_HandleTypeDef *device)
{
    assert_usb_excluded_and_capture_enabled();
    assert(device == &hUsbDeviceFS);
    assert(usb.rx_buffer == UserRxBufferFS);
    ++usb.rearm_calls;
    return usb.rearm_result;
}

void analyzer_runtime_usb_tx_complete_from_isr(void)
{
    ++usb.completions;
}

uint8_t USBD_CDC_SetTxBuffer(USBD_HandleTypeDef *device, uint8_t *bytes, uint16_t length)
{
    assert(usb.in_interrupt);
    assert(device == &hUsbDeviceFS && bytes == UserTxBufferFS && length == 0U);
    return USBD_OK;
}

void analyzer_runtime_usb_connected_from_isr(void)
{
    assert(usb.in_interrupt && usb.rx_buffer == UserRxBufferFS);
    ++usb.connections;
}

USBD_StatusTypeDef USBD_LL_Resume(USBD_HandleTypeDef *device)
{
    assert(usb.in_interrupt && device == &hUsbDeviceFS);
    device->dev_state = device->dev_old_state;
    return USBD_OK;
}

void analyzer_runtime_usb_resumed_from_isr(void)
{
    assert(usb.in_interrupt && hUsbDeviceFS.dev_state == USBD_STATE_CONFIGURED);
    ++usb.resumes;
}

void analyzer_runtime_usb_disconnected_from_isr(void)
{
    assert(usb.in_interrupt);
    ++usb.disconnections;
}

void analyzer_runtime_usb_receive_from_isr(const uint8_t *bytes, uint32_t length)
{
    assert(usb.in_interrupt);
    ++usb.received_packets;
    usb.received_bytes = bytes;
    usb.received_length = length;
}

void USBD_CtlError(USBD_HandleTypeDef *device, USBD_SetupReqTypedef *request)
{
    assert(device == &hUsbDeviceFS && request != NULL);
    ++usb.setup_stalls;
}

static uint8_t class_init(USBD_HandleTypeDef *device, uint8_t configuration)
{
    (void)device;
    (void)configuration;
    return USBD_OK;
}

static uint8_t class_setup(USBD_HandleTypeDef *device, USBD_SetupReqTypedef *request)
{
    assert(device == &hUsbDeviceFS && request != NULL);
    ++usb.setup_calls;
    return USBD_OK;
}

static uint8_t endpoint_zero(USBD_HandleTypeDef *device)
{
    (void)device;
    return USBD_OK;
}

static uint8_t *class_descriptor(uint16_t *length)
{
    static uint8_t descriptor[] = {2, 0};
    *length = sizeof(descriptor);
    return descriptor;
}

static uint8_t class_data_in(USBD_HandleTypeDef *device, uint8_t endpoint)
{
    ++usb.data_in_calls;
    if (device->pClassData == NULL || usb.data_in_result != USBD_OK) {
        return USBD_FAIL;
    }
    USBD_CDC_HandleTypeDef *class_data = device->pClassData;
    /* Match ST CDC's packet/ZLP completion rule. A packet-size multiple keeps
     * TxState busy until the following zero-length packet completes. */
    uint32_t total_length = device->ep_in[endpoint].total_length;
    if (total_length > 0 && total_length % pcd.in_endpoint[endpoint].maxpacket == 0) {
        device->ep_in[endpoint].total_length = 0;
        ++usb.zlp_transmissions;
    } else {
        class_data->TxState = 0;
    }
    return USBD_OK;
}

USBD_ClassTypeDef USBD_CDC = {
    .Init = class_init,
    .DeInit = class_init,
    .Setup = class_setup,
    .EP0_TxSent = endpoint_zero,
    .EP0_RxReady = endpoint_zero,
    .DataIn = class_data_in,
    .DataOut = class_data_in,
    .SOF = endpoint_zero,
    .IsoINIncomplete = class_data_in,
    .IsoOUTIncomplete = class_data_in,
    .GetHSConfigDescriptor = class_descriptor,
    .GetFSConfigDescriptor = class_descriptor,
    .GetOtherSpeedConfigDescriptor = class_descriptor,
    .GetDeviceQualifierDescriptor = class_descriptor,
};

static void reset_fixture(void)
{
    memset(&hUsbDeviceFS, 0, sizeof(hUsbDeviceFS));
    memset(&cdc, 0, sizeof(cdc));
    memset(&usb, 0, sizeof(usb));
    for (unsigned endpoint = 0; endpoint < 16; ++endpoint) {
        pcd.in_endpoint[endpoint].maxpacket = CDC_DATA_FS_IN_PACKET_SIZE;
    }
    critical_nesting = 0;
    basepri = 0;
    hUsbDeviceFS.pClassData = &cdc;
    hUsbDeviceFS.dev_state = USBD_STATE_CONFIGURED;
    usb_cdc_port_init();
}

static void test_class_adapter_preserves_other_callbacks(void)
{
    reset_fixture();
    USBD_ClassTypeDef *wrapped = usb_cdc_port_class();
    assert(wrapped != &USBD_CDC);
    assert(wrapped == usb_cdc_port_class());
    assert(wrapped->DataIn != USBD_CDC.DataIn);
    assert(USBD_CDC.DataIn == class_data_in);
#define ASSERT_UNCHANGED(member) assert(wrapped->member == USBD_CDC.member)
    ASSERT_UNCHANGED(Init);
    ASSERT_UNCHANGED(DeInit);
    assert(wrapped->Setup != USBD_CDC.Setup);
    ASSERT_UNCHANGED(EP0_TxSent);
    ASSERT_UNCHANGED(EP0_RxReady);
    ASSERT_UNCHANGED(DataOut);
    ASSERT_UNCHANGED(SOF);
    ASSERT_UNCHANGED(IsoINIncomplete);
    ASSERT_UNCHANGED(IsoOUTIncomplete);
    ASSERT_UNCHANGED(GetHSConfigDescriptor);
    ASSERT_UNCHANGED(GetFSConfigDescriptor);
    ASSERT_UNCHANGED(GetOtherSpeedConfigDescriptor);
    ASSERT_UNCHANGED(GetDeviceQualifierDescriptor);
#undef ASSERT_UNCHANGED
}

static void test_completion_waits_for_terminating_packet(void)
{
    static const uint8_t bytes[128] = {1, 2, 3};
    const size_t lengths[] = {1, 63, 64, 128};
    for (size_t index = 0; index < sizeof(lengths) / sizeof(lengths[0]); ++index) {
        reset_fixture();
        assert(usb_cdc_port_idle(NULL));
        assert(usb_cdc_port_write(NULL, bytes, lengths[index]));
        assert(usb.borrowed == bytes && usb.borrowed_length == lengths[index]);
        assert(!usb_cdc_port_idle(NULL));
        assert(usb_cdc_port_class()->DataIn(&hUsbDeviceFS, CDC_IN_EP & 0x0fu) == USBD_OK);
        if (lengths[index] % CDC_DATA_FS_IN_PACKET_SIZE == 0) {
            assert(usb.completions == 0 && usb.zlp_transmissions == 1);
            assert(!usb_cdc_port_idle(NULL));
            assert(usb_cdc_port_class()->DataIn(&hUsbDeviceFS, CDC_IN_EP & 0x0fu) == USBD_OK);
        }
        assert(usb_cdc_port_idle(NULL) && usb.completions == 1);
        /* A duplicate completion IRQ cannot release the same buffer twice. */
        assert(usb_cdc_port_class()->DataIn(&hUsbDeviceFS, CDC_IN_EP & 0x0fu) == USBD_OK);
        assert(usb.completions == 1);
    }
}

static void test_unrelated_or_failed_interrupts_do_not_complete_tx(void)
{
    reset_fixture();
    cdc.TxState = 1;
    assert(usb_cdc_port_class()->DataIn(&hUsbDeviceFS, CDC_CMD_EP & 0x0fu) == USBD_OK);
    assert(usb.data_in_calls == 1 && usb.completions == 0);
    cdc.TxState = 1;
    usb.data_in_result = USBD_FAIL;
    assert(usb_cdc_port_class()->DataIn(&hUsbDeviceFS, CDC_IN_EP & 0x0fu) == USBD_FAIL);
    assert(usb.completions == 0 && cdc.TxState == 1);
    hUsbDeviceFS.pClassData = NULL;
    assert(usb_cdc_port_class()->DataIn(&hUsbDeviceFS, CDC_IN_EP & 0x0fu) == USBD_FAIL);
    assert(usb.completions == 0);
}

static void test_transfer_rejections_preserve_buffer_ownership(void)
{
    reset_fixture();
    const uint8_t byte = 42;
    assert(!usb_cdc_port_write(NULL, NULL, 1));
    assert(!usb_cdc_port_write(NULL, &byte, 0));
    assert(!usb_cdc_port_write(NULL, &byte, (size_t)UINT16_MAX + 1));
    assert(usb.transmit_calls == 0 && usb.borrowed == NULL);
    hUsbDeviceFS.dev_state = USBD_STATE_DEFAULT;
    assert(!usb_cdc_port_idle(NULL));
    assert(!usb_cdc_port_write(NULL, &byte, 1));
    hUsbDeviceFS.dev_state = USBD_STATE_CONFIGURED;
    hUsbDeviceFS.pClassData = NULL;
    assert(!usb_cdc_port_idle(NULL));
    assert(!usb_cdc_port_write(NULL, &byte, 1));
    hUsbDeviceFS.pClassData = &cdc;
    cdc.TxState = 1;
    assert(!usb_cdc_port_idle(NULL));
    assert(!usb_cdc_port_write(NULL, &byte, 1));
    assert(usb.transmit_calls == 0);
    cdc.TxState = 0;
    usb.transmit_result = USBD_BUSY;
    assert(!usb_cdc_port_write(NULL, &byte, 1));
    usb.transmit_result = USBD_FAIL;
    assert(!usb_cdc_port_write(NULL, &byte, 1));
    assert(usb.transmit_calls == 2 && usb.borrowed == NULL);
    usb.transmit_result = USBD_OK;
    assert(usb_cdc_port_write(NULL, &byte, 1));
    assert(usb.borrowed == &byte);
}

static void test_critical_sections_nest_without_masking_capture(void)
{
    reset_fixture();
    const uint8_t byte = 42;
    taskENTER_CRITICAL();
    uint32_t outer_mask = basepri;
    assert(usb_cdc_port_idle(NULL));
    assert(critical_nesting == 1 && basepri == outer_mask);
    assert(usb_cdc_port_write(NULL, &byte, 1));
    assert(critical_nesting == 1 && basepri == outer_mask);
    usb_cdc_port_rearm(NULL);
    assert(critical_nesting == 1 && basepri == outer_mask);
    taskEXIT_CRITICAL();
    assert(critical_nesting == 0 && basepri == 0);
}

static void test_rearm_requires_configured_class_and_irq_allows_syscalls(void)
{
    reset_fixture();
    hUsbDeviceFS.dev_state = USBD_STATE_DEFAULT;
    usb_cdc_port_rearm(NULL);
    assert(usb.rearm_calls == 0);
    hUsbDeviceFS.dev_state = USBD_STATE_CONFIGURED;
    hUsbDeviceFS.pClassData = NULL;
    usb_cdc_port_rearm(NULL);
    assert(usb.rearm_calls == 0);
    hUsbDeviceFS.pClassData = &cdc;
    usb_cdc_port_rearm(NULL);
    assert(usb.rearm_calls == 1);
    usb.irq_enabled = true; /* Generated MSP initialization enables the IRQ. */
    usb_cdc_port_configure_irq();
    assert(usb.configured_irq == OTG_FS_IRQn);
    assert(usb.configured_priority == 6 && usb.configured_subpriority == 0);
    assert(!usb.irq_enabled);
    usb.scheduler_running = true;
    usb_cdc_port_activate(NULL);
    assert(usb.irq_enabled);
}

static void test_rearm_retries_after_suspend_and_reports_stack_failure(void)
{
    reset_fixture();
    hUsbDeviceFS.dev_old_state = USBD_STATE_CONFIGURED;
    hUsbDeviceFS.dev_state = USBD_STATE_SUSPENDED;
    assert(!usb_cdc_port_idle(NULL));
    assert(!usb_cdc_port_rearm(NULL));
    assert(usb.rearm_calls == 0U);

    usb.in_interrupt = true;
    assert(usb_cdc_port_resume_from_isr(&hUsbDeviceFS) == USBD_OK);
    usb.in_interrupt = false;
    assert(usb.resumes == 1U && usb.connections == 0U && usb.disconnections == 0U);
    assert(usb_cdc_port_idle(NULL));
    assert(usb_cdc_port_rearm(NULL));
    assert(usb.rearm_calls == 1U);

    usb.rx_buffer_result = USBD_FAIL;
    assert(!usb_cdc_port_rearm(NULL));
    assert(usb.rearm_calls == 1U);
    usb.rx_buffer_result = USBD_OK;
    usb.rearm_result = USBD_FAIL;
    assert(!usb_cdc_port_rearm(NULL));
    assert(usb.rearm_calls == 2U);
    usb.rearm_result = USBD_OK;
    assert(usb_cdc_port_rearm(NULL));
    assert(usb.rearm_calls == 3U);
}

static void test_static_class_storage_lifetime(void)
{
    assert(usb_cdc_storage_acquire(0U) == NULL);
    assert(usb_cdc_storage_acquire(sizeof(USBD_CDC_HandleTypeDef) - 1U) == NULL);
    assert(usb_cdc_storage_acquire(sizeof(USBD_CDC_HandleTypeDef) + 1U) == NULL);
    USBD_CDC_HandleTypeDef *storage = usb_cdc_storage_acquire(sizeof(*storage));
    assert(storage != NULL);
    assert(((uintptr_t)storage % _Alignof(USBD_CDC_HandleTypeDef)) == 0U);
    assert(storage->TxState == 0U && storage->RxState == 0U);
    assert(storage->TxBuffer == NULL && storage->RxBuffer == NULL);
    storage->TxState = 1U;
    storage->RxState = 1U;
    storage->TxBuffer = UserTxBufferFS;
    storage->RxBuffer = UserRxBufferFS;
    assert(usb_cdc_storage_acquire(sizeof(*storage)) == NULL);
    usb_cdc_storage_release(NULL);
    usb_cdc_storage_release(&cdc);
    assert(usb_cdc_storage_acquire(sizeof(*storage)) == NULL);
    assert(storage->TxState == 1U && storage->TxBuffer == UserTxBufferFS);
    usb_cdc_storage_release(storage);
    USBD_CDC_HandleTypeDef *reopened = usb_cdc_storage_acquire(sizeof(*reopened));
    assert(reopened == storage);
    assert(reopened->TxState == 0U && reopened->RxState == 0U);
    assert(reopened->TxBuffer == NULL && reopened->RxBuffer == NULL);
    usb_cdc_storage_release(reopened);
}

static void test_isr_callbacks_only_notify_runtime(void)
{
    reset_fixture();
    usb.in_interrupt = true;
    assert(usb_cdc_port_open_from_isr() == USBD_OK);
    assert(usb.connections == 1U && usb.rearm_calls == 0U);
    const uint8_t packet[] = {0x80U, 0x01U, 0x00U};
    assert(usb_cdc_port_receive_from_isr(packet, sizeof(packet)) == USBD_OK);
    assert(usb.received_packets == 1U && usb.received_bytes == packet);
    assert(usb.received_length == sizeof(packet) && usb.rearm_calls == 0U);
    assert(usb_cdc_port_receive_from_isr(packet, 0U) == USBD_OK);
    assert(usb.received_packets == 2U && usb.received_length == 0U);
    assert(usb_cdc_port_receive_from_isr(NULL, 1U) == USBD_FAIL);
    assert(usb_cdc_port_receive_from_isr(packet, 65U) == USBD_FAIL);
    assert(usb.received_packets == 4U);
    assert(usb_cdc_port_close_from_isr() == USBD_OK);
    assert(usb.disconnections == 1U);
}

static void test_line_coding_roundtrip_and_bounds(void)
{
    reset_fixture();
    const uint8_t initial[] = {0x00U, 0xc2U, 0x01U, 0x00U, 0x00U, 0x00U, 0x08U};
    uint8_t guarded[9] = {0xa5U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0x5aU};
    assert(usb_cdc_port_control_from_isr(CDC_GET_LINE_CODING, &guarded[1], 7U) == USBD_OK);
    assert(memcmp(&guarded[1], initial, sizeof(initial)) == 0);
    uint8_t settings[] = {0xffU, 0xffU, 0xffU, 0xffU, 2U, 4U, 7U};
    assert(usb_cdc_port_control_from_isr(CDC_SET_LINE_CODING, settings, 7U) == USBD_OK);
    assert(usb_cdc_port_control_from_isr(CDC_GET_LINE_CODING, &guarded[1], 7U) == USBD_OK);
    assert(memcmp(&guarded[1], settings, sizeof(settings)) == 0);
    assert(guarded[0] == 0xa5U && guarded[8] == 0x5aU);
    assert(usb_cdc_port_control_from_isr(CDC_SET_LINE_CODING, NULL, 7U) == USBD_FAIL);
    assert(usb_cdc_port_control_from_isr(CDC_GET_LINE_CODING, guarded, 6U) == USBD_FAIL);
    assert(usb_cdc_port_control_from_isr(CDC_SET_LINE_CODING, guarded, 8U) == USBD_FAIL);
    assert(usb_cdc_port_control_from_isr(CDC_SET_CONTROL_LINE_STATE, NULL, 0U) == USBD_OK);
    assert(usb_cdc_port_control_from_isr(CDC_SEND_BREAK, NULL, 0U) == USBD_OK);
    assert(usb_cdc_port_control_from_isr(UINT8_MAX, guarded, 0U) == USBD_FAIL);
}

static void test_control_lengths_are_checked_before_vendor_buffer_access(void)
{
    reset_fixture();
    const struct {
        uint8_t direction;
        uint8_t command;
        uint16_t length;
        bool accepted;
    } cases[] = {
        {0x21U, CDC_SET_LINE_CODING, 7U, true},
        {0xa1U, CDC_GET_LINE_CODING, 7U, true},
        {0x21U, CDC_SET_CONTROL_LINE_STATE, 0U, true},
        {0x21U, CDC_SEND_BREAK, 0U, true},
        {0xa1U, CDC_SET_LINE_CODING, 7U, false},
        {0x21U, CDC_GET_LINE_CODING, 7U, false},
        {0x21U, CDC_SET_LINE_CODING, 6U, false},
        {0xa1U, CDC_GET_LINE_CODING, UINT16_MAX, false},
        {0x21U, CDC_SET_LINE_CODING, UINT16_MAX, false},
        {0x21U, CDC_SET_CONTROL_LINE_STATE, 1U, false},
        {0xa1U, CDC_SET_CONTROL_LINE_STATE, 0U, false},
        {0x21U, CDC_SEND_ENCAPSULATED_COMMAND, 1U, false},
        {0x80U, USB_REQ_GET_DESCRIPTOR, 18U, true},
    };
    for (size_t index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        USBD_SetupReqTypedef request = {
            .bmRequest = cases[index].direction,
            .bRequest = cases[index].command,
            .wLength = cases[index].length,
        };
        const unsigned calls_before = usb.setup_calls;
        const unsigned stalls_before = usb.setup_stalls;
        const uint8_t result = usb_cdc_port_class()->Setup(&hUsbDeviceFS, &request);
        assert(result == (cases[index].accepted ? USBD_OK : USBD_FAIL));
        assert(usb.setup_calls == calls_before + (cases[index].accepted ? 1U : 0U));
        assert(usb.setup_stalls == stalls_before + (cases[index].accepted ? 0U : 1U));
    }
    hUsbDeviceFS.pClassData = NULL;
    USBD_SetupReqTypedef request = {
        .bmRequest = 0x21U, .bRequest = CDC_SET_LINE_CODING, .wLength = 7U,
    };
    assert(usb_cdc_port_class()->Setup(&hUsbDeviceFS, &request) == USBD_FAIL);
}

int main(void)
{
    test_class_adapter_preserves_other_callbacks();
    test_completion_waits_for_terminating_packet();
    test_unrelated_or_failed_interrupts_do_not_complete_tx();
    test_transfer_rejections_preserve_buffer_ownership();
    test_critical_sections_nest_without_masking_capture();
    test_rearm_requires_configured_class_and_irq_allows_syscalls();
    test_rearm_retries_after_suspend_and_reports_stack_failure();
    test_static_class_storage_lifetime();
    test_isr_callbacks_only_notify_runtime();
    test_line_coding_roundtrip_and_bounds();
    test_control_lengths_are_checked_before_vendor_buffer_access();
    puts("ALL USB CDC PORT TESTS PASSED");
    return 0;
}
