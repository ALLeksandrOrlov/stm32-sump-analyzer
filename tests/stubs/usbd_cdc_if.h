#ifndef TEST_USBD_CDC_IF_H
#define TEST_USBD_CDC_IF_H
#include <stdint.h>
#define USBD_OK 0u
#define USBD_BUSY 1u
uint8_t Analyzer_CDC_Ready(void);
void Analyzer_CDC_Rearm(void);
uint8_t CDC_Transmit_FS(uint8_t *bytes, uint16_t length);
uint32_t __get_PRIMASK(void);
void __disable_irq(void);
void __set_PRIMASK(uint32_t mask);
static inline void __DMB(void) {}
#endif
