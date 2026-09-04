#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Byte limit, not character limit. Oversized results are rejected, never cut. */
#define QR_USB_MAX_PAYLOAD 1024
#define QR_REARM_MS 1000

typedef struct {
    uint8_t payload[QR_USB_MAX_PAYLOAD];
    size_t length;
    uint32_t eci;
    int data_type;
    int64_t last_seen_ms;
    bool valid;
} qr_repeat_filter_t;

bool qr_rgb565_to_gray(const uint8_t *src, size_t src_len,
                      uint8_t *gray, size_t pixels);
bool qr_repeat_should_send(qr_repeat_filter_t *filter, const uint8_t *payload,
                          size_t length, uint32_t eci, int data_type, int64_t now_ms);
void qr_repeat_mark_sent(qr_repeat_filter_t *filter, const uint8_t *payload,
                         size_t length, uint32_t eci, int data_type, int64_t now_ms);
