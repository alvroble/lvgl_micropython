#include "qr_cam_core.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "quirc.h"

#ifndef APP_CPU_NUM
#define APP_CPU_NUM 1
#endif
#ifndef PRO_CPU_NUM
#define PRO_CPU_NUM 0
#endif

#if !CONFIG_SPIRAM
#error "qr_cam requires CONFIG_SPIRAM enabled"
#endif

static const char *TAG = "qr_cam";

typedef struct {
    camera_fb_t *fb;
    uint32_t frame_id;
} qr_cam_frame_packet_t;

static uint16_t s_gray_to_rgb565[256];
static bool s_gray_to_rgb565_ready = false;

static inline void qr_cam_init_gray_lut(void) {
    if (s_gray_to_rgb565_ready) {
        return;
    }
    for (int i = 0; i < 256; ++i) {
        uint8_t g = (uint8_t)i;
        uint16_t r = (uint16_t)(g >> 3) & 0x1F;
        uint16_t g6 = (uint16_t)(g >> 2) & 0x3F;
        uint16_t b = r;
        s_gray_to_rgb565[i] = (r << 11) | (g6 << 5) | b;
    }
    s_gray_to_rgb565_ready = true;
}

static void qr_cam_detach_decoder(qr_cam_ctx_t *ctx) {
    if (!ctx) {
        return;
    }
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    for (uint8_t i = 0; i < QR_CAM_MAX_DECODERS; ++i) {
        if (ctx->decoder_tasks[i] == self) {
            ctx->decoder_tasks[i] = NULL;
            break;
        }
    }
}

static inline bool qr_cam_framesize_dims(framesize_t fs, uint16_t *w, uint16_t *h) {
    if (fs < 0 || fs >= FRAMESIZE_INVALID) {
        return false;
    }
    const resolution_info_t info = resolution[fs];
    if (info.width == 0 || info.height == 0) {
        return false;
    }
    if (w) {
        *w = info.width;
    }
    if (h) {
        *h = info.height;
    }
    return true;
}

static inline void copy_gray(uint8_t *dst, const uint8_t *src, uint16_t w, uint16_t h, bool mirror) {
    if (!dst || !src || w == 0 || h == 0) {
        return;
    }
    if (!mirror) {
        memcpy(dst, src, (size_t)w * h);
        return;
    }
    for (uint16_t y = 0; y < h; ++y) {
        const uint8_t *s = src + (size_t)y * w;
        uint8_t *d = dst + (size_t)y * w;
        for (uint16_t x = 0; x < w; ++x) {
            d[w - 1 - x] = s[x];
        }
    }
}

static void qr_cam_reset_ctx(qr_cam_ctx_t *ctx, bool clear_stats) {
    if (!ctx) {
        return;
    }
    ctx->frames_q = NULL;
    ctx->results_q = NULL;
    ctx->capture_task = NULL;
    for (int i = 0; i < QR_CAM_MAX_DECODERS; ++i) {
        ctx->decoder_tasks[i] = NULL;
    }
    ctx->preview_mtx = NULL;
    ctx->preview_buf = NULL;
    ctx->preview_len = 0;
    ctx->preview_width = 0;
    ctx->preview_height = 0;
    ctx->running = false;
    ctx->stop_requested = false;
    ctx->camera_active = false;
    if (clear_stats) {
        ctx->frame_seq = 0;
        ctx->preview_seq = 0;
        memset(&ctx->stats, 0, sizeof(ctx->stats));
    }
}

