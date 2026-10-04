#ifndef ANALYZER_BOARD_H
#define ANALYZER_BOARD_H

#include <stdint.h>

void analyzer_board_init(void);
void analyzer_board_poll(void);
/* USB interrupt entry points. One mailbox; OUT endpoint is rearmed only after
 * foreground consumption, so USB NAK supplies lossless backpressure. */
void analyzer_board_usb_receive(const uint8_t *bytes, uint32_t length);
void analyzer_board_usb_reset(void);

#endif
