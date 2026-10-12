#include "analyzer_board.h"
#include "analyzer_app.h"
#include "capture_stm32.h"
#include "usbd_cdc_if.h"
#include <string.h>

/* Only foreground code touches app and rx_offset. USB callbacks publish a
 * complete packet through rx_length, then leave OUT unarmed until consumption.
 * Reset is deferred so the parser/controller are never mutated by an IRQ. */
static analyzer_app_t app;
static uint8_t rx[64];
static volatile uint32_t rx_length;
static uint32_t rx_offset;
static volatile bool reset_pending;
static volatile bool rx_fault;

static bool usb_tx_ready(void *context)
{
    (void)context;
    return Analyzer_CDC_Ready();
}

static bool usb_write(void *context, const uint8_t *bytes, size_t length)
{
    (void)context;
    uint32_t mask = __get_PRIMASK();
    __disable_irq();
    bool accepted = !reset_pending &&
        CDC_Transmit_FS((uint8_t *)bytes, (uint16_t)length) == USBD_OK;
    __set_PRIMASK(mask);
    return accepted;
}

void analyzer_board_init(void)
{
    capture_stm32_init();
    analyzer_app_init(&app, capture_stm32_driver(),
                      (analyzer_transport_t){NULL, usb_tx_ready, usb_write},
                      capture_stm32_timer_clock_hz());
}

void analyzer_board_usb_receive(const uint8_t *bytes, uint32_t length)
{
    if (length > sizeof(rx) || rx_length) {
        rx_length = 0;
        rx_fault = true;
        reset_pending = true;
        return;
    }
    if (!length) {
        Analyzer_CDC_Rearm();
        return;
    }
    memcpy(rx, bytes, length);
    __DMB();
    rx_length = length;
}

void analyzer_board_usb_reset(void)
{
    rx_length = 0;
    rx_fault = false;
    reset_pending = true;
}

void analyzer_board_poll(void)
{
    uint32_t mask = __get_PRIMASK();
    __disable_irq();
    if (reset_pending) {
        reset_pending = false;
        rx_offset = 0;
        analyzer_app_reset(&app);
        /* CDC Init already arms OUT. Rearm here only after a malformed RX;
         * otherwise an arriving first packet could be interrupted. */
        if (rx_fault) {
            rx_fault = false;
            Analyzer_CDC_Rearm();
        }
    }
    __set_PRIMASK(mask);
    analyzer_app_poll(&app);
    /* At most one packet per iteration. Commands and USB writes run with
     * interrupts enabled; the short rearm operation is protected from reset. */
    while (rx_offset < rx_length && !reset_pending) {
        if (!analyzer_app_receive(&app, rx[rx_offset])) break;
        ++rx_offset;
    }
    mask = __get_PRIMASK();
    __disable_irq();
    if (!reset_pending && rx_length && rx_offset == rx_length) {
        rx_offset = 0;
        rx_length = 0;
        Analyzer_CDC_Rearm();
    }
    __set_PRIMASK(mask);
    analyzer_app_poll(&app);
}
