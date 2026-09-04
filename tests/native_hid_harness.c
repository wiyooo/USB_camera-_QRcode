#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef NDEBUG
#error "Tests require assertions"
#endif

/* Include production code so tests exercise the real task loop and callbacks. */
#include "../main/usb_hid/qr_hid.c"
#include "../main/usb_hid/usb_descriptors.c"

static jmp_buf stop_loop;
static int clock_ms, stop_ms, scenario, fail_init;
static bool queue_full, pending, mock_inited, inject_request, injected, disconnected;
static hid_request_t queued;
static qr_keyboard_report_t pending_report, reports[100];
static int report_count, notify_count, queue_deletes, phy_deletes;
static uint32_t notified;

QueueHandle_t xQueueCreate(unsigned count, size_t size)
{
    assert(count == 1 && size == sizeof(hid_request_t));
    return fail_init == 1 ? NULL : &queued;
}
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait)
{
    assert(q == &queued && wait == 0);
    if (queue_full) return pdFALSE;
    queued = *(const hid_request_t *)item;
    queue_full = true;
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t wait)
{
    assert(q == &queued && wait == 0);
    if (!queue_full) return pdFALSE;
    *(hid_request_t *)item = queued;
    queue_full = false;
    return pdTRUE;
}
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q) { (void)q; return queue_full; }
void vQueueDelete(QueueHandle_t q) { assert(q == &queued); ++queue_deletes; }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (TaskHandle_t)1; }
BaseType_t xTaskNotifyWait(uint32_t a, uint32_t b, uint32_t *value, TickType_t wait)
{
    (void)a; (void)b; (void)value;
    assert(wait == 0); /* Blocking caller isn't used by the deterministic task simulator. */
    return pdFALSE;
}
BaseType_t xTaskNotify(TaskHandle_t task, uint32_t value, int action)
{
    assert(task == (TaskHandle_t)1 && action == eSetValueWithOverwrite);
    notified = value;
    ++notify_count;
    return pdTRUE;
}
BaseType_t xTaskCreatePinnedToCore(void (*task)(void *), const char *name, unsigned stack,
                                  void *arg, unsigned priority, TaskHandle_t *handle, int core)
{
    (void)name; (void)arg; (void)handle;
    assert(task == hid_task && stack >= 4096 && priority == 5 && core == 0);
    return fail_init == 4 ? pdFALSE : pdPASS;
}
void vTaskDelay(TickType_t ticks)
{
    assert(ticks == 1);
    clock_ms += 10;
    if (inject_request && !injected && qr_hid_ready()) {
        hid_request_t req = {.caller=(TaskHandle_t)1, .epoch=atomic_load(&s_epoch), .length=2, .payload={'A','A'}};
        if (scenario == 5) --req.epoch; /* Stale queued request after re-enumeration. */
        assert(xQueueSend(s_requests, &req, 0));
        atomic_store(&s_accepting, false);
        injected = true;
    }
    if (disconnected && clock_ms == 300) {
        if (scenario == 4) tud_resume_cb(); else tud_mount_cb();
    }
    if (clock_ms >= stop_ms) longjmp(stop_loop, 1);
}
int64_t esp_timer_get_time(void) { return (int64_t)clock_ms * 1000; }
esp_err_t esp_efuse_mac_get_default(uint8_t *mac)
{
    const uint8_t bytes[] = {0x10,0x20,0x30,0x40,0x50,0x60};
    memcpy(mac, bytes, 6);
    return ESP_OK;
}
esp_err_t usb_new_phy(const usb_phy_config_t *config, usb_phy_handle_t *phy)
{
    assert(config->controller == USB_PHY_CTRL_OTG && config->target == USB_PHY_TARGET_INT);
    if (fail_init == 2) return ESP_FAIL;
    *phy = (usb_phy_handle_t)1;
    return ESP_OK;
}
esp_err_t usb_del_phy(usb_phy_handle_t phy) { assert(phy == (void *)1); ++phy_deletes; return ESP_OK; }
bool tud_inited(void) { return mock_inited; }
bool tusb_rhport_init(uint8_t rhport, const tusb_rhport_init_t *init)
{
    assert(rhport == 0 && init->role == TUSB_ROLE_DEVICE && init->speed == TUSB_SPEED_FULL);
    mock_inited = fail_init != 3;
    return mock_inited;
}
bool tud_deinit(uint8_t rhport) { assert(rhport == 0); mock_inited = false; return true; }
bool tud_hid_n_ready(uint8_t instance) { assert(instance == 0); return s_mounted && !s_suspended && !pending; }
bool tud_hid_n_keyboard_report(uint8_t instance, uint8_t report_id, uint8_t modifier, const uint8_t keycode[6])
{
    assert(instance == 0 && report_id == 0 && !pending);
    /* Rejected submissions must not advance to the next key. */
    if (scenario == 6 && clock_ms < 100) return false;
    pending_report = (qr_keyboard_report_t){.modifier=modifier};
    memcpy(pending_report.keycode, keycode, 6);
    assert(report_count < 100);
    reports[report_count++] = pending_report;
    pending = true;
    return true;
}
void tud_task_ext(uint32_t timeout, bool in_isr)
{
    assert(timeout == 0 && !in_isr);
    if (!pending) return;
    if (pending_report.keycode[0] && !disconnected && (scenario == 2 || scenario == 4)) {
        pending = false;
        disconnected = true;
        if (scenario == 4) tud_suspend_cb(false); else tud_umount_cb();
        return;
    }
    if (scenario == 1 && pending_report.keycode[0] && clock_ms < 1800) return;
    pending = false;
    if (scenario == 3 && pending_report.keycode[0]) {
        tud_hid_report_failed_cb(0, HID_REPORT_TYPE_INPUT, (uint8_t *)&pending_report, 0);
    } else {
        tud_hid_report_complete_cb(0, (uint8_t *)&pending_report, sizeof(pending_report));
    }
}