void qr_cam_config_init_defaults(qr_cam_config_t *cfg) {
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->cam.pin_pwdn = MICROPY_CAMERA_PIN_PWDN;
    cfg->cam.pin_reset = MICROPY_CAMERA_PIN_RESET;
    cfg->cam.pin_xclk = MICROPY_CAMERA_PIN_XCLK;
    cfg->cam.pin_sccb_sda = MICROPY_CAMERA_PIN_SIOD;
    cfg->cam.pin_sccb_scl = MICROPY_CAMERA_PIN_SIOC;
    cfg->cam.pin_d7 = MICROPY_CAMERA_PIN_D7;
    cfg->cam.pin_d6 = MICROPY_CAMERA_PIN_D6;
    cfg->cam.pin_d5 = MICROPY_CAMERA_PIN_D5;
    cfg->cam.pin_d4 = MICROPY_CAMERA_PIN_D4;
    cfg->cam.pin_d3 = MICROPY_CAMERA_PIN_D3;
    cfg->cam.pin_d2 = MICROPY_CAMERA_PIN_D2;
    cfg->cam.pin_d1 = MICROPY_CAMERA_PIN_D1;
    cfg->cam.pin_d0 = MICROPY_CAMERA_PIN_D0;
    cfg->cam.pin_vsync = MICROPY_CAMERA_PIN_VSYNC;
    cfg->cam.pin_href = MICROPY_CAMERA_PIN_HREF;
    cfg->cam.pin_pclk = MICROPY_CAMERA_PIN_PCLK;

    cfg->cam.xclk_freq_hz = 20000000;
    cfg->cam.ledc_timer = LEDC_TIMER_3;
    cfg->cam.ledc_channel = LEDC_CHANNEL_0;
    cfg->cam.pixel_format = PIXFORMAT_GRAYSCALE;
    cfg->cam.frame_size = FRAMESIZE_QVGA;
    cfg->cam.jpeg_quality = 12;
    cfg->cam.fb_count = 3;
    cfg->cam.fb_location = CAMERA_FB_IN_PSRAM;
    cfg->cam.grab_mode = CAMERA_GRAB_LATEST;
#if CONFIG_CAMERA_CONVERTER_ENABLED
    cfg->cam.conv_mode = CONV_DISABLE;
#endif
    cfg->cam.sccb_i2c_port = -1;

    cfg->frame_size = FRAMESIZE_QVGA;
    cfg->mirror = false;
    cfg->flip_retry = true;
    cfg->decode_tasks = 2;
    cfg->max_decode_per_frame = 3;
    cfg->frames_q_len = 4;
    cfg->results_q_len = 8;
    cfg->capture_core = -1;
    cfg->decode_core = -1;
}

static inline uint8_t clamp_decoder_count(uint8_t count) {
    if (count == 0) {
        count = 1;
    } else if (count > QR_CAM_MAX_DECODERS) {
        count = QR_CAM_MAX_DECODERS;
    }
    return count;
}

static inline uint8_t clamp_queue_len(uint8_t value, uint8_t min_value, uint8_t fallback) {
    if (value < min_value) {
        return fallback;
    }
    return value;
}

