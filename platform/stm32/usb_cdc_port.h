#ifndef USB_CDC_PORT_H
#define USB_CDC_PORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "usbd_def.h"

/** @brief Prepare the static class adapter before MX_USB_DEVICE_Init(). */
void usb_cdc_port_init(void);

/** @return Module-owned CDC class callbacks, valid for the whole program. */
USBD_ClassTypeDef *usb_cdc_port_class(void);

/**
 * @brief Configure a syscall-safe USB priority and leave its interrupt masked.
 * @note Called by generated hardware initialization before scheduler startup.
 */
void usb_cdc_port_configure_irq(void);

/** @brief Enable USB interrupts from the TX task after scheduler startup. */
void usb_cdc_port_activate(void *context);

/**
 * @brief Offer a packet to the CDC transport from the TX task.
 * @param context Unused adapter context.
 * @param bytes Borrowed, immutable storage until TX completion or disconnect.
 * @param length Number of bytes, in the range 1..UINT16_MAX.
 * @return true if ownership was accepted; false leaves it with the caller.
 * @note Completion includes the terminating zero-length USB packet, if needed.
 */
bool usb_cdc_port_write(void *context, const uint8_t *bytes, size_t length);

/** @return true in task context only when a configured class can accept TX. */
bool usb_cdc_port_idle(void *context);

/**
 * @brief Rearm reception from the RX task after its packet has been copied.
 * @return true only if the configured endpoint accepted a receive buffer.
 *         A suspended endpoint returns false; resume wakes the task to retry.
 */
bool usb_cdc_port_rearm(void *context);

/**
 * @brief Restore the USB device state, then wake the RX and TX tasks.
 * @note USB resume ISR only, after the HAL callback ungates the PHY clock.
 *       This preserves parser state and ownership of any active IN packet.
 */
USBD_StatusTypeDef usb_cdc_port_resume_from_isr(USBD_HandleTypeDef *device);

/** @brief Bind generated endpoint buffers and notify a configured USB session. */
uint8_t usb_cdc_port_open_from_isr(void);

/** @brief Notify session loss after the USB stack closes all endpoints. */
uint8_t usb_cdc_port_close_from_isr(void);

/**
 * @brief Handle CDC serial settings without running analyzer commands.
 * @note USB ISR only. Serial settings are metadata; no physical UART exists.
 */
uint8_t usb_cdc_port_control_from_isr(uint8_t command, uint8_t *data, uint16_t length);

/**
 * @brief Copy a received USB packet to the runtime's ISR message channel.
 * @note USB ISR only. The RX task owns framing, command parsing and rearming.
 */
uint8_t usb_cdc_port_receive_from_isr(const uint8_t *data, uint32_t length);

#endif
