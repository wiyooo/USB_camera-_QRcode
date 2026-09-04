cmake_minimum_required(VERSION 3.16)

option(CAMERA_OUTPUT_QR_USB "Decode QR codes and type results as a USB HID keyboard" ON)
option(CAMERA_OUTPUT_USB_UVC "When QR mode is OFF, build UVC instead of Wi-Fi web output" ON)
option(CAMERA_UVC_QR_TEST "Test firmware: UVC preview plus QR results on UART0 (overrides output modes)" OFF)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Dependency discovery runs in a separate CMake process. Export the mode so
# that early discovery and the actual compilation select the same components.
idf_build_set_property(CAMERA_OUTPUT_QR_USB "${CAMERA_OUTPUT_QR_USB}")
idf_build_set_property(CAMERA_OUTPUT_USB_UVC "${CAMERA_OUTPUT_USB_UVC}")
idf_build_set_property(CAMERA_UVC_QR_TEST "${CAMERA_UVC_QR_TEST}")
# Only build the camera application and its dependencies.
set(COMPONENTS main)
project(lcd_camera)

if(CAMERA_OUTPUT_QR_USB AND NOT CAMERA_UVC_QR_TEST)
    # IDF 5.4 builds every managed component, even require:no dependencies.
    # usb_device_uvc 1.3.1 injects its descriptors into TinyUSB with PUBLIC
    # target_sources(). Remove that injection after all components register;
    # leave the managed source files and the selectable UVC mode intact.
    idf_component_get_property(qr_tusb_lib espressif__tinyusb COMPONENT_LIB)
    idf_component_get_property(qr_uvc_lib espressif__usb_device_uvc COMPONENT_LIB)
    idf_component_get_property(qr_uvc_dir espressif__usb_device_uvc COMPONENT_DIR)
    # Its unused archive still compiles against its own UVC headers. It isn't
    # referenced by main in HID mode and contributes no runtime USB callbacks.
    target_include_directories(${qr_uvc_lib} BEFORE PRIVATE "${qr_uvc_dir}/tusb")
    foreach(qr_source_property SOURCES INTERFACE_SOURCES)
        get_target_property(qr_usb_sources ${qr_tusb_lib} ${qr_source_property})
        if(qr_usb_sources)
            list(FILTER qr_usb_sources EXCLUDE REGEX "espressif__usb_device_uvc/tusb/usb_descriptors\\.c$")
            set_property(TARGET ${qr_tusb_lib} PROPERTY ${qr_source_property} "${qr_usb_sources}")
        endif()
    endforeach()
endif()
