#include "qr_hid.h"

#include <stdatomic.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_private/usb_phy.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "qr_keyboard.h"
#include "qr_payload.h"
#include "tusb.h"
#include "usb_descriptors.h"

#define HID_REPORT_TIMEOUT_MS 1500
#define HID_TASK_STACK_BYTES 4096
#define HID_APPEND_ENTER true

typedef struct {
    TaskHandle_t caller;
    unsigned epoch;
    size_t length;
    uint8_t payload[QR_USB_MAX_PAYLOAD];
} hid_request_t;

static const char *TAG = "qr_hid";
static QueueHandle_t s_requests;
static atomic_bool s_accepting;
static atomic_uint s_epoch;
/* Everything below is owned by the USB task, including all TinyUSB callbacks. */
static bool s_mounted;
static bool s_suspended;
static bool s_recovery = true;
static bool s_inflight;
static bool s_recovery_report;
static bool s_completed;
static bool s_failed;
static uint8_t s_leds;
static qr_keyboard_report_t s_last_report;
static hid_request_t s_request;

bool qr_hid_ready(void)
{
    return atomic_load(&s_accepting);
}

qr_hid_result_t qr_hid_type(const uint8_t *payload, size_t length)
{
    if (!qr_keyboard_validate(payload, length)) {
        return QR_HID_UNSUPPORTED;
    }
    bool expected = true;
    if (!atomic_compare_exchange_strong(&s_accepting, &expected, false)) {
        return QR_HID_NOT_READY;
    }
    hid_request_t request = {
        .caller = xTaskGetCurrentTaskHandle(),
        .epoch = atomic_load(&s_epoch),
        .length = length,
    };
    memcpy(request.payload, payload, length);
    uint32_t result;
    /* The scanner owns its notification slot. Drop any obsolete startup signal. */
    xTaskNotifyWait(0, UINT32_MAX, &result, 0);
    if (xQueueSend(s_requests, &request, 0) != pdTRUE) {
        if (request.epoch == atomic_load(&s_epoch)) {
            atomic_store(&s_accepting, true);
        }
        return QR_HID_NOT_READY;
    }
    /* USB task always completes requests, including disconnect/suspend/timeouts.
     * Blocking here keeps the single scanner from accumulating offline scans. */
    xTaskNotifyWait(0, UINT32_MAX, &result, portMAX_DELAY);
    return (qr_hid_result_t)result;
}

void tud_mount_cb(void)
{
    atomic_store(&s_accepting, false);
    atomic_fetch_add(&s_epoch, 1);
    s_mounted = true;
    s_suspended = false;
    s_recovery = true;
    s_inflight = s_completed = s_failed = false;
    s_leds = 0;
    memset(&s_last_report, 0, sizeof(s_last_report));
    ESP_LOGI(TAG, "HID keyboard mounted");
}

void tud_umount_cb(void)
{
    atomic_store(&s_accepting, false);
    atomic_fetch_add(&s_epoch, 1);
    s_mounted = false;
    s_recovery = true;
    s_inflight = s_completed = s_failed = false;
    memset(&s_last_report, 0, sizeof(s_last_report));
    ESP_LOGI(TAG, "HID keyboard disconnected");
}

void tud_suspend_cb(bool remote_wakeup_en)
{
    (void)remote_wakeup_en;
    atomic_store(&s_accepting, false);
    atomic_fetch_add(&s_epoch, 1);
    s_suspended = true;
    s_recovery = true;
}

void tud_resume_cb(void)
{
    s_suspended = false;
    s_recovery = true;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t type, uint8_t *buffer, uint16_t reqlen)
{
    if (instance != 0 || report_id != 0) {
        return 0;
    }
    if (type == HID_REPORT_TYPE_INPUT) {
        uint16_t length = reqlen < sizeof(s_last_report) ? reqlen : sizeof(s_last_report);
        memcpy(buffer, &s_last_report, length);
        return length;
    }
    if (type == HID_REPORT_TYPE_OUTPUT && reqlen) {
        buffer[0] = s_leds;
        return 1;
    }
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t type, const uint8_t *buffer, uint16_t size)
{
    if (instance == 0 && report_id == 0 && type == HID_REPORT_TYPE_OUTPUT && size) {
        s_leds = buffer[0]; /* Use host Caps Lock state without toggling it. */
    }
}

void tud_hid_report_complete_cb(uint8_t instance, const uint8_t *report, uint16_t len)
{
    if (instance != 0 || !s_inflight || len != sizeof(s_last_report)) {
        return;
    }
    memcpy(&s_last_report, report, sizeof(s_last_report));
    s_inflight = false;
    if (s_recovery_report) {
        s_recovery = false;
    } else {
        s_completed = true;
    }
}

