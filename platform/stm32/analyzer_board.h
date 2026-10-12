#ifndef ANALYZER_BOARD_H
#define ANALYZER_BOARD_H

#include <stdint.h>

/* Initialize once, before the USB device is started. Poll from the main loop. */
void analyzer_board_init(void);
void analyzer_board_poll(void);
/* USB interrupt entry points. One mailbox; OUT endpoint is rearmed only after
 * foreground consumption, so USB NAK supplies lossless backpressure.
 * Receive copies one full-speed packet (up to 64 bytes); bytes may be NULL only
 * for a zero-length packet. Reset discards commands from the previous session. */
void analyzer_board_usb_receive(const uint8_t *bytes, uint32_t length);
void analyzer_board_usb_reset(void);

#endif
