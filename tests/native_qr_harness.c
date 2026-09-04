#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef NDEBUG
#error "This test harness requires assertions; compile with -UNDEBUG"
#endif

#include "qr_payload.h"
#include "qr_keyboard.h"
#include "quirc.h"
#include "quirc_internal.h"

static void test_payload(void)
{
    const uint8_t rgb[] = {0, 0, 255, 255, 0xf8, 0, 0x07, 0xe0, 0, 0x1f};
    uint8_t gray[7] = {0xa5, 0, 0, 0, 0, 0, 0xa5};
    assert(qr_rgb565_to_gray(rgb, sizeof(rgb), gray + 1, 5));
    assert(gray[0] == 0xa5 && gray[6] == 0xa5);
    assert(gray[1] == 0 && gray[2] == 255 && gray[3] == 76 && gray[4] == 149 && gray[5] == 28);
    assert(!qr_rgb565_to_gray(rgb, sizeof(rgb) - 1, gray + 1, 5));
    assert(!qr_rgb565_to_gray(rgb, sizeof(rgb), gray + 1, SIZE_MAX));

    const uint8_t a[] = {'a', 0, '\n', '"', '\\', 0xff};
    uint8_t large[QR_USB_MAX_PAYLOAD + 1];
    memset(large, 'A', sizeof(large));
    assert(qr_keyboard_validate(large, QR_USB_MAX_PAYLOAD));
    assert(!qr_keyboard_validate(large, sizeof(large)));
    assert(!qr_keyboard_validate(a, sizeof(a)));
    assert(!qr_keyboard_validate(a, 0));

    qr_repeat_filter_t repeat = {0};
    assert(qr_repeat_should_send(&repeat, a, sizeof(a), 26, 4, 0));
    /* Simulate USB backpressure: no mark_sent, so the next frame must retry. */
    assert(qr_repeat_should_send(&repeat, a, sizeof(a), 26, 4, 50));
    qr_repeat_mark_sent(&repeat, a, sizeof(a), 26, 4, 50);
    for (int64_t t = 100; t <= 10000; t += 100) {
        assert(!qr_repeat_should_send(&repeat, a, sizeof(a), 26, 4, t));
    }
    assert(qr_repeat_should_send(&repeat, a, sizeof(a), 26, 4, 11000));
    assert(qr_repeat_should_send(&repeat, a, sizeof(a), 26, 4, 11100));
    qr_repeat_mark_sent(&repeat, a, sizeof(a), 26, 4, 11100);
    assert(qr_repeat_should_send(&repeat, a, sizeof(a), 20, 4, 11101));
    qr_repeat_mark_sent(&repeat, a, sizeof(a), 26, 4, 12000);
    assert(qr_repeat_should_send(&repeat, large, 1, 0, 4, 12001));
    assert(!qr_repeat_should_send(&repeat, large, sizeof(large), 0, 4, 12002));
}

/* JSON is only a host-test protocol. The firmware emits HID reports. */
static void print_hid(const uint8_t *payload, size_t length, bool caps_lock)
{
    printf("{\"hex\":\"");
    for (size_t i = 0; i < length; ++i) printf("%02x", payload[i]);
    bool supported = qr_keyboard_validate(payload, length);
    printf("\",\"hid_supported\":%s,\"reports\":[", supported ? "true" : "false");
    if (supported) {
        qr_keyboard_sequence_t sequence;
        assert(qr_keyboard_begin(&sequence, payload, length, true));
        bool first = true;
        qr_keyboard_report_t report;
        while (qr_keyboard_report(&sequence, caps_lock, &report)) {
            assert(report.reserved == 0);
            for (int i = 1; i < 6; ++i) assert(report.keycode[i] == 0);
            qr_keyboard_report_t repeated;
            assert(qr_keyboard_report(&sequence, caps_lock, &repeated));
            assert(memcmp(&report, &repeated, sizeof(report)) == 0); /* Wait for completion. */
            printf("%s[%u,%u]", first ? "" : ",", report.modifier, report.keycode[0]);
            first = false;
            qr_keyboard_complete(&sequence);
        }
    }
    printf("]}\n");
}

int main(int argc, char **argv)
{
    test_payload();
    if (argc == 1) {
        printf("PASS: RGB565, HID bounds, repeat/rearm and refused-send retry\n");
        printf("quirc working allocations: %zu bytes (native struct layout)\n",
               sizeof(struct quirc) + 320 * 240 + 160 * sizeof(struct quirc_flood_fill_vars));
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "--keys") == 0) {
        uint8_t text[98];
        for (int i = 0; i < 95; ++i) text[i] = 32 + i;
        text[95] = '\r'; text[96] = '\n'; text[97] = '\t';
        print_hid(text, sizeof(text), atoi(argv[2]) != 0);
        return 0;
    }
    assert(argc == 2);
    uint8_t *rgb = malloc(320 * 240 * 2);
    assert(rgb);
    FILE *input = fopen(argv[1], "rb");
    assert(input && fread(rgb, 1, 320 * 240 * 2, input) == 320 * 240 * 2);
    assert(fgetc(input) == EOF);
    fclose(input);
    struct quirc *q = quirc_new();
    assert(q && quirc_resize(q, 320, 240) == 0);
    assert(qr_rgb565_to_gray(rgb, 320 * 240 * 2, quirc_begin(q, NULL, NULL), 320 * 240));
    free(rgb);
    quirc_end(q);
    fprintf(stderr, "identified: %d grids, %d capstones, %d regions\n",
            quirc_count(q), q->num_capstones, q->num_regions);
    int found = 0;
    for (int i = 0; i < quirc_count(q); ++i) {
        struct quirc_code code;
        struct quirc_data data;
        quirc_extract(q, i, &code);
        quirc_decode_error_t err = quirc_decode(&code, &data);
        if (err != QUIRC_SUCCESS) {
            quirc_flip(&code);
            err = quirc_decode(&code, &data);
        }
        if (err == QUIRC_SUCCESS) {
            ++found;
            print_hid(data.payload, data.payload_len, false);
        } else {
            fprintf(stderr, "decode: %s\n", quirc_strerror(err));
        }
    }
    quirc_destroy(q);
    return found ? 0 : 2;
}
