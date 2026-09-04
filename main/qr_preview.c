#include "esp32_s3_szp.h"

#if CAMERA_UVC_QR_TEST

#include <inttypes.h>
#include <stdbool.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "qr_payload.h"
#include "qr_preview.h"
#include "quirc.h"

#define QR_PREVIEW_PIXELS (CAMERA_FRAME_WIDTH * CAMERA_FRAME_HEIGHT)
#define QR_SCAN_INTERVAL_MS 250
#define QR_LOG_INTERVAL_MS 2000
#define QR_LOG_MAX_BYTES 256
#define QR_WORKER_STACK_BYTES (16 * 1024)

static const char *TAG = "qr_preview";
static struct quirc *s_decoder;
static struct quirc_code s_code;
static struct quirc_data s_data;
static TaskHandle_t s_worker;
static SemaphoreHandle_t s_done;
static const camera_fb_t *s_input;
static int64_t s_next_scan_ms;
static int64_t s_next_payload_log_ms;
static int64_t s_next_stats_ms;
static uint32_t s_scans, s_candidates, s_decoded;

static void log_payload(void)
{
    /* Escape controls and embedded NUL so binary data cannot hide later bytes.
     * Keep UTF-8 bytes for Chinese text. Bound UART traffic to preserve preview. */
    char text[QR_LOG_MAX_BYTES * 4 + 1];
    static const char hex[] = "0123456789ABCDEF";
    const size_t length = (size_t)s_data.payload_len;
    const size_t shown = length < QR_LOG_MAX_BYTES ? length : QR_LOG_MAX_BYTES;
    size_t pos = 0;
    for (size_t i = 0; i < shown; ++i) {
        const uint8_t ch = s_data.payload[i];
        if (ch < 0x20 || ch == 0x7f || ch == '\\') {
            text[pos++] = '\\';
            text[pos++] = 'x';
            text[pos++] = hex[ch >> 4];
            text[pos++] = hex[ch & 15];
        } else {
            text[pos++] = (char)ch;
        }
    }
    text[pos] = 0;
    ESP_LOGI(TAG, "QR decoded: bytes=%u version=%d type=%d eci=%" PRIu32,
             (unsigned)length, s_data.version, s_data.data_type, s_data.eci);
    ESP_LOGI(TAG, "QR text: %s", text);
    if (shown < length) {
        ESP_LOGW(TAG, "Log truncated to %u/%u bytes; decoder received the full payload",
                 (unsigned)shown, (unsigned)length);
    }
}

static void scan_frame(const camera_fb_t *frame)
{
    uint8_t *gray = quirc_begin(s_decoder, NULL, NULL);
    if (frame == NULL || frame->format != PIXFORMAT_RGB565 ||
        frame->width != CAMERA_FRAME_WIDTH || frame->height != CAMERA_FRAME_HEIGHT ||
        !qr_rgb565_to_gray(frame->buf, frame->len, gray, QR_PREVIEW_PIXELS)) {
        ESP_LOGW(TAG, "Invalid RGB565 frame; QR scan skipped");
        return;
    }

    unsigned min = 255, max = 0, sum = 0;
    for (size_t i = 0; i < QR_PREVIEW_PIXELS; ++i) {
        const unsigned value = gray[i];
        if (value < min) min = value;
        if (value > max) max = value;
        sum += value;
    }
    const int64_t start_us = esp_timer_get_time();
    ++s_scans;
    quirc_end(s_decoder);
    const int count = quirc_count(s_decoder);
    s_candidates += (uint32_t)count;
    quirc_decode_error_t last_error = QUIRC_SUCCESS;
    for (int i = 0; i < count; ++i) {
        quirc_extract(s_decoder, i, &s_code);
        last_error = quirc_decode(&s_code, &s_data);
        if (last_error != QUIRC_SUCCESS) {
            quirc_flip(&s_code);
            last_error = quirc_decode(&s_code, &s_data);
        }
        if (last_error != QUIRC_SUCCESS) continue;
        ++s_decoded;
        const int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms >= s_next_payload_log_ms) {
            log_payload();
            s_next_payload_log_ms = now_ms + QR_LOG_INTERVAL_MS;
        }
        /* Present one QR code at a time. The next scan always runs again;
         * repeated successful scans are visible in the decoded counter. */
        break;
    }

    const int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms >= s_next_stats_ms) {
        ESP_LOGI(TAG, "scans=%" PRIu32 " candidates=%" PRIu32 " decoded=%" PRIu32
                 " gray=%u..%u mean=%u work_ms=%u stack_free=%u",
                 s_scans, s_candidates, s_decoded, min, max,
                 sum / QR_PREVIEW_PIXELS,
                 (unsigned)((esp_timer_get_time() - start_us) / 1000),
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
        if (last_error != QUIRC_SUCCESS) {
            ESP_LOGW(TAG, "QR candidate could not decode: %s", quirc_strerror(last_error));
        }
        s_next_stats_ms = now_ms + QR_LOG_INTERVAL_MS;
    }
}