static void qr_cam_cleanup(qr_cam_ctx_t *ctx) {
    if (!ctx) {
        return;
    }
    if (ctx->frames_q) {
        qr_cam_frame_packet_t pkt;
        while (xQueueReceive(ctx->frames_q, &pkt, 0) == pdTRUE) {
            if (pkt.fb) {
                esp_camera_fb_return(pkt.fb);
            }
        }
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
    qr_cam_reset_ctx(ctx, false);
}

static bool qr_cam_alloc_preview(qr_cam_ctx_t *ctx) {
    if (!ctx) {
        return false;
    }
    ctx->preview_len = (size_t)ctx->preview_width * ctx->preview_height;
    if (ctx->preview_len == 0) {
        return false;
    }
    ctx->preview_buf = (uint8_t *)heap_caps_malloc(ctx->preview_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ctx->preview_buf) {
        ESP_LOGE(TAG, "failed to allocate preview buffer (%u bytes)", (unsigned)ctx->preview_len);
        return false;
    }
    memset(ctx->preview_buf, 0, ctx->preview_len);
    ctx->preview_mtx = xSemaphoreCreateMutex();
    if (!ctx->preview_mtx) {
        ESP_LOGE(TAG, "failed to create preview mutex");
        heap_caps_free(ctx->preview_buf);
        ctx->preview_buf = NULL;
        return false;
    }
    return true;
}

static void qr_cam_capture_task(void *arg);
static void qr_cam_decoder_task(void *arg);

bool qr_cam_start(qr_cam_ctx_t *ctx, const qr_cam_config_t *cfg) {
    if (!ctx || !cfg) {
        return false;
    }
    if (ctx->running) {
        ESP_LOGW(TAG, "qr_cam already running");
        return true;
    }

    qr_cam_reset_ctx(ctx, true);
    ctx->frame_size = cfg->frame_size;
    ctx->mirror = cfg->mirror;
    ctx->flip_retry = cfg->flip_retry;
    ctx->max_decode_per_frame = cfg->max_decode_per_frame == 0 ? 1 : cfg->max_decode_per_frame;
    if (ctx->max_decode_per_frame > 8) {
        ctx->max_decode_per_frame = 8;
    }
    ctx->decode_tasks = clamp_decoder_count(cfg->decode_tasks);
    ctx->frames_q_len = clamp_queue_len(cfg->frames_q_len, 2, 4);
    ctx->results_q_len = clamp_queue_len(cfg->results_q_len, 2, 8);
    ctx->cam = cfg->cam;
    ctx->cam.pixel_format = PIXFORMAT_GRAYSCALE;
    ctx->cam.frame_size = cfg->frame_size;
    if (ctx->cam.fb_count < 2) {
        ctx->cam.fb_count = 2;
    }
    ctx->cam.fb_location = CAMERA_FB_IN_PSRAM;
    ctx->cam.grab_mode = CAMERA_GRAB_LATEST;

    if (!qr_cam_framesize_dims(ctx->frame_size, &ctx->preview_width, &ctx->preview_height)) {
        ESP_LOGE(TAG, "unsupported frame size %d", (int)ctx->frame_size);
        qr_cam_cleanup(ctx);
        return false;
    }

    ctx->frames_q = xQueueCreate(ctx->frames_q_len, sizeof(qr_cam_frame_packet_t));
    ctx->results_q = xQueueCreate(ctx->results_q_len, sizeof(qr_cam_result_t));
    if (!ctx->frames_q || !ctx->results_q) {
        ESP_LOGE(TAG, "failed to create queues");
        qr_cam_cleanup(ctx);
        return false;
    }

    if (!qr_cam_alloc_preview(ctx)) {
        qr_cam_cleanup(ctx);
        return false;
    }

    esp_err_t err = esp_camera_init(&ctx->cam);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: 0x%x", (int)err);
        qr_cam_cleanup(ctx);
        return false;
    }

    ctx->camera_active = true;
    ctx->running = true;
    ctx->stop_requested = false;

    BaseType_t ok;
#if defined(CONFIG_FREERTOS_UNICORE)
    ok = xTaskCreate(qr_cam_capture_task, "qr_cam_cap", 8192, ctx, 4, &ctx->capture_task);
#else
#ifdef xTaskCreatePinnedToCore
    int cap_core = (cfg->capture_core >= 0) ? cfg->capture_core : APP_CPU_NUM;
    ok = xTaskCreatePinnedToCore(qr_cam_capture_task, "qr_cam_cap", 8192, ctx, 4, &ctx->capture_task, cap_core);
#else
    ok = xTaskCreate(qr_cam_capture_task, "qr_cam_cap", 8192, ctx, 4, &ctx->capture_task);
#endif
#endif
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create capture task");
        ctx->capture_task = NULL;
        qr_cam_stop(ctx);
        return false;
    }

    for (uint8_t i = 0; i < ctx->decode_tasks; ++i) {
#if defined(CONFIG_FREERTOS_UNICORE)
        ok = xTaskCreate(qr_cam_decoder_task, "qr_cam_dec", 16384, ctx, 5, &ctx->decoder_tasks[i]);
#else
#ifdef xTaskCreatePinnedToCore
        int dec_core = (cfg->decode_core >= 0) ? cfg->decode_core : PRO_CPU_NUM;
        ok = xTaskCreatePinnedToCore(qr_cam_decoder_task, "qr_cam_dec", 16384, ctx, 5, &ctx->decoder_tasks[i], dec_core);
#else
        ok = xTaskCreate(qr_cam_decoder_task, "qr_cam_dec", 16384, ctx, 5, &ctx->decoder_tasks[i]);
#endif
#endif
        if (ok != pdPASS) {
            ESP_LOGE(TAG, "failed to create decoder task %u", (unsigned)i);
            ctx->decoder_tasks[i] = NULL;
            qr_cam_stop(ctx);
            return false;
        }
    }

    ESP_LOGI(TAG, "qr_cam started (%ux%u grayscale, fb=%u)", ctx->preview_width, ctx->preview_height, (unsigned)ctx->cam.fb_count);
    return true;
}

