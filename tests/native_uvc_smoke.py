"""Host smoke test: actual usb_camera.c + actual esp32-camera JPEG encoder.

Requires Python, Pillow and Zig (or GCC/G++). Hardware I/O is mocked;
this checks JPEG correctness/ownership/bounds, not USB enumeration or sensor timing.
"""
import argparse
from pathlib import Path
import subprocess
import sys

import qrcode

from PIL import Image

parser = argparse.ArgumentParser()
parser.add_argument("--cc")
parser.add_argument("--cxx")
parser.add_argument("--qr-preview", action="store_true", help="Also execute real QR/UVC integration")
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
out = root / ".native-qr-test" / ("uvc_qr" if args.qr_preview else "uvc")
inc = out / "include"
inc.mkdir(parents=True, exist_ok=True)

headers = {
    "esp_err.h": """#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_SUPPORTED 0x106
""",
    "esp_camera.h": """#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/time.h>
typedef enum { PIXFORMAT_RGB565, PIXFORMAT_YUV422, PIXFORMAT_GRAYSCALE,
               PIXFORMAT_RGB888, PIXFORMAT_JPEG } pixformat_t;
typedef struct {
    uint8_t *buf;
    size_t len, width, height;
    pixformat_t format;
    struct timeval timestamp;
} camera_fb_t;
camera_fb_t *esp_camera_fb_get(void);
void esp_camera_fb_return(camera_fb_t *fb);
""",
    "esp_heap_caps.h": """#pragma once
#include <stddef.h>
#include <stdlib.h>
#define MALLOC_CAP_8BIT 1
#define MALLOC_CAP_INTERNAL 2
#ifdef __cplusplus
extern "C" {
#endif
void *heap_caps_malloc(size_t size, unsigned caps);
size_t heap_caps_get_free_size(unsigned caps);
size_t heap_caps_get_largest_free_block(unsigned caps);
#ifdef __cplusplus
}
#endif
""",
    "esp_log.h": """#pragma once
#include <stdio.h>
#define ESP_LOGI(tag, fmt, ...) printf("I (%s) " fmt "\\n", tag, ##__VA_ARGS__)
#define ESP_LOGW ESP_LOGI
#define ESP_LOGE ESP_LOGI
""",
    "esp_attr.h": "#pragma once\n#define IRAM_ATTR\n",
    "soc/efuse_reg.h": "#pragma once\n",
    "img_converters.h": """#pragma once
#include "esp_camera.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef size_t (*jpg_out_cb)(void *, size_t, const void *, size_t);
typedef enum { CHROMA_444=1, CHROMA_422=2, CHROMA_420=3 } chroma_t;
bool frame2jpg_cb(camera_fb_t *, uint8_t, jpg_out_cb, void *);
void jpgSetRgb565BE(bool);
void jpgSetChroma(chroma_t);
#ifdef __cplusplus
}
#endif
""",
}
if args.qr_preview:
    headers.update({
        "esp_timer.h": "#pragma once\n#include <stdint.h>\nint64_t esp_timer_get_time(void);\n",
        "freertos/FreeRTOS.h": """#pragma once
#include <stdint.h>
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
typedef void *TaskHandle_t;
typedef void *SemaphoreHandle_t;
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
""",
        "freertos/task.h": """#pragma once
#include "FreeRTOS.h"
BaseType_t xTaskCreatePinnedToCore(void (*)(void *), const char *, uint32_t, void *,
                                 UBaseType_t, TaskHandle_t *, BaseType_t);
void vTaskDelete(TaskHandle_t);
void xTaskNotifyGive(TaskHandle_t);
uint32_t ulTaskNotifyTake(BaseType_t, TickType_t);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t);
""",
        "freertos/semphr.h": """#pragma once
#include "FreeRTOS.h"
SemaphoreHandle_t xSemaphoreCreateBinary(void);
void vSemaphoreDelete(SemaphoreHandle_t);
BaseType_t xSemaphoreGive(SemaphoreHandle_t);
BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t);
""",
    })
for name, content in headers.items():
    path = inc / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")

