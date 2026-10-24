/** @file
 * @brief Static lifetime adapter for the ST USB class allocation interface.
 */
#include "usb_cdc_storage.h"
#include "usbd_cdc.h"
#include <stdbool.h>

static USBD_CDC_HandleTypeDef class_storage;
static bool storage_in_use;

void *usb_cdc_storage_acquire(size_t size)
{
    void *result = NULL;
    if ((size == sizeof(class_storage)) && !storage_in_use) {
        /* A typed assignment initializes pointer members without depending on
         * an all-zero pointer representation or a byte-array alignment. */
        class_storage = (USBD_CDC_HandleTypeDef){0};
        storage_in_use = true;
        result = &class_storage;
    }
    return result;
}

void usb_cdc_storage_release(void *storage)
{
    if (storage == &class_storage) {
        storage_in_use = false;
    }
}
