#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    QR_HID_TYPED = 1,
    QR_HID_NOT_READY,
    QR_HID_UNSUPPORTED,
    QR_HID_ABORTED,
} qr_hid_result_t;

esp_err_t qr_hid_init(void);
bool qr_hid_ready(void);
/* Single scanner caller. Waits for completion, or the USB task's bounded timeout.
 * ABORTED may have typed a prefix; never automatically replay that request. */
qr_hid_result_t qr_hid_type(const uint8_t *payload, size_t length);