void qr_cam_stop(qr_cam_ctx_t *ctx) {
    if (!ctx) {
        return;
    }
    if (!ctx->running && !ctx->capture_task) {
        qr_cam_cleanup(ctx);
        return;
    }
    ctx->stop_requested = true;
    ctx->running = false;

    while (ctx->capture_task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    for (uint8_t i = 0; i < QR_CAM_MAX_DECODERS; ++i) {
        while (ctx->decoder_tasks[i] != NULL) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }

    if (ctx->camera_active) {
        esp_camera_return_all();
        esp_camera_deinit();
        ctx->camera_active = false;
    }

    qr_cam_cleanup(ctx);
}

uint32_t qr_cam_results_available(const qr_cam_ctx_t *ctx) {
    if (!ctx || !ctx->results_q) {
        return 0;
    }
    return (uint32_t)uxQueueMessagesWaiting(ctx->results_q);
}

bool qr_cam_try_get_result(qr_cam_ctx_t *ctx, qr_cam_result_t *out) {
    if (!ctx || !ctx->results_q || !out) {
        return false;
    }
    if (xQueueReceive(ctx->results_q, out, 0) == pdTRUE) {
        return true;
    }
    return false;
}

size_t qr_cam_preview_copy(qr_cam_ctx_t *ctx, uint8_t *dst, size_t dst_len, bool mirror) {
    if (!ctx || !dst || dst_len == 0 || !ctx->preview_buf || !ctx->preview_mtx) {
        return 0;
    }
    if (dst_len < ctx->preview_len) {
        ESP_LOGW(TAG, "preview buffer too small (need %u)", (unsigned)ctx->preview_len);
        return 0;
    }
    if (xSemaphoreTake(ctx->preview_mtx, pdMS_TO_TICKS(5)) != pdTRUE) {
        return 0;
    }
    if (!ctx->preview_buf) {
        xSemaphoreGive(ctx->preview_mtx);
        return 0;
    }
    if (mirror) {
        copy_gray(dst, ctx->preview_buf, ctx->preview_width, ctx->preview_height, true);
    } else {
        memcpy(dst, ctx->preview_buf, ctx->preview_len);
    }
    xSemaphoreGive(ctx->preview_mtx);
    return ctx->preview_len;
}

bool qr_cam_preview_size(const qr_cam_ctx_t *ctx, uint16_t *out_w, uint16_t *out_h) {
    if (!ctx || ctx->preview_width == 0 || ctx->preview_height == 0) {
        return false;
    }
    if (out_w) {
        *out_w = ctx->preview_width;
    }
    if (out_h) {
        *out_h = ctx->preview_height;
    }
    return true;
}

size_t qr_cam_preview_rgb565(qr_cam_ctx_t *ctx, uint8_t *dst, size_t dst_len, bool mirror) {
    if (!ctx || !dst || dst_len == 0) {
        return 0;
    }
    if (!ctx->preview_buf || ctx->preview_len == 0 || !ctx->preview_mtx) {
        return 0;
    }
    size_t needed = ctx->preview_len * 2;
    if (dst_len < needed) {
        ESP_LOGW(TAG, "rgb565 preview buffer too small (need %u)", (unsigned)needed);
        return 0;
    }
    if (xSemaphoreTake(ctx->preview_mtx, pdMS_TO_TICKS(5)) != pdTRUE) {
        return 0;
    }
    qr_cam_init_gray_lut();
    const uint8_t *src = ctx->preview_buf;
    uint16_t *out16 = (uint16_t *)dst;
    uint16_t width = ctx->preview_width;
    uint16_t height = ctx->preview_height;
    for (uint16_t y = 0; y < height; ++y) {
        const uint8_t *row = src + (size_t)y * width;
        uint16_t *drow = out16 + (size_t)y * width;
        if (!mirror) {
            for (uint16_t x = 0; x < width; ++x) {
                drow[x] = s_gray_to_rgb565[row[x]];
            }
        } else {
            for (uint16_t x = 0; x < width; ++x) {
                drow[x] = s_gray_to_rgb565[row[width - 1 - x]];
            }
        }
    }
    xSemaphoreGive(ctx->preview_mtx);
    return needed;
}

size_t qr_cam_preview_rgb565_scaled(qr_cam_ctx_t *ctx, uint8_t *dst, int out_w, int out_h, size_t dst_len, bool mirror) {
    if (!ctx || !dst || out_w <= 0 || out_h <= 0) {
        return 0;
    }
    if (!ctx->preview_buf || ctx->preview_width == 0 || ctx->preview_height == 0 || !ctx->preview_mtx) {
        return 0;
    }
    size_t needed = (size_t)out_w * (size_t)out_h * 2;
    if (dst_len < needed) {
        ESP_LOGW(TAG, "scaled rgb565 buffer too small (need %u)", (unsigned)needed);
        return 0;
    }
    if (xSemaphoreTake(ctx->preview_mtx, pdMS_TO_TICKS(5)) != pdTRUE) {
        return 0;
    }
    qr_cam_init_gray_lut();
    const uint8_t *src = ctx->preview_buf;
    int src_w = ctx->preview_width;
    int src_h = ctx->preview_height;
    for (int y = 0; y < out_h; ++y) {
        int sample_y = (int)((int64_t)y * src_h / out_h);
        if (sample_y >= src_h) {
            sample_y = src_h - 1;
        }
        const uint8_t *srow = src + (size_t)sample_y * src_w;
        uint16_t *drow = (uint16_t *)(dst + (size_t)y * out_w * 2);
        if (!mirror) {
            for (int x = 0; x < out_w; ++x) {
                int sample_x = (int)((int64_t)x * src_w / out_w);
                if (sample_x >= src_w) {
                    sample_x = src_w - 1;
                }
                drow[x] = s_gray_to_rgb565[srow[sample_x]];
            }
        } else {
            for (int x = 0; x < out_w; ++x) {
                int src_x = out_w - 1 - x;
                int sample_x = (int)((int64_t)src_x * src_w / out_w);
                if (sample_x >= src_w) {
                    sample_x = src_w - 1;
                }
                drow[x] = s_gray_to_rgb565[srow[sample_x]];
            }
        }
    }
    xSemaphoreGive(ctx->preview_mtx);
    return needed;
}

void qr_cam_get_stats(qr_cam_ctx_t *ctx, qr_cam_stats_t *out) {
    if (!ctx || !out) {
        return;
    }
    *out = ctx->stats;
}

static void qr_cam_capture_task(void *arg) {
    qr_cam_ctx_t *ctx = (qr_cam_ctx_t *)arg;
    if (!ctx) {
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "capture task started");
    while (!ctx->stop_requested) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        if (fb->format != PIXFORMAT_GRAYSCALE) {
            esp_camera_fb_return(fb);
            continue;
        }

        if (ctx->preview_mtx && xSemaphoreTake(ctx->preview_mtx, pdMS_TO_TICKS(2)) == pdTRUE) {
            size_t expected = (size_t)ctx->preview_width * ctx->preview_height;
            size_t to_copy = ((size_t)fb->width * fb->height);
            if (expected == to_copy && ctx->preview_buf) {
                copy_gray(ctx->preview_buf, fb->buf, ctx->preview_width, ctx->preview_height, ctx->mirror);
                ctx->preview_seq++;
                ctx->stats.preview_seq = ctx->preview_seq;
            } else {
                ctx->stats.preview_drops++;
            }
            xSemaphoreGive(ctx->preview_mtx);
        } else {
            ctx->stats.preview_drops++;
        }

        uint32_t next_frame = ctx->frame_seq + 1;
        qr_cam_frame_packet_t pkt = {
            .fb = fb,
            .frame_id = next_frame,
        };

        if (xQueueSend(ctx->frames_q, &pkt, 0) != pdTRUE) {
            esp_camera_fb_return(fb);
            ctx->stats.dropped_frames++;
            taskYIELD();
            continue;
        }
        ctx->frame_seq = next_frame;
        ctx->stats.cap_frames++;
    }

    ESP_LOGI(TAG, "capture task exiting");
    ctx->capture_task = NULL;
    vTaskDelete(NULL);
}