static void qr_preview_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        scan_frame(s_input);
        /* No access to frame/decoder/image after signalling completion. */
        xSemaphoreGive(s_done);
    }
}

void qr_preview_process(const camera_fb_t *frame)
{
    if (s_worker == NULL || esp_timer_get_time() / 1000 < s_next_scan_ms) return;
    s_input = frame;
    xTaskNotifyGive(s_worker);
    /* Use a semaphore, not this task's notification: UVC already uses its
     * notification for USB transfer completion. Keep the frame until done. */
    xSemaphoreTake(s_done, portMAX_DELAY);
    s_input = NULL;
    s_next_scan_ms = esp_timer_get_time() / 1000 + QR_SCAN_INTERVAL_MS;
}

void qr_preview_deinit(void)
{
    /* Startup failure cleanup only, before any UVC frame requests exist. */
    if (s_worker != NULL) {
        vTaskDelete(s_worker);
        s_worker = NULL;
    }
    if (s_done != NULL) {
        vSemaphoreDelete(s_done);
        s_done = NULL;
    }
    if (s_decoder != NULL) {
        quirc_destroy(s_decoder);
        s_decoder = NULL;
    }
    s_input = NULL;
}

esp_err_t qr_preview_init(uint8_t **shared_buffer, size_t *capacity)
{
    if (shared_buffer == NULL || capacity == NULL) return ESP_ERR_INVALID_ARG;
    if (s_decoder != NULL || s_worker != NULL) return ESP_ERR_INVALID_STATE;
    *shared_buffer = NULL;
    *capacity = 0;
    s_decoder = quirc_new();
    if (s_decoder == NULL ||
        quirc_resize(s_decoder, CAMERA_FRAME_WIDTH, CAMERA_FRAME_HEIGHT) < 0) goto no_mem;
    s_done = xSemaphoreCreateBinary();
    if (s_done == NULL) goto no_mem;
    /* quirc_decode alone uses over 9 KiB of stack. The UVC callback task has
     * only 4 KiB, so delegate decoding while that task retains the same frame. */
    if (xTaskCreatePinnedToCore(qr_preview_task, "qr_preview", QR_WORKER_STACK_BYTES,
                              NULL, 4, &s_worker, 1) != pdPASS) goto no_mem;
    s_next_scan_ms = s_next_payload_log_ms = s_next_stats_ms = 0;
    s_scans = s_candidates = s_decoded = 0;
    *shared_buffer = quirc_begin(s_decoder, NULL, NULL);
    *capacity = QR_PREVIEW_PIXELS;
    ESP_LOGI(TAG, "QR test ready: open the PC Camera app to start capture and scanning");
    ESP_LOGI(TAG, "Shared gray/JPEG/USB buffer=%u; QR interval >=%d ms; logs on UART0 at 115200",
             (unsigned)*capacity, QR_SCAN_INTERVAL_MS);
    return ESP_OK;

no_mem:
    qr_preview_deinit();
    ESP_LOGE(TAG, "QR init failed: insufficient RAM; free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return ESP_ERR_NO_MEM;
}

#endif
