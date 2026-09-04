#include "usb_descriptors.h"

#include <stdio.h>
#include <string.h>
#include "esp_mac.h"
#include "tusb.h"

/* Development VID/PID, separate from the original UVC device identity. */
static const tusb_desc_device_t s_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0,
    .bDeviceSubClass = 0,
    .bDeviceProtocol = 0,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303a,
    .idProduct = 0x4004,
    .bcdDevice = 0x0100,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

static const uint8_t s_hid_report[] = {TUD_HID_REPORT_DESC_KEYBOARD()};
static const uint8_t s_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN, 0, 500),
    TUD_HID_DESCRIPTOR(0, 4, HID_ITF_PROTOCOL_KEYBOARD, sizeof(s_hid_report), 0x81, 8, 10),
};
static char s_serial[13];

esp_err_t qr_usb_descriptors_init(void)
{
    uint8_t mac[6];
    esp_err_t err = esp_efuse_mac_get_default(mac);
    if (err == ESP_OK) {
        snprintf(s_serial, sizeof(s_serial), "%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    return err;
}

const uint8_t *tud_descriptor_device_cb(void)
{
    return (const uint8_t *)&s_device;
}

const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
    return index == 0 ? s_configuration : NULL;
}

const uint8_t *tud_hid_descriptor_report_cb(uint8_t instance)
{
    return instance == 0 ? s_hid_report : NULL;
}

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    static uint16_t descriptor[32];
    static const char *const strings[] = {NULL, "LCSC", "GC2145 QR HID Keyboard", s_serial, "QR Scanner"};
    size_t length;
    if (index == 0) {
        descriptor[1] = 0x0409;
        length = 1;
    } else {
        if (index >= sizeof(strings) / sizeof(strings[0])) {
            return NULL;
        }
        length = strlen(strings[index]);
        if (length > 31) {
            length = 31;
        }
        for (size_t i = 0; i < length; ++i) {
            descriptor[1 + i] = (uint8_t)strings[index][i];
        }
    }
    descriptor[0] = (TUSB_DESC_STRING << 8) | (2 * length + 2);
    return descriptor;
}