static void qr_cam_decoder_task(void *arg) {
    qr_cam_ctx_t *ctx = (qr_cam_ctx_t *)arg;
    if (!ctx) {
        qr_cam_detach_decoder(ctx);
        vTaskDelete(NULL);
        return;
    }

    struct quirc *q = quirc_new();
    if (!q) {
        ESP_LOGE(TAG, "quirc_new failed");
        qr_cam_detach_decoder(ctx);
        vTaskDelete(NULL);
        return;
    }

    if (quirc_resize(q, ctx->preview_width, ctx->preview_height) < 0) {
        ESP_LOGE(TAG, "quirc_resize failed");
        quirc_destroy(q);
        qr_cam_detach_decoder(ctx);
        vTaskDelete(NULL);
        return;
    }

    struct quirc_code *code = heap_caps_malloc(sizeof(struct quirc_code), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    struct quirc_data *data = heap_caps_malloc(sizeof(struct quirc_data), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!code || !data) {
        ESP_LOGE(TAG, "quirc buffers alloc failed");
        if (code) {
            heap_caps_free(code);
        }
        if (data) {
            heap_caps_free(data);
        }
        quirc_destroy(q);
        qr_cam_detach_decoder(ctx);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "decoder task started");
    while (!ctx->stop_requested) {
        qr_cam_frame_packet_t pkt = {0};
        if (xQueueReceive(ctx->frames_q, &pkt, pdMS_TO_TICKS(50)) != pdTRUE) {
            continue;
        }
        camera_fb_t *fb = pkt.fb;
        uint32_t frame_id = pkt.frame_id;

        int iw, ih;
        uint8_t *qbuf = quirc_begin(q, &iw, &ih);
        if (!qbuf) {
            esp_camera_fb_return(fb);
            continue;
        }

        if (iw != ctx->preview_width || ih != ctx->preview_height) {
            quirc_end(q);
            esp_camera_fb_return(fb);
            continue;
        }

        copy_gray(qbuf, fb->buf, ctx->preview_width, ctx->preview_height, ctx->mirror);
        quirc_end(q);

        int found = quirc_count(q);
        if (found > 0) {
            ctx->stats.decoded_candidates += (uint32_t)found;
        }

        uint8_t limit = ctx->max_decode_per_frame;
        if (limit > 8) {
            limit = 8;
        }

        // Iterate detections in natural order; prioritize larger codes first
        for (int idx = 0; idx < found && idx < limit; ++idx) {
            int64_t decode_start = esp_timer_get_time();
            memset(code, 0, sizeof(*code));
            memset(data, 0, sizeof(*data));
            quirc_extract(q, idx, code);
            quirc_decode_error_t derr = quirc_decode(code, data);
            if (derr != QUIRC_SUCCESS && ctx->flip_retry) {
                quirc_flip(code);
                derr = quirc_decode(code, data);
            }
            if (derr == QUIRC_SUCCESS) {
                qr_cam_result_t res = {0};
                uint16_t n = data->payload_len;
                if (n >= QR_CAM_MAX_TEXT) {
                    n = QR_CAM_MAX_TEXT - 1;
                }
                if (n > 0) {
                    memcpy(res.text, data->payload, n);
                }
                res.text[n] = '\0';
                res.text_len = n;
                res.version = data->version;
                res.ecc_level = data->ecc_level;
                res.mask = data->mask;
                res.frame_id = frame_id;
                res.decode_ms = (uint32_t)((esp_timer_get_time() - decode_start) / 1000ULL);

                if (xQueueSend(ctx->results_q, &res, 0) != pdTRUE) {
                    ctx->stats.result_drops++;
                } else {
                    ctx->stats.decoded_ok++;
                    ctx->stats.last_error = QUIRC_SUCCESS;
                }
            } else {
                ctx->stats.decode_errors++;
                ctx->stats.last_error = (uint32_t)derr;
            }
        }

        esp_camera_fb_return(fb);
        taskYIELD();
    }

    heap_caps_free(code);
    heap_caps_free(data);
    quirc_destroy(q);
    ESP_LOGI(TAG, "decoder task exiting");
    qr_cam_detach_decoder(ctx);
    vTaskDelete(NULL);
}

