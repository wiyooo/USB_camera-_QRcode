#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_camera.h"
#include "esp_err.h"

/* The decoder owns this buffer. UVC may use it for JPEG/USB between calls to
 * qr_preview_process(), after the preceding USB transfer has completed.
 * Never free or resize it separately; startup cleanup uses qr_preview_deinit(). */
esp_err_t qr_preview_init(uint8_t **shared_buffer, size_t *capacity);
void qr_preview_deinit(void);

/* Single caller: UVC task. Blocks until the worker stops reading the camera
 * frame and shared buffer. It never captures or returns a camera frame itself. */
void qr_preview_process(const camera_fb_t *frame);