static void reset(void)
{
    clock_ms = report_count = notify_count = queue_deletes = phy_deletes = 0;
    scenario = fail_init = 0; stop_ms = 500;
    queue_full = pending = mock_inited = injected = disconnected = false;
    inject_request = true;
    s_requests = NULL;
    atomic_store(&s_accepting, false);
    atomic_store(&s_epoch, 0);
    s_mounted = s_suspended = s_inflight = s_completed = s_failed = false;
    s_recovery = true;
    memset(&s_last_report, 0, sizeof(s_last_report));
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    for (int failure = 1; failure <= 4; ++failure) {
        reset(); fail_init = failure;
        assert(qr_hid_init() != ESP_OK && s_requests == NULL && !mock_inited);
        assert(queue_deletes == (failure > 1));
        assert(phy_deletes == (failure > 2));
    }
    printf("PASS: HID init failure cleanup at queue, PHY, stack and task stages\n");

    assert(qr_usb_descriptors_init() == ESP_OK);
    const tusb_desc_device_t *dev = (const tusb_desc_device_t *)tud_descriptor_device_cb();
    assert(dev->bNumConfigurations == 1 && dev->bMaxPacketSize0 == 64);
    const uint8_t *cfg = tud_descriptor_configuration_cb(0);
    assert(cfg[2] == 34 && cfg[4] == 1);
    assert(cfg[14] == 3 && cfg[15] == 1 && cfg[16] == 1); /* HID / boot / keyboard */
    assert(cfg[29] == 0x81 && cfg[30] == 3 && cfg[31] == 8 && cfg[33] == 10);
    assert(tud_descriptor_configuration_cb(1) == NULL && tud_hid_descriptor_report_cb(1) == NULL);
    assert(tud_hid_descriptor_report_cb(0) != NULL);
    const uint16_t *serial = tud_descriptor_string_cb(3, 0x0409);
    assert((serial[0] & 255) == 26 && serial[1] == '1' && serial[12] == '0');
    assert(tud_descriptor_string_cb(99, 0) == NULL);
    printf("PASS: real device/config/HID/string descriptors, boot keyboard, 8-byte reports\n");

    for (int mode = 0; mode <= 6; ++mode) {
        reset(); scenario = mode; stop_ms = mode == 1 ? 2500 : 700;
        assert(qr_hid_init() == ESP_OK);
        tud_mount_cb();
        if (setjmp(stop_loop) == 0) hid_task(NULL);
        assert(injected && notify_count == 1);
        if (mode == 0 || mode == 6) {
            assert(notified == QR_HID_TYPED && report_count == 8); /* recovery, clear, 2*(A,A,Enter) */
            assert(reports[2].modifier == 2 && reports[2].keycode[0] == 4);
            assert(reports[3].modifier == 0 && reports[3].keycode[0] == 0);
            assert(reports[4].keycode[0] == 4 && reports[6].keycode[0] == 0x28);
        } else if (mode == 5) {
            assert(notified == QR_HID_NOT_READY && report_count == 1);
        } else {
            assert(notified == QR_HID_ABORTED);
            int presses = 0;
            for (int i = 0; i < report_count; ++i) if (reports[i].keycode[0]) ++presses;
            assert(presses == 1); /* No replay of a possibly typed prefix. */
        }
        assert(s_last_report.modifier == 0 && s_last_report.keycode[0] == 0);
        if (!qr_hid_ready()) printf("mode=%d inflight=%d recovery=%d pending=%d reports=%d\n",
                                   mode, s_inflight, s_recovery, pending, report_count);
        assert(qr_hid_ready());
    }
    uint8_t led = 2, report[8];
    tud_hid_set_report_cb(0, 0, HID_REPORT_TYPE_OUTPUT, &led, 1);
    assert(tud_hid_get_report_cb(0, 0, HID_REPORT_TYPE_OUTPUT, report, 1) == 1 && report[0] == 2);
    assert(tud_hid_get_report_cb(0, 0, HID_REPORT_TYPE_INPUT, report, 8) == 8);
    assert(qr_hid_type((const uint8_t *)"bad\x1b", 4) == QR_HID_UNSUPPORTED);
    printf("PASS: actual HID task completion, backpressure, timeout, disconnect, suspend, failure and stale-request rejection\n");
    return 0;
}