void tud_hid_report_failed_cb(uint8_t instance, hid_report_type_t type,
                              const uint8_t *report, uint16_t size)
{
    (void)report;
    (void)size;
    if (instance == 0 && type == HID_REPORT_TYPE_INPUT) {
        s_inflight = false;
        s_failed = true;
    }
}

static bool send_report(const qr_keyboard_report_t *report, bool recovery)
{
    if (!tud_hid_ready() || !tud_hid_keyboard_report(0, report->modifier, report->keycode)) {
        return false;
    }
    s_inflight = true;
    s_recovery_report = recovery;
    return true;
}

static void hid_task(void *arg)
{
    (void)arg;
    qr_keyboard_sequence_t sequence = {0};
    bool active = false;
    int64_t progress_ms = 0;
    int64_t deadline_ms = 0;

    for (;;) {
        tud_task_ext(0, false);
        const int64_t now_ms = esp_timer_get_time() / 1000;
        bool connected = s_mounted && !s_suspended;
        if (active && (!connected || s_request.epoch != atomic_load(&s_epoch) ||
                       s_failed || now_ms - progress_ms >= HID_REPORT_TIMEOUT_MS || now_ms >= deadline_ms)) {
            /* A prefix might already be in the application. Complete as aborted;
             * release all keys before allowing a new scan, never replay a prefix. */
            xTaskNotify(s_request.caller, QR_HID_ABORTED, eSetValueWithOverwrite);
            active = false;
            s_recovery = true;
            atomic_store(&s_accepting, false);
            ESP_LOGW(TAG, "HID typing aborted; clear any partial input and present the code again");
        }
        if (s_failed) {
            s_recovery = true;
            s_failed = false;
        }
        if (s_completed) {
            s_completed = false;
            if (active) {
                qr_keyboard_complete(&sequence);
                progress_ms = now_ms;
                if (sequence.phase == QR_KEY_DONE) {
                    active = false;
                    xTaskNotify(s_request.caller, QR_HID_TYPED, eSetValueWithOverwrite);
                }
            }
        }

        if (!active && xQueueReceive(s_requests, &s_request, 0) == pdTRUE) {
            if (!connected || s_recovery || s_request.epoch != atomic_load(&s_epoch)) {
                xTaskNotify(s_request.caller, QR_HID_NOT_READY, eSetValueWithOverwrite);
            } else {
                qr_keyboard_begin(&sequence, s_request.payload, s_request.length, HID_APPEND_ENTER);
                active = true;
                progress_ms = now_ms;
                deadline_ms = now_ms + 2000 + (int64_t)(s_request.length + 2) * 100;
            }
        }

        if (connected && s_recovery && s_inflight && tud_hid_ready()) {
            /* The endpoint may have been cancelled during suspend/reset without
             * a completion callback. An idle endpoint can accept a key release. */
            s_inflight = false;
        }
        if (connected && !s_inflight) {
            qr_keyboard_report_t report = {0};
            if (s_recovery) {
                send_report(&report, true);
            } else if (active && qr_keyboard_report(&sequence, (s_leds & 0x02) != 0, &report)) {
                send_report(&report, false);
            }
        }
        /* Do not advertise readiness while a request is waiting in the queue. */
        atomic_store(&s_accepting, connected && !s_recovery && !active && uxQueueMessagesWaiting(s_requests) == 0);
        vTaskDelay(1);
    }
}

esp_err_t qr_hid_init(void)
{
    if (s_requests != NULL || tud_inited()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = qr_usb_descriptors_init();
    if (err != ESP_OK) {
        return err;
    }
    s_requests = xQueueCreate(1, sizeof(hid_request_t));
    if (!s_requests) {
        return ESP_ERR_NO_MEM;
    }
    usb_phy_handle_t phy = NULL;
    const usb_phy_config_t phy_config = {
        .controller = USB_PHY_CTRL_OTG,
        .target = USB_PHY_TARGET_INT,
        .otg_mode = USB_OTG_MODE_DEVICE,
        .otg_speed = USB_PHY_SPEED_FULL,
    };
    err = usb_new_phy(&phy_config, &phy);
    if (err != ESP_OK) {
        goto fail;
    }
    const tusb_rhport_init_t root_port = {.role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL};
    if (!tusb_init(0, &root_port)) {
        err = ESP_FAIL;
        goto fail_phy;
    }
    if (xTaskCreatePinnedToCore(hid_task, "qr_hid_usb", HID_TASK_STACK_BYTES, NULL,
                                5, NULL, 0) != pdPASS) {
        tud_deinit(0);
        err = ESP_ERR_NO_MEM;
        goto fail_phy;
    }
    ESP_LOGI(TAG, "USB HID keyboard initialized, US layout, append Enter=%d", HID_APPEND_ENTER);
    return ESP_OK;

fail_phy:
    usb_del_phy(phy);
fail:
    vQueueDelete(s_requests);
    s_requests = NULL;
    return err;
}
