#include "esp32_s3_szp.h"

#if CAMERA_OUTPUT_QR_USB

#include <inttypes.h>
#include <stdlib.h>

#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "qr_payload.h"
#include "qr_hid.h"
#include "quirc.h"

#define QR_TASK_STACK_BYTES (16 * 1024)
#define QR_SCAN_PAUSE_MS 80

static const char *TAG = "qr_scanner";
static struct quirc *s_decoder;
/* Keep these large structures off the task stack. Only the scanner task owns them. */
static struct quirc_code s_code;
static struct quirc_data s_data;
static qr_repeat_filter_t s_repeat;
static uint32_t s_sequence;
static bool s_initialized;

static void qr_scanner_task(void *arg)
{
    (void)arg;
    /* The task is created before USB init so allocation failures can clean up. */
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    unsigned frames = 0;
    unsigned decoded = 0;
    unsigned typed = 0;
    unsigned unsupported = 0;
    unsigned aborted = 0;
    unsigned too_long = 0;
    unsigned usb_busy = 0;
    int64_t stats_ms = esp_timer_get_time() / 1000;

    for (;;) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb == NULL) {
            ESP_LOGW(TAG, "Camera frame timeout; check camera signals");
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        uint8_t *gray = quirc_begin(s_decoder, NULL, NULL);
        bool valid = fb->format == PIXFORMAT_RGB565 &&
                     fb->width == CAMERA_FRAME_WIDTH && fb->height == CAMERA_FRAME_HEIGHT &&
                     qr_rgb565_to_gray(fb->buf, fb->len, gray,
                                       CAMERA_FRAME_WIDTH * CAMERA_FRAME_HEIGHT);
        /* Return the only camera buffer before the CPU-intensive decoder runs. */
        esp_camera_fb_return(fb);
        if (!valid) {
            ESP_LOGW(TAG, "Invalid RGB565 frame; QR frame dropped");
            vTaskDelay(pdMS_TO_TICKS(QR_SCAN_PAUSE_MS));
            continue;
        }
        ++frames;
        quirc_end(s_decoder);

        /* Single-target scanner: report the first decodable code in each frame.
         * Present one code at a time so selection cannot alternate between codes. */
        for (int i = 0; i < quirc_count(s_decoder); ++i) {
            quirc_extract(s_decoder, i, &s_code);
            quirc_decode_error_t result = quirc_decode(&s_code, &s_data);
            if (result != QUIRC_SUCCESS) {
                quirc_flip(&s_code);
                result = quirc_decode(&s_code, &s_data);
            }
            if (result != QUIRC_SUCCESS) {
                continue;
            }
            ++decoded;
            if (s_data.payload_len < 0 || s_data.payload_len > QR_USB_MAX_PAYLOAD) {
                ++too_long;
                break;
            }
            const size_t length = (size_t)s_data.payload_len;
            const int64_t now_ms = esp_timer_get_time() / 1000;
            if (!qr_repeat_should_send(&s_repeat, s_data.payload, length,
                                       s_data.eci, s_data.data_type, now_ms)) {
                break;
            }
            if (!qr_hid_ready()) {
                break;
            }
            qr_hid_result_t output = qr_hid_type(s_data.payload, length);
            if (output == QR_HID_TYPED || output == QR_HID_ABORTED) {
                /* Long strings take time to type. Start the repeat timer at
                 * completion; an interrupted prefix must not be replayed. */
                qr_repeat_mark_sent(&s_repeat, s_data.payload, length,
                                     s_data.eci, s_data.data_type, esp_timer_get_time() / 1000);
            }
            if (output == QR_HID_TYPED) {
                ++s_sequence;
                ++typed;
                ESP_LOGI(TAG, "QR #%" PRIu32 " typed by HID: %u bytes", s_sequence, (unsigned)length);
            } else if (output == QR_HID_UNSUPPORTED) {
                ++unsupported;
            } else if (output == QR_HID_ABORTED) {
                ++aborted;
            } else {
                ++usb_busy;
            }
            break;
        }

        const int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms - stats_ms >= 10000) {
            ESP_LOGI(TAG, "frames=%u decoded=%u typed=%u oversized=%u unsupported=%u aborted=%u usb_busy=%u free=%u stack_free=%u",
                     frames, decoded, typed, too_long, unsupported, aborted, usb_busy,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
            stats_ms = now_ms;
        }
        /* Yield every frame so the idle task and USB interrupts keep running. */
        vTaskDelay(pdMS_TO_TICKS(QR_SCAN_PAUSE_MS));
    }
}

esp_err_t app_camera_qr_init(void)
{
    if (s_initialized || s_decoder != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_decoder = quirc_new();
    if (s_decoder == NULL || quirc_resize(s_decoder, CAMERA_FRAME_WIDTH, CAMERA_FRAME_HEIGHT) < 0) {
        if (s_decoder != NULL) {
            quirc_destroy(s_decoder);
            s_decoder = NULL;
        }
        ESP_LOGE(TAG, "No RAM for QR decoder: free=%u largest=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        return ESP_ERR_NO_MEM;
    }
    TaskHandle_t scanner = NULL;
    if (xTaskCreatePinnedToCore(qr_scanner_task, "qr_scanner", QR_TASK_STACK_BYTES,
                                NULL, 4, &scanner, 1) != pdPASS) {
        quirc_destroy(s_decoder);
        s_decoder = NULL;
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = qr_hid_init();
    if (err != ESP_OK) {
        vTaskDelete(scanner); /* Still waiting for its startup notification. */
        quirc_destroy(s_decoder);
        s_decoder = NULL;
        return err;
    }
    s_initialized = true;
    xTaskNotifyGive(scanner);
    ESP_LOGI(TAG, "QR scanner ready: %dx%d RGB565 -> gray -> quirc -> USB HID keyboard",
             CAMERA_FRAME_WIDTH, CAMERA_FRAME_HEIGHT);
    ESP_LOGI(TAG, "USB GPIO19 D-, GPIO20 D+; UART0 logs; max payload %u bytes",
             QR_USB_MAX_PAYLOAD);
    ESP_LOGI(TAG, "Free internal RAM: %u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return ESP_OK;
}

#endif
