#include "qr_pipeline.h"

#include <string.h>
#include <stddef.h>
#include <stdio.h>

#include "quirc.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#ifndef APP_CPU_NUM
#define APP_CPU_NUM 1
#endif
#ifndef PRO_CPU_NUM
#define PRO_CPU_NUM 0
#endif

static const char *TAG = "qr_pipeline";

typedef struct {
    qr_pipeline_t *pipeline;
} task_arg_t;

// Capture loop: continuously grab frames from the camera and push to frames_q.
static void capture_task_entry(void *arg) {
    qr_pipeline_t *ctx = ((task_arg_t *)arg)->pipeline;
    vPortFree(arg);

    ESP_LOGI(TAG, "capture task started");
    while (ctx->running) {
        // Get a frame from the ESP32 camera driver.
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            // Small backoff if camera returns nothing
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        // Enforce expected format/size, drop otherwise to keep pipeline simple.
        if (fb->width != ctx->width || fb->height != ctx->height || fb->format != PIXFORMAT_GRAYSCALE || fb->len < (size_t)ctx->width * (size_t)ctx->height) {
            ESP_LOGW(TAG, "Dropping frame: unexpected format %d or size %ux%u", (int)fb->format, (unsigned)fb->width, (unsigned)fb->height);
            esp_camera_fb_return(fb);
            continue;
        }

        // Tag frame with a sequence number by reusing reserved field if any; otherwise, track separately.
        uint32_t seq = ctx->frame_seq++;
        (void)seq; // seq carried in result only, not stored in fb

        // Update preview buffer (copy latest grayscale frame)
        if (ctx->preview_buf && ctx->preview_len >= (size_t)ctx->width * (size_t)ctx->height) {
            if (xSemaphoreTake(ctx->preview_mtx, pdMS_TO_TICKS(2)) == pdTRUE) {
                memcpy(ctx->preview_buf, fb->buf, (size_t)ctx->width * (size_t)ctx->height);
                ctx->preview_seq++;
                xSemaphoreGive(ctx->preview_mtx);
            }
        }

        // Try to enqueue without blocking; drop oldest if full.
        if (xQueueSend(ctx->frames_q, &fb, 0) != pdTRUE) {
            // Queue full: return frame to driver (drop)
            esp_camera_fb_return(fb);
            // Yield briefly to allow decoders to catch up
            taskYIELD();
        }

        ctx->cap_frames++;
    }

    // Drain: when stopping, nothing special to do; camera frames returned by decoder or dropped above.
    ESP_LOGI(TAG, "capture task exiting");
    vTaskDelete(NULL);
}

