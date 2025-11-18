#ifndef QR_CAM_CORE_H
#define QR_CAM_CORE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_camera.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sensor.h"

#include "camera_pins.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MICROPY_CAMERA_PIN_PWDN
#define MICROPY_CAMERA_PIN_PWDN    (-1)
#endif
#ifndef MICROPY_CAMERA_PIN_RESET
#define MICROPY_CAMERA_PIN_RESET   (-1)
#endif
#ifndef MICROPY_CAMERA_PIN_XCLK
#define MICROPY_CAMERA_PIN_XCLK    (-1)
#endif
#ifndef MICROPY_CAMERA_PIN_SIOD
#define MICROPY_CAMERA_PIN_SIOD    (-1)
#endif
#ifndef MICROPY_CAMERA_PIN_SIOC
#define MICROPY_CAMERA_PIN_SIOC    (-1)
#endif
#ifndef MICROPY_CAMERA_PIN_D7
#define MICROPY_CAMERA_PIN_D7      (-1)
#endif
#ifndef MICROPY_CAMERA_PIN_D6
#define MICROPY_CAMERA_PIN_D6      (-1)
#endif
#ifndef MICROPY_CAMERA_PIN_D5
#define MICROPY_CAMERA_PIN_D5      (-1)
#endif
#ifndef MICROPY_CAMERA_PIN_D4
#define MICROPY_CAMERA_PIN_D4      (-1)
#endif
#ifndef MICROPY_CAMERA_PIN_D3
#define MICROPY_CAMERA_PIN_D3      (-1)
#endif
#ifndef MICROPY_CAMERA_PIN_D2
#define MICROPY_CAMERA_PIN_D2      (-1)
#endif
#ifndef MICROPY_CAMERA_PIN_D1
#define MICROPY_CAMERA_PIN_D1      (-1)
#endif
#ifndef MICROPY_CAMERA_PIN_D0
#define MICROPY_CAMERA_PIN_D0      (-1)
#endif
#ifndef MICROPY_CAMERA_PIN_VSYNC
#define MICROPY_CAMERA_PIN_VSYNC   (-1)
#endif
#ifndef MICROPY_CAMERA_PIN_HREF
#define MICROPY_CAMERA_PIN_HREF    (-1)
#endif
#ifndef MICROPY_CAMERA_PIN_PCLK
#define MICROPY_CAMERA_PIN_PCLK    (-1)
#endif

#define QR_CAM_MAX_TEXT              (256)
#define QR_CAM_MAX_DECODERS          (3)

typedef struct {
    char text[QR_CAM_MAX_TEXT];
    uint16_t text_len;
    uint8_t version;
    uint8_t ecc_level;
    uint8_t mask;
    uint32_t frame_id;
    uint32_t decode_ms;
} qr_cam_result_t;

typedef struct {
    uint32_t cap_frames;
    uint32_t decoded_ok;
    uint32_t decoded_candidates;
    uint32_t decode_errors;
    uint32_t dropped_frames;
    uint32_t preview_seq;
    uint32_t preview_drops;
    uint32_t result_drops;
    uint32_t last_error;
} qr_cam_stats_t;

typedef struct {
    camera_config_t cam;
    framesize_t frame_size;
    bool mirror;
    bool flip_retry;
    uint8_t decode_tasks;
    uint8_t max_decode_per_frame;
    uint8_t frames_q_len;
    uint8_t results_q_len;
    int capture_core;
    int decode_core;
} qr_cam_config_t;

typedef struct {
    camera_config_t cam;
    framesize_t frame_size;
    bool mirror;
    bool flip_retry;
    uint8_t max_decode_per_frame;
    uint8_t decode_tasks;
    uint8_t frames_q_len;
    uint8_t results_q_len;
    QueueHandle_t frames_q;
    QueueHandle_t results_q;
    TaskHandle_t capture_task;
    TaskHandle_t decoder_tasks[QR_CAM_MAX_DECODERS];
    SemaphoreHandle_t preview_mtx;
    uint8_t *preview_buf;
    size_t preview_len;
    uint16_t preview_width;
    uint16_t preview_height;
    volatile bool running;
    volatile bool stop_requested;
    uint32_t frame_seq;
    uint32_t preview_seq;
    bool camera_active;
    qr_cam_stats_t stats;
} qr_cam_ctx_t;

void qr_cam_config_init_defaults(qr_cam_config_t *cfg);

bool qr_cam_start(qr_cam_ctx_t *ctx, const qr_cam_config_t *cfg);
void qr_cam_stop(qr_cam_ctx_t *ctx);

uint32_t qr_cam_results_available(const qr_cam_ctx_t *ctx);
bool qr_cam_try_get_result(qr_cam_ctx_t *ctx, qr_cam_result_t *out);

size_t qr_cam_preview_copy(qr_cam_ctx_t *ctx, uint8_t *dst, size_t dst_len, bool mirror);
bool qr_cam_preview_size(const qr_cam_ctx_t *ctx, uint16_t *out_w, uint16_t *out_h);
size_t qr_cam_preview_rgb565(qr_cam_ctx_t *ctx, uint8_t *dst, size_t dst_len, bool mirror);
size_t qr_cam_preview_rgb565_scaled(qr_cam_ctx_t *ctx, uint8_t *dst, int out_w, int out_h, size_t dst_len, bool mirror);

void qr_cam_get_stats(qr_cam_ctx_t *ctx, qr_cam_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif // QR_CAM_CORE_H