harness = r'''
#include <assert.h>
#include <stdio.h>
#ifdef NDEBUG
#error "Assertions must be enabled"
#endif
#include "usb_camera.c"

static camera_fb_t input;
static int captures, returns, allocation_count, fail_at = -1;
static bool timeout;
static esp_err_t init_result;
static uvc_device_config_t configured;
void *heap_caps_malloc(size_t n, unsigned caps) {
    (void)caps;
    if (allocation_count++ == fail_at) return NULL;
    return malloc(n);
}
size_t heap_caps_get_free_size(unsigned caps) { (void)caps; return 100000; }
camera_fb_t *esp_camera_fb_get(void) {
    if (timeout) return NULL;
    ++captures;
    return &input;
}
void esp_camera_fb_return(camera_fb_t *fb) { assert(fb == &input); ++returns; }
esp_err_t uvc_device_config(int index, uvc_device_config_t *cfg) {
    assert(index == 0);
    configured = *cfg;
    return ESP_OK;
}
esp_err_t uvc_device_init(void) { return init_result; }

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    /* Allocation failure and USB init failure must leave a retry safe. */
    for (int i = 0; i < 1; ++i) {
        allocation_count = 0; fail_at = i;
        assert(app_camera_usb_init() == ESP_ERR_NO_MEM);
        assert(!s_jpeg.buffer);
    }
    fail_at = -1; init_result = ESP_ERR_INVALID_STATE;
    assert(app_camera_usb_init() == ESP_ERR_INVALID_STATE);
    assert(!s_jpeg.buffer);
    init_result = ESP_OK;
    assert(app_camera_usb_init() == ESP_OK);
    assert(app_camera_usb_init() == ESP_ERR_INVALID_STATE);
    assert(configured.uvc_buffer == s_jpeg.buffer);
    assert(usb_camera_start(UVC_FORMAT_JPEG, 320, 240, 20, NULL) == ESP_OK);
    assert(usb_camera_start(UVC_FORMAT_H264, 320, 240, 20, NULL) != ESP_OK);
    assert(usb_camera_start(UVC_FORMAT_JPEG, 640, 480, 20, NULL) != ESP_OK);
    assert(usb_camera_start(UVC_FORMAT_JPEG, 320, 240, 30, NULL) != ESP_OK);

    /* Guard bytes catch overflow; index mismatch and SIZE_MAX cannot wrap. */
    uint8_t guarded[UVC_MAX_JPEG_SIZE + 2], source[UVC_MAX_JPEG_SIZE];
    memset(guarded, 0xa5, sizeof(guarded)); memset(source, 0x55, sizeof(source));
    jpeg_output_t sink = {.buffer = guarded + 1, .capacity = UVC_MAX_JPEG_SIZE};
    assert(jpeg_write(&sink, 0, source, sizeof(source)) == sizeof(source));
    assert(jpeg_write(&sink, sizeof(source), source, 1) == 0 && sink.overflow);
    assert(guarded[0] == 0xa5 && guarded[sizeof(guarded)-1] == 0xa5);
    sink.length = 0; sink.overflow = false;
    assert(jpeg_write(&sink, 0, source, SIZE_MAX) == 0 && sink.overflow);
    sink.length = 0; sink.overflow = false;
    assert(jpeg_write(&sink, 1, source, 1) == 0 && sink.overflow);

    timeout = true;
    assert(usb_camera_fb_get(NULL) == NULL && returns == 0);
    timeout = false;
    input = (camera_fb_t){.width=320, .height=240, .format=PIXFORMAT_RGB565,
                         .len=320*240*2, .timestamp={123,456}};
    input.buf = malloc(input.len);
    assert(input.buf);
    const uint16_t bars[] = {0xf800, 0x07e0, 0x001f, 0xffff};
    for (size_t y=0; y<240; ++y) for (size_t x=0; x<320; ++x) {
        uint16_t pixel = bars[x/80];
        input.buf[(y*320+x)*2] = pixel >> 8;
        input.buf[(y*320+x)*2+1] = pixel;
    }
    --input.len;
    assert(usb_camera_fb_get(NULL) == NULL && captures == returns);
    ++input.len;
    input.format = PIXFORMAT_JPEG;
    assert(usb_camera_fb_get(NULL) == NULL && captures == returns);
    input.format = PIXFORMAT_RGB565;

    uint8_t *original_jpeg = s_jpeg.buffer;
    for (int i=0; i<12; ++i) {
        uvc_fb_t *fb = usb_camera_fb_get(NULL);
        assert(fb && fb->width == 320 && fb->height == 240);
        assert(fb->timestamp.tv_sec == 123 && fb->timestamp.tv_usec == 456);
        assert(captures == returns && fb->buf == original_jpeg);
        assert(fb->len > 4 && fb->len <= configured.uvc_buffer_size);
        if (configured.uvc_buffer != fb->buf) {
            memcpy(configured.uvc_buffer, fb->buf, fb->len);
        }
        if (i == 0) {
            FILE *file = fopen("colorbars.jpg", "wb"); assert(file);
            assert(fwrite(fb->buf, 1, fb->len, file) == fb->len); fclose(file);
        }
        usb_camera_fb_return(fb, NULL);
        usb_camera_stop(NULL);
        assert(usb_camera_start(UVC_FORMAT_JPEG, 320, 240, 20, NULL) == ESP_OK);
    }
    /* High-entropy input goes through the real JPEG encoder too. */
    uint32_t rng=1;
    for (size_t i=0; i<input.len; ++i) {
        rng = rng*1664525u + 1013904223u; input.buf[i] = rng >> 24;
    }
    uvc_fb_t *noisy = usb_camera_fb_get(NULL);
    printf("Noise encode: overflow=%d, bytes=%u\n", s_jpeg.overflow, (unsigned)s_jpeg.length);
    assert(noisy && captures == returns);
    FILE *file = fopen("noise.jpg", "wb"); assert(file);
    assert(fwrite(noisy->buf, 1, noisy->len, file) == noisy->len); fclose(file);
    printf("PASS: real JPEG encode, bounds, capture ownership, repeated callbacks, init failure cleanup\n");
    printf("Noise JPEG: %u / %u bytes\n", (unsigned)noisy->len, UVC_MAX_JPEG_SIZE);
    free(input.buf); free(s_jpeg.buffer);
    return 0;
}
'''
if args.qr_preview:
    harness = (root / "tests/native_uvc_qr_harness.c").read_text(encoding="utf-8")
