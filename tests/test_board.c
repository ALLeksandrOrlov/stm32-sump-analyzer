#include "analyzer_board.h"
#include "capture_stm32.h"
#include "usbd_cdc_if.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static bool connected, busy, armed_rx;
static unsigned starts;
static uint32_t primask;
static capture_plan_t plan;
static uint8_t wire[128];
static size_t wire_length, tx_length;
static const uint8_t *tx;
uint32_t __get_PRIMASK(void) { return primask; }
void __disable_irq(void) { primask = 1; }
void __set_PRIMASK(uint32_t mask) { primask = mask; }
uint8_t Analyzer_CDC_Ready(void) { return connected && !busy; }
void Analyzer_CDC_Rearm(void) { if (connected) armed_rx = true; }
uint8_t CDC_Transmit_FS(uint8_t *bytes, uint16_t length)
{
    if (!connected || busy) return USBD_BUSY;
    tx = bytes;
    tx_length = length;
    busy = true;
    return USBD_OK;
}
static void finish(void)
{
    assert(busy && wire_length + tx_length <= sizeof(wire));
    memcpy(wire + wire_length, tx, tx_length);
    wire_length += tx_length;
    busy = false;
}
static bool start(void *ctx, const capture_plan_t *p)
{
    (void)ctx;
    plan = *p;
    ++starts;
    return true;
}
static void stop(void *ctx) { (void)ctx; }
static capture_hw_event_t poll(void *ctx) { (void)ctx; return CAPTURE_HW_NONE; }
static const uint8_t *buffer(void *ctx, uint32_t *next)
{
    (void)ctx;
    *next = 0;
    return NULL;
}
void capture_stm32_init(void) {}
uint32_t capture_stm32_timer_clock_hz(void) { return 168000000; }
capture_driver_t capture_stm32_driver(void)
{
    return (capture_driver_t){NULL, start, stop, poll, buffer};
}
static void packet(const uint8_t *bytes, uint32_t length)
{
    assert(armed_rx);
    armed_rx = false;
    analyzer_board_usb_receive(bytes, length);
}
static void connect(void)
{
    connected = true;
    busy = false;
    analyzer_board_usb_reset();
    armed_rx = true; /* initial OUT arm belongs to the ST CDC class */
}
int main(void)
{
    analyzer_board_init();
    connect();
    const uint8_t ids[] = {2, 2, 2};
    packet(ids, sizeof(ids)); /* packet arrives before reset is polled */
    analyzer_board_poll();
    assert(busy && !armed_rx);
    for (unsigned i = 0; i < 8; ++i) {
        analyzer_board_poll();
        if (busy) finish();
    }
    assert(wire_length == 12 && !memcmp(wire, "1ALS1ALS1ALS", 12));
    assert(armed_rx && primask == 0);

    const uint8_t partial[] = {0x81, 0};
    packet(partial, sizeof(partial));
    analyzer_board_poll();
    assert(armed_rx);
    const uint8_t rest[] = {0, 0, 0, 1};
    packet(rest, sizeof(rest));
    analyzer_board_poll();
    assert(starts == 1 && plan.config.samples == 4 && plan.config.post_trigger == 4);

    /* A disconnect discards partial parser state. Preserve the very first
     * command of a new USB session even before foreground sees the reset. */
    packet(partial, sizeof(partial));
    analyzer_board_poll();
    connected = false;
    analyzer_board_usb_reset();
    connect();
    packet(ids, 1);
    analyzer_board_poll();
    assert(busy);
    finish();
    analyzer_board_poll();
    assert(wire_length == 16 && !memcmp(wire + 12, "1ALS", 4));

    packet(ids, 1);
    analyzer_board_poll();
    assert(busy);
    connected = false;
    analyzer_board_usb_reset();
    analyzer_board_poll();
    connect();
    packet(ids, 1);
    analyzer_board_poll();
    assert(busy);
    finish();
    analyzer_board_poll();
    assert(wire_length == 20 && !memcmp(wire + 16, "1ALS", 4));

    packet(ids, 0); /* Zero-length packet cannot wedge OUT. */
    assert(armed_rx);
    uint8_t malformed[65] = {0};
    packet(malformed, sizeof(malformed));
    analyzer_board_poll();
    assert(armed_rx);
    primask = 1;
    analyzer_board_poll();
    assert(primask == 1); /* critical sections restore prior IRQ state */
    puts("ALL BOARD ADAPTER TESTS PASSED");
    return 0;
}
