idf_build_get_property(CAMERA_OUTPUT_QR_USB CAMERA_OUTPUT_QR_USB)
idf_build_get_property(CAMERA_OUTPUT_USB_UVC CAMERA_OUTPUT_USB_UVC)
idf_build_get_property(CAMERA_UVC_QR_TEST CAMERA_UVC_QR_TEST)
set(camera_uvc_qr_test 0)

set(camera_sources
    "esp32_s3_szp.c"
    "camera_diagnostics.c"
    "main.c")
set(camera_private_requires
    esp32-camera
    esp_driver_ledc
    esp_driver_gpio
    driver)

if(CAMERA_UVC_QR_TEST)
    list(APPEND camera_sources "usb_camera.c" "qr_preview.c" "qr_payload.c")
    list(APPEND camera_private_requires usb_device_uvc quirc esp_timer)
    set(camera_output_qr_usb 0)
    set(camera_output_usb_uvc 1)
    set(camera_uvc_qr_test 1)
elseif(CAMERA_OUTPUT_QR_USB)
    list(APPEND camera_sources "qr_scanner.c" "qr_payload.c" "qr_keyboard.c"
         "usb_hid/qr_hid.c" "usb_hid/usb_descriptors.c")
    list(APPEND camera_private_requires quirc espressif__tinyusb usb esp_timer)
    set(camera_output_qr_usb 1)
    set(camera_output_usb_uvc 0)
elseif(CAMERA_OUTPUT_USB_UVC)
    list(APPEND camera_sources "usb_camera.c")
    list(APPEND camera_private_requires usb_device_uvc)
    set(camera_output_usb_uvc 1)
    set(camera_output_qr_usb 0)
else()
    list(APPEND camera_sources "web_camera.c")
    list(APPEND camera_private_requires
         esp_http_server
         esp_wifi
         esp_event
         esp_netif
         nvs_flash)
    set(camera_output_usb_uvc 0)
    set(camera_output_qr_usb 0)
endif()

idf_component_register(SRCS ${camera_sources}
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES ${camera_private_requires})

if(CAMERA_OUTPUT_QR_USB AND NOT CAMERA_UVC_QR_TEST)
    # Prefer the HID config. The root CMake removes UVC's descriptor injection.
    idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
    target_include_directories(${tusb_lib} BEFORE PUBLIC "${COMPONENT_DIR}/usb_hid")
endif()

target_compile_definitions(${COMPONENT_LIB} PRIVATE
                           CAMERA_OUTPUT_QR_USB=${camera_output_qr_usb}
                           CAMERA_OUTPUT_USB_UVC=${camera_output_usb_uvc}
                           CAMERA_UVC_QR_TEST=${camera_uvc_qr_test})
