#include "analyzer_board.h"
#include "capture_stm32.h"
#include "usbd_cdc_if.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* The USB mock borrows TX memory until finish_transfer(), as the CDC driver
 * does. OUT packets may arrive only after the adapter rearms the endpoint. */
static struct {
    bool connected;
    bool busy;
    bool rx_armed;
    bool blocked;
    bool reset_after_ready;
    bool reconnect_after_ready;
    unsigned reject_writes;
    unsigned rearm_count;
    uint8_t wire[512];
    size_t wire_length;
    const uint8_t *tx;
    size_t tx_length;
} usb;
static struct {
    unsigned starts;
    unsigned stops;
    capture_plan_t plan;
} capture;
static uint32_t primask;

uint32_t __get_PRIMASK(void) { return primask; }
void __disable_irq(void) { primask = 1; }
void __set_PRIMASK(uint32_t mask) { primask = mask; }

uint8_t Analyzer_CDC_Ready(void)
{
    bool ready = usb.connected && !usb.busy && !usb.blocked;
    if (usb.reset_after_ready) {
        usb.reset_after_ready = false;
        usb.connected = false;
        analyzer_board_usb_reset();
    }
    if (usb.reconnect_after_ready) {
        usb.reconnect_after_ready = false;
        /* USB reset, reconfiguration and the first new packet interrupt a
         * foreground poll before it can process the session reset. */
        analyzer_board_usb_reset();
        usb.busy = false;
        usb.rx_armed = false;
        const uint8_t identity = 2;
        analyzer_board_usb_receive(&identity, 1);
    }
    return ready;
}

void Analyzer_CDC_Rearm(void)
{
    if (usb.connected) {
        /* Double rearming can replace a packet that just arrived. */
        assert(!usb.rx_armed);
        usb.rx_armed = true;
        ++usb.rearm_count;
    }
}

uint8_t CDC_Transmit_FS(uint8_t *bytes, uint16_t length)
{
    assert(primask == 1); /* Submission and disconnect must be atomic. */
    if (!usb.connected || usb.busy || usb.blocked) return USBD_BUSY;
    if (usb.reject_writes) {
        --usb.reject_writes;
        return USBD_BUSY;
    }
    usb.tx = bytes;
    usb.tx_length = length;
    usb.busy = true;
    return USBD_OK;
}

static void finish_transfer(void)
{
    assert(usb.busy);
    assert(usb.wire_length + usb.tx_length <= sizeof(usb.wire));
    memcpy(usb.wire + usb.wire_length, usb.tx, usb.tx_length);
    usb.wire_length += usb.tx_length;
    usb.busy = false;
}

static bool start_capture(void *context, const capture_plan_t *plan)
{
    (void)context;
    capture.plan = *plan;
    ++capture.starts;
    return true;
}

static void stop_capture(void *context)
{
    (void)context;
    ++capture.stops;
}

static capture_hw_event_t poll_capture(void *context)
{
    (void)context;
    return CAPTURE_HW_NONE;
}

static const uint8_t *capture_buffer(void *context, uint32_t *next)
{
    (void)context;
    *next = 0;
    return NULL;
}

void capture_stm32_init(void) {}
uint32_t capture_stm32_timer_clock_hz(void) { return 168000000; }
capture_driver_t capture_stm32_driver(void)
{
    return (capture_driver_t){NULL, start_capture, stop_capture,
                              poll_capture, capture_buffer};
}

static void connect_usb(void)
{
    usb.connected = true;
    usb.busy = false;
    analyzer_board_usb_reset();
    usb.rx_armed = true; /* Initial OUT arm belongs to the CDC class. */
}

static void reset_fixture(void)
{
    memset(&usb, 0, sizeof(usb));
    memset(&capture, 0, sizeof(capture));
    primask = 0;
    analyzer_board_init();
    connect_usb();
    analyzer_board_poll();
}

static void receive_packet(const uint8_t *bytes, uint32_t length)
{
    assert(usb.rx_armed);
    usb.rx_armed = false;
    analyzer_board_usb_receive(bytes, length);
}

static void drain_replies(void)
{
    for (unsigned i = 0; i < 256; ++i) {
        analyzer_board_poll();
        if (usb.busy) finish_transfer();
        else if (usb.rx_armed) return;
    }
    assert(!"USB replies did not drain");
}

static void assert_identity(size_t offset)
{
    assert(usb.wire_length >= offset + 4);
    assert(memcmp(usb.wire + offset, "1ALS", 4) == 0);
}

static void test_packet_copy_and_backpressure(void)
{
    reset_fixture();
    uint8_t commands[64];
    memset(commands, 2, sizeof(commands));
    receive_packet(commands, sizeof(commands));
    memset(commands, 0, sizeof(commands)); /* CDC may reuse its RX buffer. */
    analyzer_board_poll();
    assert(usb.busy && !usb.rx_armed);
    unsigned rearm_count = usb.rearm_count;
    for (unsigned i = 0; i < 8; ++i) analyzer_board_poll();
    assert(usb.busy && !usb.rx_armed && usb.rearm_count == rearm_count);
    drain_replies();
    assert(usb.wire_length == 64 * 4);
    for (size_t offset = 0; offset < usb.wire_length; offset += 4)
        assert_identity(offset);
    assert(usb.rearm_count == rearm_count + 1);
}

