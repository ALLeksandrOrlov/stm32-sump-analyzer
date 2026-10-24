#ifndef ANALYZER_USBD_CONF_H
#define ANALYZER_USBD_CONF_H

/** @file
 * @brief Keep CubeMX USB settings and replace its heap allocation policy.
 *
 * The build searches this directory before Inc. The generated header remains
 * untouched; the CDC middleware sees only these static-storage aliases.
 */
#include "../../Inc/usbd_conf.h"
#include "usb_cdc_storage.h"

#undef USBD_malloc
#undef USBD_free
#define USBD_malloc usb_cdc_storage_acquire
#define USBD_free usb_cdc_storage_release

#endif
