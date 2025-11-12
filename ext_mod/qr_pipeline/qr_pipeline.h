#ifndef QR_PIPELINE_H
#define QR_PIPELINE_H

#include <stdbool.h>
#include <stdint.h>

#include "modcamera.h"          // for mp_camera_obj_t and esp_camera types
#include "esp_camera.h"          // camera_fb_t

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

// Max size of decoded text payload (truncate if longer)
#ifndef QR_MAX_TEXT
#define QR_MAX_TEXT 512
#endif

typedef struct {
    // Decoded text (UTF-8)
    char text[QR_MAX_TEXT];
    uint16_t text_len;     // number of bytes in text (<= QR_MAX_TEXT)

    // Metadata from quirc
    uint8_t version;
    uint8_t ecc_level;     // 0=L,1=M,2=Q,3=H per quirc
    uint8_t mask;

    // Optional timing/ids
    uint32_t frame_id;
    uint32_t decode_ms;
} qr_result_t;

typedef struct {
    // Shared state
    volatile bool running;

    // Camera handle supplied by MicroPython user (already constructed)
    mp_camera_obj_t *camera;

    // FreeRTOS primitives
    QueueHandle_t frames_q;    // camera_fb_t* items
    QueueHandle_t results_q;   // qr_result_t items

    // Tasks
    TaskHandle_t capture_task;
    TaskHandle_t decode_task1;
    TaskHandle_t decode_task2;

    // Bookkeeping
    uint32_t frame_seq;        // monotonically increasing frame id

    // Active frame geometry (set at start from camera)
    int width;
    int height;

    // Preview (latest grayscale frame) and lock
    uint8_t *preview_buf;
    size_t preview_len;
    SemaphoreHandle_t preview_mtx;
    uint32_t preview_seq;

    // Stats
    uint32_t cap_frames;
    uint32_t decoded_ok;
    uint32_t decoded_candidates; // total candidates seen from quirc_count
    uint32_t decode_errors;      // quirc_decode failures

    // Decode options
    bool mirror; // horizontally mirror input into quirc buffer
} qr_pipeline_t;

// Lifecycle
bool qr_pipeline_start(qr_pipeline_t *ctx, mp_camera_obj_t *camera, int frames_q_depth, int results_q_depth, int capture_core, int decode_core, bool mirror);
void qr_pipeline_stop(qr_pipeline_t *ctx);

// Result access (non-blocking); returns true if a result was read
bool qr_pipeline_try_get_result(qr_pipeline_t *ctx, qr_result_t *out);

// Introspection
UBaseType_t qr_pipeline_results_available(qr_pipeline_t *ctx);

// Copy the latest preview grayscale frame into dest (size must be >= width*height).
// Returns number of bytes copied, or 0 if not ready.
size_t qr_pipeline_preview_copy(qr_pipeline_t *ctx, uint8_t *dest, size_t dest_len);

// Convert latest preview grayscale to RGB565 into dest (Hi byte, then Lo byte per pixel).
// Returns number of bytes written, or 0 if not ready or buffer too small.
size_t qr_pipeline_preview_rgb565_into(qr_pipeline_t *ctx, uint8_t *dest, size_t dest_len);

// Convert latest preview grayscale to RGB565 with nearest-neighbor scaling into dest.
// out_w/out_h are the desired output dimensions. dest must be at least out_w*out_h*2 bytes.
// Returns number of bytes written, or 0 if not ready/invalid.
size_t qr_pipeline_preview_rgb565_scaled_into(qr_pipeline_t *ctx, uint8_t *dest, int out_w, int out_h, size_t dest_len);

// Retrieve simple stats
typedef struct {
    uint32_t cap_frames;
    uint32_t decoded_ok;
    uint32_t preview_seq;
    uint32_t decoded_candidates;
    uint32_t decode_errors;
} qr_stats_t;

void qr_pipeline_get_stats(qr_pipeline_t *ctx, qr_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif // QR_PIPELINE_H
