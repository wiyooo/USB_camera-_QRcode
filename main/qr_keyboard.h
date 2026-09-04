#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Standard boot keyboard report, USB HID usage page 0x07, US layout. */
typedef struct {
    uint8_t modifier;
    uint8_t reserved;
    uint8_t keycode[6];
} qr_keyboard_report_t;

typedef enum {
    QR_KEY_CLEAR, QR_KEY_DOWN, QR_KEY_UP,
    QR_KEY_ENTER_DOWN, QR_KEY_ENTER_UP, QR_KEY_DONE
} qr_keyboard_phase_t;

typedef struct {
    const uint8_t *payload;
    size_t length;
    size_t position;
    bool append_enter;
    qr_keyboard_phase_t phase;
} qr_keyboard_sequence_t;

bool qr_keyboard_validate(const uint8_t *payload, size_t length);
bool qr_keyboard_begin(qr_keyboard_sequence_t *sequence, const uint8_t *payload,
                       size_t length, bool append_enter);
bool qr_keyboard_report(const qr_keyboard_sequence_t *sequence, bool caps_lock,
                        qr_keyboard_report_t *report);
/* Advance only after the host completes the preceding HID report. */
void qr_keyboard_complete(qr_keyboard_sequence_t *sequence);
