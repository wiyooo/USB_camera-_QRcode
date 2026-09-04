#include "esp32_s3_szp.h"

#if CAMERA_OUTPUT_USB_UVC

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "img_converters.h"
#include "usb_device_uvc.h"
#if CAMERA_UVC_QR_TEST
#include "esp_timer.h"
#include "qr_preview.h"
#endif

#define UVC_MAX_JPEG_SIZE (64 * 1024)
#define UVC_JPEG_QUALITY  80

static const char *TAG = "usb_camera";

typedef struct {
    uint8_t *buffer;
    size_t capacity;
    size_t length;
    bool overflow;
} jpeg_output_t;

static jpeg_output_t s_jpeg;
static uvc_fb_t s_frame;
static bool s_initialized;
#if CAMERA_UVC_QR_TEST
static bool s_qr_enabled;
static unsigned s_frames;
static int64_t s_next_video_log_ms;
#endif

/* frame2jpg() allocates 128 KiB per frame. Use a bounded, reusable buffer instead.
 * This esp32-camera version ignores callback short writes: latch overflow and
 * discard the entire JPEG afterwards, never transmit a truncated image. */
static size_t jpeg_write(void *ctx, size_t index, const void *data, size_t len)
{
    jpeg_output_t *output = ctx;
    if (output->overflow) {
        return 0;
    }
    if (index != output->length || index > output->capacity ||
        len > output->capacity - index || (len != 0 && data == NULL)) {
        output->overflow = true;
        return 0;
    }
    if (len != 0) {
        memcpy(output->buffer + index, data, len);
    }
    output->length += len;
    return len;
}

static esp_err_t usb_camera_start(uvc_format_t format, int width, int height,
                                  int rate, void *ctx)
{
    (void)ctx;
    if (format != UVC_FORMAT_JPEG || width != CAMERA_FRAME_WIDTH ||
        height != CAMERA_FRAME_HEIGHT || rate != CAMERA_FRAME_RATE) {
        ESP_LOGE(TAG, "Unsupported UVC mode: format=%d, %dx%d @ %d FPS",
                 format, width, height, rate);
        return ESP_ERR_NOT_SUPPORTED;
    }
    ESP_LOGI(TAG, "UVC stream requested: MJPEG %dx%d @ %d FPS", width, height, rate);
    return ESP_OK;
}

static void usb_camera_stop(void *ctx)
{
    (void)ctx;
    /* TinyUSB can call this while the UVC task encodes a frame.
     * The component controls frame requests; never reset/free buffers here. */
    ESP_LOGI(TAG, "UVC stream suspended/stopped");
}

static uvc_fb_t *usb_camera_fb_get(void *ctx)
{
    (void)ctx;
    /* Called serially by the UVC task after the previous USB transfer.
     * No separate producer task or queue is needed. */
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == NULL) {
        ESP_LOGW(TAG, "Camera frame timeout; check XCLK, PCLK, VSYNC and HREF");
        return NULL;
    }
    if (fb->format != PIXFORMAT_RGB565 || fb->width != CAMERA_FRAME_WIDTH ||
        fb->height != CAMERA_FRAME_HEIGHT ||
        fb->len != CAMERA_FRAME_WIDTH * CAMERA_FRAME_HEIGHT * 2) {
        ESP_LOGW(TAG, "Invalid camera frame: format=%d, %ux%u, %u bytes",
                 fb->format, (unsigned)fb->width, (unsigned)fb->height,
                 (unsigned)fb->len);
        esp_camera_fb_return(fb);
        return NULL;
    }

#if CAMERA_UVC_QR_TEST
    /* Previous USB transfer is complete. Decode this exact RGB565 frame,
     * then overwrite the decoder's gray image with JPEG. The blocking call
     * retains ownership of both buffers until its worker is finished. */
    if (s_qr_enabled) qr_preview_process(fb);
