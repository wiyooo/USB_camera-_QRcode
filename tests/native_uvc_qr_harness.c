/* Hardware and scheduling are simulated. QR worker, UVC callback, RGB565
 * conversion, quirc and JPEG encoder are the actual production sources. */
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef NDEBUG
#error "Assertions must be enabled"
#endif
#define TAG qr_tag
#include "qr_preview.c"
#undef TAG
#include "usb_camera.c"

static camera_fb_t input;
static bool held, timeout, pending, done, fail_sem, fail_task;
static int tasks, semaphores, captures, returns;
static int64_t time_ms;
static jmp_buf worker_yield;
static void (*worker_entry)(void *);
static esp_err_t init_result;
static uvc_device_config_t configured;

int64_t esp_timer_get_time(void) { return time_ms * 1000; }
void *heap_caps_malloc(size_t n, unsigned caps) { (void)caps; return malloc(n); }
size_t heap_caps_get_free_size(unsigned caps) { (void)caps; return 100000; }
size_t heap_caps_get_largest_free_block(unsigned caps) { (void)caps; return 80000; }
TaskHandle_t worker_handle = (void *)1;
SemaphoreHandle_t semaphore_handle = (void *)2;

BaseType_t xTaskCreatePinnedToCore(void (*entry)(void *), const char *name, uint32_t stack,
    void *arg, UBaseType_t priority, TaskHandle_t *handle, BaseType_t core)
{
    (void)name; (void)arg;
    assert(stack >= 16*1024 && priority == 4 && core == 1);
    if (fail_task) return 0;
    assert(tasks == 0);
    ++tasks; worker_entry = entry; *handle = worker_handle;
    return pdPASS;
}
void vTaskDelete(TaskHandle_t handle) { assert(handle == worker_handle && tasks == 1); --tasks; }
void xTaskNotifyGive(TaskHandle_t handle) { assert(handle == worker_handle && !pending); pending = true; }
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait)
{
    assert(clear == pdTRUE && wait == portMAX_DELAY);
    if (!pending) longjmp(worker_yield, 1);
    pending = false;
    return 1;
}
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t handle) { (void)handle; return 4096; }
SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    if (fail_sem) return NULL;
    assert(semaphores == 0); ++semaphores; done = false;
    return semaphore_handle;
}
void vSemaphoreDelete(SemaphoreHandle_t handle)
{ assert(handle == semaphore_handle && semaphores == 1); --semaphores; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t handle)
{ assert(handle == semaphore_handle && held && !done); done = true; return pdTRUE; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t handle, TickType_t wait)
{
    assert(handle == semaphore_handle && wait == portMAX_DELAY && held && pending && !done);
    if (setjmp(worker_yield) == 0) worker_entry(NULL);
    assert(done && !pending && held);
    done = false;
    return pdTRUE;
}
camera_fb_t *esp_camera_fb_get(void)
{
    assert(!held);
    if (timeout) return NULL;
    held = true; ++captures; return &input;
}
void esp_camera_fb_return(camera_fb_t *fb)
{ assert(fb == &input && held && !pending && !done); held = false; ++returns; }
esp_err_t uvc_device_config(int index, uvc_device_config_t *cfg)
{ assert(index == 0); configured = *cfg; return ESP_OK; }
esp_err_t uvc_device_init(void) { return init_result; }