static void test_fragmented_command_runs_in_foreground(void)
{
    reset_fixture();
    const uint8_t first[] = {0x81, 0};
    const uint8_t second[] = {0, 0, 0, 1};
    receive_packet(first, sizeof(first));
    analyzer_board_poll();
    assert(usb.rx_armed && capture.starts == 0);
    receive_packet(second, sizeof(second));
    assert(capture.starts == 0);
    analyzer_board_poll();
    assert(capture.starts == 1);
    assert(capture.plan.config.samples == 4);
    assert(capture.plan.config.post_trigger == 4);
}

static void test_reconnect_discards_partial_command(void)
{
    reset_fixture();
    const uint8_t partial[] = {0x81, 0};
    receive_packet(partial, sizeof(partial));
    analyzer_board_poll();
    usb.connected = false;
    analyzer_board_usb_reset();
    connect_usb();
    const uint8_t identity = 2;
    /* The first new packet can arrive before foreground observes the reset. */
    receive_packet(&identity, 1);
    drain_replies();
    assert(usb.wire_length == 4);
    assert_identity(0);
    assert(capture.starts == 0);
}

static void test_disconnect_cancels_pending_transfer_and_capture(void)
{
    reset_fixture();
    const uint8_t commands[] = {1, 2, 2};
    receive_packet(commands, sizeof(commands));
    analyzer_board_poll();
    assert(capture.starts == 1 && usb.busy && !usb.rx_armed);
    unsigned stops = capture.stops;
    usb.connected = false;
    analyzer_board_usb_reset();
    analyzer_board_poll();
    assert(capture.stops == stops + 1);
    connect_usb();
    const uint8_t identity = 2;
    receive_packet(&identity, 1);
    drain_replies();
    assert(usb.wire_length == 4); /* No reply from the disconnected session. */
    assert_identity(0);
}

static void test_reset_between_ready_and_write(void)
{
    reset_fixture();
    const uint8_t identity = 2;
    usb.blocked = true;
    receive_packet(&identity, 1);
    analyzer_board_poll(); /* Queue a reply while USB cannot accept writes. */
    usb.blocked = false;
    usb.reset_after_ready = true;
    analyzer_board_poll();
    assert(!usb.busy && usb.wire_length == 0);
    connect_usb();
    receive_packet(&identity, 1);
    drain_replies();
    assert(usb.wire_length == 4);
    assert_identity(0);
}

static void test_reconnect_during_poll_preserves_first_packet(void)
{
    reset_fixture();
    const uint8_t run = 1;
    receive_packet(&run, 1);
    usb.reconnect_after_ready = true;
    analyzer_board_poll();
    assert(capture.starts == 0); /* Old session's RUN was discarded. */
    assert(!usb.busy && !usb.rx_armed); /* New session awaits foreground reset. */
    drain_replies();
    assert(usb.wire_length == 4);
    assert_identity(0);
}

static void test_rejected_write_is_retried_once(void)
{
    reset_fixture();
    const uint8_t identity = 2;
    receive_packet(&identity, 1);
    usb.reject_writes = 1;
    analyzer_board_poll();
    assert(!usb.busy && usb.wire_length == 0);
    drain_replies();
    assert(usb.wire_length == 4);
    assert_identity(0);
}

static void test_invalid_packets_recover(void)
{
    reset_fixture();
    receive_packet(NULL, 0);
    assert(usb.rx_armed); /* A ZLP must not stall OUT. */
    uint8_t oversized[65] = {0};
    receive_packet(oversized, sizeof(oversized));
    analyzer_board_poll();
    assert(usb.rx_armed);
    const uint8_t identity = 2;
    receive_packet(&identity, 1);
    /* Violate the endpoint contract: fail closed instead of mixing packets. */
    analyzer_board_usb_receive(&identity, 1);
    analyzer_board_poll();
    assert(usb.rx_armed && !usb.busy);
    receive_packet(&identity, 1);
    drain_replies();
    assert(usb.wire_length == 4);
    assert_identity(0);
}

static void test_interrupt_mask_is_preserved(void)
{
    reset_fixture();
    for (uint32_t initial_mask = 0; initial_mask <= 1; ++initial_mask) {
        primask = initial_mask;
        analyzer_board_usb_reset();
        analyzer_board_poll();
        assert(primask == initial_mask);
        const uint8_t identity = 2;
        receive_packet(&identity, 1);
        drain_replies();
        assert(primask == initial_mask);
    }
}

int main(void)
{
    test_packet_copy_and_backpressure();
    test_fragmented_command_runs_in_foreground();
    test_reconnect_discards_partial_command();
    test_disconnect_cancels_pending_transfer_and_capture();
    test_reset_between_ready_and_write();
    test_reconnect_during_poll_preserves_first_packet();
    test_rejected_write_is_retried_once();
    test_invalid_packets_recover();
    test_interrupt_mask_is_preserved();
    puts("ALL BOARD ADAPTER TESTS PASSED");
    return 0;
}
