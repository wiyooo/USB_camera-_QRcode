#include "qr_payload.h"

#include <string.h>

bool qr_rgb565_to_gray(const uint8_t *src, size_t src_len,
                      uint8_t *gray, size_t pixels)
{
    if (!src || !gray || pixels > SIZE_MAX / 2 || src_len != pixels * 2) {
        return false;
    }
    for (size_t i = 0; i < pixels; ++i) {
        const uint16_t rgb = ((uint16_t)src[2 * i] << 8) | src[2 * i + 1];
        const unsigned r5 = (rgb >> 11) & 31;
        const unsigned g6 = (rgb >> 5) & 63;
        const unsigned b5 = rgb & 31;
        const unsigned r = (r5 << 3) | (r5 >> 2);
        const unsigned g = (g6 << 2) | (g6 >> 4);
        const unsigned b = (b5 << 3) | (b5 >> 2);
        gray[i] = (uint8_t)((77 * r + 150 * g + 29 * b) >> 8);
    }
    return true;
}

bool qr_repeat_should_send(qr_repeat_filter_t *filter, const uint8_t *payload,
                          size_t length, uint32_t eci, int data_type, int64_t now_ms)
{
    if (!payload || length > QR_USB_MAX_PAYLOAD) {
        return false;
    }
    if (filter->valid && filter->length == length && filter->eci == eci &&
        filter->data_type == data_type &&
        memcmp(filter->payload, payload, length) == 0 &&
        now_ms >= filter->last_seen_ms && now_ms - filter->last_seen_ms < QR_REARM_MS) {
        filter->last_seen_ms = now_ms;
        return false;
    }
    /* A failed/blocked send must remain eligible on the next frame. */
    filter->valid = false;
    return true;
}

void qr_repeat_mark_sent(qr_repeat_filter_t *filter, const uint8_t *payload,
                         size_t length, uint32_t eci, int data_type, int64_t now_ms)
{
    if (!payload || length > QR_USB_MAX_PAYLOAD) {
        return;
    }
    memcpy(filter->payload, payload, length);
    filter->length = length;
    filter->eci = eci;
    filter->data_type = data_type;
    filter->last_seen_ms = now_ms;
    filter->valid = true;
}