int main(void)
{
    uint8_t *scratch = NULL;
    size_t capacity = 0;
    assert(qr_preview_init(NULL, &capacity) == ESP_ERR_INVALID_ARG);
    fail_sem = true;
    assert(qr_preview_init(&scratch, &capacity) == ESP_ERR_NO_MEM);
    assert(!s_decoder && !tasks && !semaphores && !scratch && capacity == 0);
    fail_sem = false; fail_task = true;
    assert(qr_preview_init(&scratch, &capacity) == ESP_ERR_NO_MEM);
    assert(!s_decoder && !tasks && !semaphores);
    fail_task = false;

    /* Failed USB startup must release both decoder buffer and worker. */
    init_result = ESP_ERR_INVALID_STATE;
    assert(app_camera_usb_init() == ESP_ERR_INVALID_STATE);
    assert(!s_jpeg.buffer && !s_decoder && !tasks && !semaphores);
    /* If QR cannot initialize, preview still gets its own bounded buffer. */
    fail_task = true; init_result = ESP_OK;
    assert(app_camera_usb_init() == ESP_OK);
    assert(!s_qr_enabled && configured.uvc_buffer_size == UVC_MAX_JPEG_SIZE);
    free(s_jpeg.buffer); s_jpeg.buffer = NULL; s_initialized = false;
    fail_task = false;

    assert(app_camera_usb_init() == ESP_OK);
    assert(app_camera_usb_init() == ESP_ERR_INVALID_STATE);
    assert(s_qr_enabled && tasks == 1 && semaphores == 1);
    assert(configured.uvc_buffer_size == 320*240);
    assert(configured.uvc_buffer == quirc_begin(s_decoder, NULL, NULL));
    assert(usb_camera_start(UVC_FORMAT_JPEG, 320, 240, 20, NULL) == ESP_OK);
    timeout = true;
    assert(!usb_camera_fb_get(NULL)); timeout = false;

    input = (camera_fb_t){.width=320, .height=240, .format=PIXFORMAT_RGB565,
        .len=320*240*2, .timestamp={123,456}};
    input.buf = malloc(input.len); assert(input.buf);
    --input.len; assert(!usb_camera_fb_get(NULL) && !held); ++input.len;

    const char *names[] = {"ascii", "chinese", "binary"};
    const unsigned char *payloads[] = {(const unsigned char *)"USB-QR-123456",
        (const unsigned char *)"\xe6\x89\xab\xe7\xa0\x81\xe6\x88\x90\xe5\x8a\x9f",
        (const unsigned char *)"A\0B\nC\\D"};
    const size_t lengths[] = {13, 12, 7};
    for (size_t k=0; k<3; ++k) {
        char path[40]; snprintf(path, sizeof(path), "%s.rgb565", names[k]);
        FILE *f = fopen(path, "rb"); assert(f);
        assert(fread(input.buf, 1, input.len, f) == input.len); fclose(f);
        /* Repeated scans must succeed even after JPEG overwrites the same
         * quirc image. Sensor bytes and JPEG bytes must remain intact. */
        for (int repeat=0; repeat<4; ++repeat) {
            time_ms += 2100;
            uint32_t before = s_decoded;
            uvc_fb_t *fb = usb_camera_fb_get(NULL);
            assert(fb && !held && captures == returns && s_decoded == before+1);
            assert((size_t)s_data.payload_len == lengths[k]);
            assert(!memcmp(s_data.payload, payloads[k], lengths[k]));
            assert(fb->buf == configured.uvc_buffer && fb->len <= configured.uvc_buffer_size);
            assert(fb->buf[0] == 0xff && fb->buf[1] == 0xd8);
            assert(fb->timestamp.tv_sec == 123 && fb->timestamp.tv_usec == 456);
            snprintf(path, sizeof(path), "%s.jpg", names[k]);
            f = fopen(path, "wb"); assert(f);
            assert(fwrite(fb->buf, 1, fb->len, f) == fb->len); fclose(f);
            usb_camera_fb_return(fb, NULL);
            /* Callback return/stop must not invalidate in-flight JPEG. */
            usb_camera_stop(NULL);
            assert(fb->buf[0] == 0xff && fb->buf[fb->len-1] == 0xd9);
            assert(usb_camera_start(UVC_FORMAT_JPEG, 320, 240, 20, NULL) == ESP_OK);
            before = s_scans;
            assert(usb_camera_fb_get(NULL) && s_scans == before); /* throttled */
        }
    }
    /* Blank frame still makes valid video; QR counters stop increasing. */
    memset(input.buf, 0xff, input.len);
    time_ms += 3000;
    uint32_t before = s_decoded;
    assert(usb_camera_fb_get(NULL) && s_decoded == before);
    assert(!held && captures == returns);
    qr_preview_deinit();
    assert(!s_decoder && !tasks && !semaphores);
    free(input.buf);
    puts("PASS: QR worker handoff, shared memory reuse, UART logs, blank frame, fallback and startup cleanup");
    return 0;
}