#endif

    s_jpeg.length = 0;
    s_jpeg.overflow = false;
    const struct timeval timestamp = fb->timestamp;
    bool encoded = frame2jpg_cb(fb, UVC_JPEG_QUALITY, jpeg_write, &s_jpeg);
    esp_camera_fb_return(fb);

    if (!encoded || s_jpeg.overflow || s_jpeg.length < 4 ||
        s_jpeg.buffer[0] != 0xff || s_jpeg.buffer[1] != 0xd8 ||
        s_jpeg.buffer[s_jpeg.length - 2] != 0xff ||
        s_jpeg.buffer[s_jpeg.length - 1] != 0xd9) {
        ESP_LOGW(TAG, "JPEG encoding failed or exceeded %u bytes; frame dropped",
                 (unsigned)s_jpeg.capacity);
        return NULL;
    }

    s_frame = (uvc_fb_t) {
        .buf = s_jpeg.buffer,
        .len = s_jpeg.length,
        .width = CAMERA_FRAME_WIDTH,
        .height = CAMERA_FRAME_HEIGHT,
        .format = UVC_FORMAT_JPEG,
        .timestamp = timestamp,
    };
#if CAMERA_UVC_QR_TEST
    ++s_frames;
    const int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms >= s_next_video_log_ms) {
        ESP_LOGI(TAG, "UVC frames=%u jpeg=%u bytes qr=%s free=%u",
                 s_frames, (unsigned)s_jpeg.length, s_qr_enabled ? "on" : "OFF",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        s_next_video_log_ms = now_ms + 2000;
    }
#endif
    return &s_frame;
}

static void usb_camera_fb_return(uvc_fb_t *fb, void *ctx)
{
    (void)fb;
    (void)ctx;
    /* Zero-copy mode keeps this buffer owned by the UVC task until the USB
     * transfer-complete notification, then reuses it for the next frame. */
}

esp_err_t app_camera_usb_init(void)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

#if CAMERA_UVC_QR_TEST
    s_qr_enabled = qr_preview_init(&s_jpeg.buffer, &s_jpeg.capacity) == ESP_OK;
    if (!s_qr_enabled) {
        ESP_LOGW(TAG, "QR disabled: falling back to UVC preview only; inspect QR init error above");
    }
#endif
    if (s_jpeg.buffer == NULL) {
        s_jpeg.capacity = UVC_MAX_JPEG_SIZE;
        s_jpeg.buffer = heap_caps_malloc(s_jpeg.capacity,
                                         MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    }
    if (s_jpeg.buffer == NULL) {
        ESP_LOGE(TAG, "Not enough internal RAM for the shared JPEG/USB buffer");
        free(s_jpeg.buffer);
        s_jpeg.buffer = NULL;
        return ESP_ERR_NO_MEM;
    }

    jpgSetRgb565BE(true);
    uvc_device_config_t config = {
        /* The UVC task waits for USB completion before requesting another
         * frame, so one buffer can safely serve both encoder and USB. */
        .uvc_buffer = s_jpeg.buffer,
        .uvc_buffer_size = s_jpeg.capacity,
        .start_cb = usb_camera_start,
        .fb_get_cb = usb_camera_fb_get,
        .fb_return_cb = usb_camera_fb_return,
        .stop_cb = usb_camera_stop,
        .cb_ctx = NULL,
    };

    esp_err_t err = uvc_device_config(0, &config);
    if (err == ESP_OK) {
        err = uvc_device_init();
    }
    if (err != ESP_OK) {
#if CAMERA_UVC_QR_TEST
        if (s_qr_enabled) {
            qr_preview_deinit();
            s_qr_enabled = false;
        } else
#endif
        {
            free(s_jpeg.buffer);
        }
        s_jpeg.buffer = NULL;
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "GC2145 USB camera ready: GPIO19 D-, GPIO20 D+, no PSRAM required");
    ESP_LOGI(TAG, "Free internal RAM: %u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return ESP_OK;
}

#endif /* CAMERA_OUTPUT_USB_UVC */