// Each decoder owns its own quirc context to avoid contention.
static void decoder_task_entry(void *arg) {
    qr_pipeline_t *ctx = ((task_arg_t *)arg)->pipeline;
    vPortFree(arg);

    ESP_LOGI(TAG, "decoder task started");

    struct quirc *q = quirc_new();
    if (!q) {
        ESP_LOGE(TAG, "quirc_new failed");
        vTaskDelete(NULL);
        return;
    }
    if (quirc_resize(q, ctx->width, ctx->height) < 0) {
        ESP_LOGE(TAG, "quirc_resize failed");
        quirc_destroy(q);
        vTaskDelete(NULL);
        return;
    }

    // Allocate decode scratch on heap (avoid large stack frames)
    struct quirc_code *code = (struct quirc_code *)heap_caps_malloc(sizeof(struct quirc_code), MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    struct quirc_data *data = (struct quirc_data *)heap_caps_malloc(sizeof(struct quirc_data), MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    if (!code || !data) {
        ESP_LOGE(TAG, "malloc quirc_code/data failed");
        if (code) heap_caps_free(code);
        if (data) heap_caps_free(data);
        quirc_destroy(q);
        vTaskDelete(NULL);
        return;
    }

    while (ctx->running) {
        camera_fb_t *fb = NULL;
        if (xQueueReceive(ctx->frames_q, &fb, pdMS_TO_TICKS(50)) != pdTRUE) {
            // No frame available right now
            continue;
        }

        // Measure decode time
        int64_t t0 = esp_timer_get_time();

        int iw, ih;
        uint8_t *qbuf = quirc_begin(q, &iw, &ih);
        if (!qbuf || iw != ctx->width || ih != ctx->height) {
            // Should not happen since we resized; return frame and continue
            esp_camera_fb_return(fb);
            quirc_end(q);
            continue;
        }

        // Copy grayscale bytes into quirc buffer, optionally mirroring horizontally
        // fb->buf is 8-bit grayscale when PIXFORMAT_GRAYSCALE
        // Copy only the amount quirc expects (w*h), ensure fb len is sufficient (validated above)
        if (!ctx->mirror) {
            memcpy(qbuf, fb->buf, (size_t)ctx->width * (size_t)ctx->height);
        } else {
            // Mirror horizontally: for each row, reverse pixels
            const int w = ctx->width;
            const int h = ctx->height;
            const uint8_t *src = (const uint8_t *)fb->buf;
            for (int y = 0; y < h; y++) {
                const uint8_t *s = src + (size_t)y * (size_t)w;
                uint8_t *d = qbuf + (size_t)y * (size_t)w;
                for (int x = 0; x < w; x++) {
                    d[w - 1 - x] = s[x];
                }
            }
        }
        quirc_end(q);

        int num = quirc_count(q);
        if (num > 0) {
            ctx->decoded_candidates += (uint32_t)num;
        }
        // Decode at most 1 symbol per frame to limit CPU and avoid starving other tasks
        if (num > 1) num = 1;
        for (int i = 0; i < num; i++) {
            memset(code, 0, sizeof(*code));
            memset(data, 0, sizeof(*data));
            quirc_extract(q, i, code);
            if (!quirc_decode(code, data)) {
                qr_result_t res = {0};
                // Copy payload (truncate if needed)
                uint16_t n = (data->payload_len > QR_MAX_TEXT - 1) ? (QR_MAX_TEXT - 1) : (uint16_t)data->payload_len;
                if (n > 0) {
                    memcpy(res.text, data->payload, n);
                }
                res.text[n] = '\0';
                res.text_len = n;
                res.version = data->version;
                res.ecc_level = data->ecc_level;
                res.mask = data->mask;
                res.frame_id = 0; // We keep a global counter but not tied to fb
                res.decode_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);

                (void)xQueueSend(ctx->results_q, &res, 0);
                ctx->decoded_ok++;
            } else {
                ctx->decode_errors++;
            }
        }

        // Return the frame buffer to the camera driver
        esp_camera_fb_return(fb);

        // Cooperative yield to prevent starving lower-priority tasks and WDT
        vTaskDelay(1);
    }

    heap_caps_free(code);
    heap_caps_free(data);
    quirc_destroy(q);
    ESP_LOGI(TAG, "decoder task exiting");
    vTaskDelete(NULL);
}

bool qr_pipeline_start(qr_pipeline_t *ctx, mp_camera_obj_t *camera, int frames_q_depth, int results_q_depth, int capture_core, int decode_core, bool mirror) {
    if (!ctx || !camera) {
        return false;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->camera = camera;
    ctx->running = true;
    ctx->mirror = mirror;

    // Ensure grayscale and grab latest (preserve current frame size to keep UI in sync)
    mp_camera_framesize_t fs = mp_camera_hal_get_frame_size(camera);
    int fb = mp_camera_hal_get_fb_count(camera);
    if (fb < 2) fb = 2; // Prefer double buffering on S3
    mp_camera_hal_reconfigure(camera, fs, PIXFORMAT_GRAYSCALE, CAMERA_GRAB_LATEST, fb);

    // Cache active geometry
    ctx->width = mp_camera_hal_get_pixel_width(camera);
    ctx->height = mp_camera_hal_get_pixel_height(camera);

    // Create queues
    if (frames_q_depth <= 0) frames_q_depth = 4;
    if (results_q_depth <= 0) results_q_depth = 8;
    ctx->frames_q = xQueueCreate((UBaseType_t)frames_q_depth, sizeof(camera_fb_t *));
    ctx->results_q = xQueueCreate((UBaseType_t)results_q_depth, sizeof(qr_result_t));
    if (!ctx->frames_q || !ctx->results_q) {
        if (ctx->frames_q) vQueueDelete(ctx->frames_q);
        if (ctx->results_q) vQueueDelete(ctx->results_q);
        ctx->running = false;
        return false;
    }

    // Allocate preview buffer and mutex
    ctx->preview_len = (size_t)ctx->width * (size_t)ctx->height;
    ctx->preview_mtx = xSemaphoreCreateMutex();
    ctx->preview_buf = (uint8_t *)heap_caps_malloc(ctx->preview_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ctx->preview_mtx || !ctx->preview_buf) {
        if (ctx->preview_mtx) vSemaphoreDelete(ctx->preview_mtx);
        ctx->preview_mtx = NULL;
        if (ctx->preview_buf) heap_caps_free(ctx->preview_buf);
        ctx->preview_buf = NULL;
        vQueueDelete(ctx->frames_q);
        vQueueDelete(ctx->results_q);
        ctx->frames_q = NULL;
        ctx->results_q = NULL;
        ctx->running = false;
        return false;
    }

    // Spawn tasks (use pinned variant if available; otherwise fall back to xTaskCreate)
    BaseType_t ok;

    task_arg_t *cap_arg = (task_arg_t *)pvPortMalloc(sizeof(task_arg_t));
    if (!cap_arg) return false;
    cap_arg->pipeline = ctx;
#if defined(CONFIG_FREERTOS_UNICORE)
    ok = xTaskCreate(capture_task_entry, "qr_cap", 8192, cap_arg, 4, &ctx->capture_task);
#else
    #ifdef xTaskCreatePinnedToCore
    ok = xTaskCreatePinnedToCore(capture_task_entry, "qr_cap", 8192, cap_arg, 4,
                                 &ctx->capture_task, (capture_core >= 0) ? capture_core : APP_CPU_NUM);
    #else
    ok = xTaskCreate(capture_task_entry, "qr_cap", 8192, cap_arg, 4, &ctx->capture_task);
    #endif
#endif
    if (ok != pdPASS) {
        vPortFree(cap_arg);
        return false;
    }

    task_arg_t *dec1_arg = (task_arg_t *)pvPortMalloc(sizeof(task_arg_t));
    if (!dec1_arg) return false;
    dec1_arg->pipeline = ctx;
#if defined(CONFIG_FREERTOS_UNICORE)
    ok = xTaskCreate(decoder_task_entry, "qr_dec1", 24576, dec1_arg, 4, &ctx->decode_task1);
#else
    #ifdef xTaskCreatePinnedToCore
    ok = xTaskCreatePinnedToCore(decoder_task_entry, "qr_dec1", 24576, dec1_arg, 4,
                                 &ctx->decode_task1, (decode_core >= 0) ? decode_core : PRO_CPU_NUM);
    #else
    ok = xTaskCreate(decoder_task_entry, "qr_dec1", 24576, dec1_arg, 4, &ctx->decode_task1);
    #endif
#endif
    if (ok != pdPASS) {
        vPortFree(dec1_arg);
        return false;
    }

    task_arg_t *dec2_arg = (task_arg_t *)pvPortMalloc(sizeof(task_arg_t));
    if (!dec2_arg) return false;
    dec2_arg->pipeline = ctx;
#if defined(CONFIG_FREERTOS_UNICORE)
    ok = xTaskCreate(decoder_task_entry, "qr_dec2", 24576, dec2_arg, 4, &ctx->decode_task2);
#else
    #ifdef xTaskCreatePinnedToCore
    ok = xTaskCreatePinnedToCore(decoder_task_entry, "qr_dec2", 24576, dec2_arg, 4,
                                 &ctx->decode_task2, (decode_core >= 0) ? decode_core : PRO_CPU_NUM);
    #else
    ok = xTaskCreate(decoder_task_entry, "qr_dec2", 24576, dec2_arg, 4, &ctx->decode_task2);
    #endif
#endif
    if (ok != pdPASS) {
        vPortFree(dec2_arg);
        return false;
    }

    return true;
}

void qr_pipeline_stop(qr_pipeline_t *ctx) {
    if (!ctx) return;
    ctx->running = false;

    // Allow tasks to exit
    vTaskDelay(pdMS_TO_TICKS(10));

    if (ctx->capture_task) {
        vTaskDelete(ctx->capture_task);
        ctx->capture_task = NULL;
    }
    if (ctx->decode_task1) {
        vTaskDelete(ctx->decode_task1);
        ctx->decode_task1 = NULL;
    }
    if (ctx->decode_task2) {
        vTaskDelete(ctx->decode_task2);
        ctx->decode_task2 = NULL;
    }

    // Drain any frames still queued and return them to driver
    camera_fb_t *fb = NULL;
    while (ctx->frames_q && xQueueReceive(ctx->frames_q, &fb, 0) == pdTRUE) {
        if (fb) esp_camera_fb_return(fb);
    }

    if (ctx->frames_q) {
        vQueueDelete(ctx->frames_q);
        ctx->frames_q = NULL;
    }
    if (ctx->results_q) {
        vQueueDelete(ctx->results_q);
        ctx->results_q = NULL;
    }

    if (ctx->preview_buf) {
        heap_caps_free(ctx->preview_buf);
        ctx->preview_buf = NULL;
    }
    if (ctx->preview_mtx) {
        vSemaphoreDelete(ctx->preview_mtx);
        ctx->preview_mtx = NULL;
    }
}

bool qr_pipeline_try_get_result(qr_pipeline_t *ctx, qr_result_t *out) {
    if (!ctx || !out || !ctx->results_q) return false;
    return xQueueReceive(ctx->results_q, out, 0) == pdTRUE;
}

UBaseType_t qr_pipeline_results_available(qr_pipeline_t *ctx) {
    if (!ctx || !ctx->results_q) return 0;
    return uxQueueMessagesWaiting(ctx->results_q);
}

size_t qr_pipeline_preview_copy(qr_pipeline_t *ctx, uint8_t *dest, size_t dest_len) {
    if (!ctx || !ctx->preview_buf || !dest) return 0;
    size_t need = (size_t)ctx->width * (size_t)ctx->height;
    if (dest_len < need) return 0;
    if (xSemaphoreTake(ctx->preview_mtx, pdMS_TO_TICKS(5)) != pdTRUE) return 0;
    memcpy(dest, ctx->preview_buf, need);
    xSemaphoreGive(ctx->preview_mtx);
    return need;
}

void qr_pipeline_get_stats(qr_pipeline_t *ctx, qr_stats_t *out) {
    if (!ctx || !out) return;
    out->cap_frames = ctx->cap_frames;
    out->decoded_ok = ctx->decoded_ok;
    out->preview_seq = ctx->preview_seq;
    out->decoded_candidates = ctx->decoded_candidates;
    out->decode_errors = ctx->decode_errors;
}

size_t qr_pipeline_preview_rgb565_into(qr_pipeline_t *ctx, uint8_t *dest, size_t dest_len) {
    if (!ctx || !ctx->preview_buf || !dest) return 0;
    size_t pixels = (size_t)ctx->width * (size_t)ctx->height;
    size_t need = pixels * 2;
    if (dest_len < need) return 0;
    if (xSemaphoreTake(ctx->preview_mtx, pdMS_TO_TICKS(5)) != pdTRUE) return 0;
    // Convert grayscale preview to RGB565 (hi,lo)
    uint8_t *src = ctx->preview_buf;
    size_t di = 0;
    for (size_t i = 0; i < pixels; i++) {
        uint8_t g = src[i];
        uint16_t r = (uint16_t)(g >> 3);
        uint16_t gg = (uint16_t)(g >> 2);
        uint16_t b = (uint16_t)(g >> 3);
        uint16_t p = (r << 11) | (gg << 5) | b;
        dest[di++] = (uint8_t)(p >> 8);
        dest[di++] = (uint8_t)(p & 0xFF);
    }
    xSemaphoreGive(ctx->preview_mtx);
    return need;
}

size_t qr_pipeline_preview_rgb565_scaled_into(qr_pipeline_t *ctx, uint8_t *dest, int out_w, int out_h, size_t dest_len) {
    if (!ctx || !ctx->preview_buf || !dest) return 0;
    if (out_w <= 0 || out_h <= 0) return 0;
    size_t need = (size_t)out_w * (size_t)out_h * 2;
    if (dest_len < need) return 0;
    const int src_w = ctx->width;
    const int src_h = ctx->height;
    if (xSemaphoreTake(ctx->preview_mtx, pdMS_TO_TICKS(5)) != pdTRUE) return 0;
    const uint8_t *src = ctx->preview_buf;
    // Fixed-point stepping to avoid float
    uint32_t x_step = ((uint32_t)src_w << 16) / (uint32_t)out_w;
    uint32_t y_step = ((uint32_t)src_h << 16) / (uint32_t)out_h;
    size_t di = 0;
    uint32_t y_acc = 0;
    for (int y = 0; y < out_h; y++, y_acc += y_step) {
        int sy = (int)(y_acc >> 16);
        if (sy >= src_h) sy = src_h - 1;
        const uint8_t *row = src + (size_t)sy * (size_t)src_w;
        uint32_t x_acc = 0;
        for (int x = 0; x < out_w; x++, x_acc += x_step) {
            int sx = (int)(x_acc >> 16);
            if (sx >= src_w) sx = src_w - 1;
            uint8_t g = row[sx];
            uint16_t r = (uint16_t)(g >> 3);
            uint16_t gg = (uint16_t)(g >> 2);
            uint16_t b = (uint16_t)(g >> 3);
            uint16_t p = (r << 11) | (gg << 5) | b;
            dest[di++] = (uint8_t)(p >> 8);
            dest[di++] = (uint8_t)(p & 0xFF);
        }
    }
    xSemaphoreGive(ctx->preview_mtx);
    return need;
}