(out / "harness.c").write_text(harness, encoding="utf-8")
camera = root / "managed_components/espressif__esp32-camera/conversions"
include_paths = [inc, root / "main", root / "build/config",
                 root / "managed_components/espressif__usb_device_uvc/include",
                 camera / "private_include", root / "components/quirc/lib"]
flags = [flag for path in include_paths for flag in ["-I", str(path)]]
flags += ["-DCAMERA_UVC_QR_TEST=" + str(int(args.qr_preview)), "-DCAMERA_OUTPUT_QR_USB=0",
          "-DCAMERA_OUTPUT_USB_UVC=1", "-DQUIRC_FLOAT_TYPE=float", "-DQUIRC_USE_TGMATH"]
objects = []
sources = [out / "harness.c", camera / "to_jpg.cpp", camera / "jpge.cpp", camera / "yuv.c"]
if args.qr_preview:
    sources += [root / "main/qr_payload.c"]
    sources += [root / "components/quirc/lib" / name for name in
                ("quirc.c", "identify.c", "decode.c", "version_db.c")]
# The camera encoder's output_stream ABI assumes 32-bit size_t == unsigned int.
# Keep that production ABI in the Windows host test instead of patching it.
target = ["-target", "x86-windows-gnu"] if sys.platform == "win32" else ["-m32"]
cc = [args.cc, "-m32"] if args.cc else [sys.executable, "-m", "ziglang", "cc", *target]
cxx = [args.cxx, "-m32"] if args.cxx else [sys.executable, "-m", "ziglang", "c++", *target]
for source in sources:
    obj = out / (source.stem + ".o")
    compiler = cxx if source.suffix == ".cpp" else cc
    subprocess.run([*compiler, "-O2", "-UNDEBUG", *flags, "-c", str(source), "-o", str(obj)], check=True)
    objects.append(str(obj))
exe = out / "native_uvc_smoke.exe"
subprocess.run([*cxx, "-static", *objects, "-o", str(exe)], check=True)
if args.qr_preview:
    for name, payload in [("ascii", b"USB-QR-123456"), ("chinese", "扫码成功".encode()),
                          ("binary", b"A\x00B\nC\\D")]:
        qr = qrcode.QRCode(box_size=5, border=4)
        qr.add_data(payload, optimize=0)
        qr.make(fit=True)
        tile = qr.make_image().convert("L")
        canvas = Image.new("L", (320, 240), 255)
        canvas.paste(tile, ((320-tile.width)//2, (240-tile.height)//2))
        raw = bytearray()
        for value in canvas.tobytes():
            packed = ((value >> 3) << 11) | ((value >> 2) << 5) | (value >> 3)
            raw.extend((packed >> 8, packed & 255))
        (out / f"{name}.rgb565").write_bytes(raw)
    result = subprocess.run([str(exe)], cwd=out, check=True, capture_output=True)
    log = result.stdout.decode("utf-8")
    print(log)
    assert "QR text: USB-QR-123456" in log
    assert "QR text: 扫码成功" in log
    assert r"QR text: A\x00B\x0AC\x5CD" in log
    for name in ("ascii", "chinese", "binary"):
        with Image.open(out / f"{name}.jpg") as picture:
            picture.load()
            assert picture.size == (320, 240)
    print("PASS: actual QR decode -> ESP_LOGI payload -> shared JPEG/USB buffer; Pillow read every frame")
    sys.exit(0)
subprocess.run([str(exe)], cwd=out, check=True)
image = Image.open(out / "colorbars.jpg").convert("RGB")
assert image.size == (320, 240)
expected = [(248, 0, 0), (0, 252, 0), (0, 0, 248), (248, 252, 248)]
for x, color in zip([40, 120, 200, 280], expected):
    actual = image.getpixel((x, 120))
    assert all(abs(a-b) <= 10 for a, b in zip(actual, color)), (actual, color)
with Image.open(out / "noise.jpg") as noisy:
    noisy.load()
    assert noisy.size == (320, 240)
print("PASS: Pillow decoded both JPEGs; RGB565 byte order and RGB colors verified")
