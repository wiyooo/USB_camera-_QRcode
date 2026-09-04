#include "qr_keyboard.h"
#include "qr_payload.h"

#include <string.h>

_Static_assert(sizeof(qr_keyboard_report_t) == 8, "Boot keyboard reports are eight bytes");

static bool ascii_key(uint8_t ch, bool caps_lock, qr_keyboard_report_t *report)
{
    uint8_t key = 0;
    bool shift = false;
    if (ch >= 'a' && ch <= 'z') {
        key = 0x04 + ch - 'a';
        shift = caps_lock;
    } else if (ch >= 'A' && ch <= 'Z') {
        key = 0x04 + ch - 'A';
        shift = !caps_lock;
    } else if (ch >= '1' && ch <= '9') {
        key = 0x1e + ch - '1';
    } else if (ch == '0') {
        key = 0x27;
    } else if (ch == '\r' || ch == '\n') {
        key = 0x28;
    } else if (ch == '\t') {
        key = 0x2b;
    } else if (ch == ' ') {
        key = 0x2c;
    } else {
        static const char plain[] = "-=[]\\;'`,./";
        static const char shifted[] = "_+{}|:\"~<>?";
        static const uint8_t usages[] = {0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38};
        for (size_t i = 0; i < sizeof(usages); ++i) {
            if (ch == (uint8_t)plain[i] || ch == (uint8_t)shifted[i]) {
                key = usages[i];
                shift = ch == (uint8_t)shifted[i];
                break;
            }
        }
        static const char number_shift[] = "!@#$%^&*()";
        for (size_t i = 0; !key && i < sizeof(number_shift) - 1; ++i) {
            if (ch == (uint8_t)number_shift[i]) {
                key = 0x1e + i;
                shift = true;
            }
        }
    }
    if (!key) {
        return false;
    }
    memset(report, 0, sizeof(*report));
    report->modifier = shift ? 0x02 : 0; /* Left Shift only. */
    report->keycode[0] = key;
    return true;
}

bool qr_keyboard_validate(const uint8_t *payload, size_t length)
{
    if (!payload || length == 0 || length > QR_USB_MAX_PAYLOAD) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        if ((payload[i] < 0x20 || payload[i] > 0x7e) &&
            payload[i] != '\r' && payload[i] != '\n' && payload[i] != '\t') {
            return false;
        }
    }
    return true;
}

bool qr_keyboard_begin(qr_keyboard_sequence_t *sequence, const uint8_t *payload,
                       size_t length, bool append_enter)
{
    if (!sequence || !qr_keyboard_validate(payload, length)) {
        return false;
    }
    *sequence = (qr_keyboard_sequence_t) {
        .payload = payload,
        .length = length,
        .append_enter = append_enter && payload[length - 1] != '\r' && payload[length - 1] != '\n',
        .phase = QR_KEY_CLEAR,
    };
    return true;
}

bool qr_keyboard_report(const qr_keyboard_sequence_t *sequence, bool caps_lock,
                        qr_keyboard_report_t *report)
{
    memset(report, 0, sizeof(*report));
    switch (sequence->phase) {
    case QR_KEY_DONE:
        return false;
    case QR_KEY_DOWN:
        return ascii_key(sequence->payload[sequence->position], caps_lock, report);
    case QR_KEY_ENTER_DOWN:
        report->keycode[0] = 0x28;
        return true;
    default:
        return true; /* Clear/release reports contain eight zero bytes. */
    }
}

void qr_keyboard_complete(qr_keyboard_sequence_t *sequence)
{
    switch (sequence->phase) {
    case QR_KEY_CLEAR:
        sequence->phase = QR_KEY_DOWN;
        break;
    case QR_KEY_DOWN:
        sequence->phase = QR_KEY_UP;
        break;
    case QR_KEY_UP:
        if (sequence->payload[sequence->position++] == '\r' &&
            sequence->position < sequence->length && sequence->payload[sequence->position] == '\n') {
            ++sequence->position; /* CRLF becomes one Enter. */
        }
        sequence->phase = sequence->position < sequence->length ? QR_KEY_DOWN :
                          sequence->append_enter ? QR_KEY_ENTER_DOWN : QR_KEY_DONE;
        break;
    case QR_KEY_ENTER_DOWN:
        sequence->phase = QR_KEY_ENTER_UP;
        break;
    case QR_KEY_ENTER_UP:
        sequence->phase = QR_KEY_DONE;
        break;
    case QR_KEY_DONE:
        break;
    }
}
