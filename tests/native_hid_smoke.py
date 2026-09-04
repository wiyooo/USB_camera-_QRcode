"""Run the actual HID transport/descriptors with a simulated USB host and RTOS."""
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / ".native-qr-test/hid"
INC = OUT / "include"
INC.mkdir(parents=True, exist_ok=True)

headers = {
    "esp_err.h": """#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_STATE 0x103
""",
    "esp_log.h": "#pragma once\n#define ESP_LOGI(...) ((void)0)\n#define ESP_LOGW(...) ((void)0)\n",
    "esp_timer.h": "#pragma once\n#include <stdint.h>\nint64_t esp_timer_get_time(void);\n",
    "esp_mac.h": "#pragma once\n#include <stdint.h>\n#include \"esp_err.h\"\nesp_err_t esp_efuse_mac_get_default(uint8_t *mac);\n",
    "freertos/FreeRTOS.h": """#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef unsigned TickType_t;
typedef void *TaskHandle_t;
typedef void *QueueHandle_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define eSetValueWithOverwrite 0
""",
    "freertos/task.h": """#pragma once
#include "FreeRTOS.h"
TaskHandle_t xTaskGetCurrentTaskHandle(void);
BaseType_t xTaskNotifyWait(uint32_t, uint32_t, uint32_t *, TickType_t);
BaseType_t xTaskNotify(TaskHandle_t, uint32_t, int);
BaseType_t xTaskCreatePinnedToCore(void (*)(void *), const char *, unsigned, void *, unsigned, TaskHandle_t *, int);
void vTaskDelay(TickType_t);
""",
    "freertos/queue.h": """#pragma once
#include "FreeRTOS.h"
QueueHandle_t xQueueCreate(unsigned, size_t);
BaseType_t xQueueSend(QueueHandle_t, const void *, TickType_t);
BaseType_t xQueueReceive(QueueHandle_t, void *, TickType_t);
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t);
void vQueueDelete(QueueHandle_t);
""",
    "esp_private/usb_phy.h": """#pragma once
#include "esp_err.h"
typedef void *usb_phy_handle_t;
typedef struct { int controller, target, otg_mode, otg_speed; } usb_phy_config_t;
#define USB_PHY_CTRL_OTG 1
#define USB_PHY_TARGET_INT 1
#define USB_OTG_MODE_DEVICE 1
#define USB_PHY_SPEED_FULL 1
esp_err_t usb_new_phy(const usb_phy_config_t *, usb_phy_handle_t *);
esp_err_t usb_del_phy(usb_phy_handle_t);
""",
}
for name, content in headers.items():
    path = INC / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")

exe = OUT / "hid_runtime.exe"
sources = [ROOT / "tests/native_hid_harness.c", ROOT / "main/qr_keyboard.c"]
includes = [INC, ROOT / "main", ROOT / "main/usb_hid", ROOT / "managed_components/espressif__tinyusb/src"]
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O2", "-UNDEBUG",
                "-DCFG_TUSB_MCU=OPT_MCU_NONE", "-DTUP_DCD_ENDPOINT_MAX=8",
                *[arg for path in includes for arg in ("-I", str(path))],
                *map(str, sources), "-o", str(exe)], check=True)
subprocess.run([str(exe)], check=True)
