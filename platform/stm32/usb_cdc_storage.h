#ifndef USB_CDC_STORAGE_H
#define USB_CDC_STORAGE_H

#include <stddef.h>

/**
 * @brief Borrow the single, statically allocated CDC class object.
 * @param size Must equal sizeof(USBD_CDC_HandleTypeDef).
 * @return Zero-initialized, correctly aligned class object, or NULL for an
 *         invalid size or an already active owner. No heap is used.
 * @note Called only by the serialized USB class lifecycle in the USB ISR.
 */
void *usb_cdc_storage_acquire(size_t size);

/**
 * @brief End the class object's lifetime after the USB endpoints are closed.
 * @param storage Object returned by acquire; NULL and foreign pointers do
 *                nothing. The next acquisition clears all previous state.
 * @note Caller must not retain or use the object after release.
 */
void usb_cdc_storage_release(void *storage);

#endif
