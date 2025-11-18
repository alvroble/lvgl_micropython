#include <string.h>

#include "py/obj.h"
#include "py/runtime.h"
#include "py/mperrno.h"
#include "py/mphal.h"

#include "qr_cam_core.h"

typedef struct _mp_qr_cam_obj_t {
    mp_obj_base_t base;
    qr_cam_ctx_t ctx;
    qr_cam_config_t cfg;
    bool started;
} mp_qr_cam_obj_t;

static mp_qr_cam_obj_t *s_active_scanner = NULL;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void mp_qr_cam_stop_internal(mp_qr_cam_obj_t *self) {
    if (!self) {
        return;
    }
    if (self->started) {
        qr_cam_stop(&self->ctx);
        self->started = false;
        if (s_active_scanner == self) {
            s_active_scanner = NULL;
        }
    }
}

static mp_obj_t mp_qr_cam___del__(mp_obj_t self_in) {
    mp_qr_cam_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_qr_cam_stop_internal(self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp_qr_cam___del___obj, mp_qr_cam___del__);

static mp_obj_t mp_qr_cam_dict_from_pins(void) {
    mp_obj_t dict = mp_obj_new_dict(12);
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_pwdn), MP_OBJ_NEW_SMALL_INT(MICROPY_CAMERA_PIN_PWDN));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_reset), MP_OBJ_NEW_SMALL_INT(MICROPY_CAMERA_PIN_RESET));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_xclk), MP_OBJ_NEW_SMALL_INT(MICROPY_CAMERA_PIN_XCLK));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_siod), MP_OBJ_NEW_SMALL_INT(MICROPY_CAMERA_PIN_SIOD));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_sioc), MP_OBJ_NEW_SMALL_INT(MICROPY_CAMERA_PIN_SIOC));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_d7), MP_OBJ_NEW_SMALL_INT(MICROPY_CAMERA_PIN_D7));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_d6), MP_OBJ_NEW_SMALL_INT(MICROPY_CAMERA_PIN_D6));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_d5), MP_OBJ_NEW_SMALL_INT(MICROPY_CAMERA_PIN_D5));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_d4), MP_OBJ_NEW_SMALL_INT(MICROPY_CAMERA_PIN_D4));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_d3), MP_OBJ_NEW_SMALL_INT(MICROPY_CAMERA_PIN_D3));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_d2), MP_OBJ_NEW_SMALL_INT(MICROPY_CAMERA_PIN_D2));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_d1), MP_OBJ_NEW_SMALL_INT(MICROPY_CAMERA_PIN_D1));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_d0), MP_OBJ_NEW_SMALL_INT(MICROPY_CAMERA_PIN_D0));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_vsync), MP_OBJ_NEW_SMALL_INT(MICROPY_CAMERA_PIN_VSYNC));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_href), MP_OBJ_NEW_SMALL_INT(MICROPY_CAMERA_PIN_HREF));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_pclk), MP_OBJ_NEW_SMALL_INT(MICROPY_CAMERA_PIN_PCLK));
    return dict;
}

static mp_obj_t mp_qr_cam_default_pins(void) {
    return mp_qr_cam_dict_from_pins();
}
static MP_DEFINE_CONST_FUN_OBJ_0(mp_qr_cam_default_pins_obj, mp_qr_cam_default_pins);

static void mp_qr_cam_apply_pin_overrides(mp_obj_t pins_obj, camera_config_t *cam) {
    if (!cam || pins_obj == mp_const_none) {
        return;
    }
    if (!mp_obj_is_type(pins_obj, &mp_type_dict)) {
        mp_raise_TypeError(MP_ERROR_TEXT("pins must be dict"));
    }
    mp_obj_dict_t *dict = MP_OBJ_TO_PTR(pins_obj);
    mp_map_t *map = &dict->map;
    struct {
        qstr name;
        int *target;
    } pin_fields[] = {
        { MP_QSTR_pwdn, &cam->pin_pwdn },
        { MP_QSTR_reset, &cam->pin_reset },
        { MP_QSTR_xclk, &cam->pin_xclk },
        { MP_QSTR_siod, &cam->pin_sccb_sda },
        { MP_QSTR_sioc, &cam->pin_sccb_scl },
        { MP_QSTR_d7, &cam->pin_d7 },
        { MP_QSTR_d6, &cam->pin_d6 },
        { MP_QSTR_d5, &cam->pin_d5 },
        { MP_QSTR_d4, &cam->pin_d4 },
        { MP_QSTR_d3, &cam->pin_d3 },
        { MP_QSTR_d2, &cam->pin_d2 },
        { MP_QSTR_d1, &cam->pin_d1 },
        { MP_QSTR_d0, &cam->pin_d0 },
        { MP_QSTR_vsync, &cam->pin_vsync },
        { MP_QSTR_href, &cam->pin_href },
        { MP_QSTR_pclk, &cam->pin_pclk },
    };
    size_t fields = sizeof(pin_fields) / sizeof(pin_fields[0]);
    for (size_t i = 0; i < fields; ++i) {
        mp_map_elem_t *elem = mp_map_lookup(map, MP_OBJ_NEW_QSTR(pin_fields[i].name), MP_MAP_LOOKUP);
        if (elem) {
            *pin_fields[i].target = mp_obj_get_int(elem->value);
        }
    }
}

static void mp_qr_cam_validate_pins(const camera_config_t *cam) {
    if (!cam) {
        mp_raise_ValueError(MP_ERROR_TEXT("camera config missing"));
    }
    if (cam->pin_xclk < 0 || cam->pin_sccb_sda < 0 || cam->pin_sccb_scl < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("i2c/xclk pins not configured"));
    }
    if (cam->pin_vsync < 0 || cam->pin_href < 0 || cam->pin_pclk < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("sync pins not configured"));
    }
    int data_pins[] = {
        cam->pin_d0, cam->pin_d1, cam->pin_d2, cam->pin_d3,
        cam->pin_d4, cam->pin_d5, cam->pin_d6, cam->pin_d7
    };
    for (size_t i = 0; i < MP_ARRAY_SIZE(data_pins); ++i) {
        if (data_pins[i] < 0) {
            mp_raise_ValueError(MP_ERROR_TEXT("data pins not configured"));
        }
    }
}

static bool mp_qr_cam_framesize_dims(framesize_t fs, uint16_t *w, uint16_t *h) {
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

static mp_obj_t mp_qr_cam_framesize_dims_fn(mp_obj_t fs_obj) {
    framesize_t fs = mp_obj_get_int(fs_obj);
    uint16_t w = 0, h = 0;
    if (!mp_qr_cam_framesize_dims(fs, &w, &h)) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid frame size"));
    }
    mp_obj_t items[2] = {
        MP_OBJ_NEW_SMALL_INT(w),
        MP_OBJ_NEW_SMALL_INT(h),
    };
    return mp_obj_new_tuple(2, items);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp_qr_cam_framesize_dims_obj, mp_qr_cam_framesize_dims_fn);

static mp_obj_t mp_qr_cam_result_to_dict(const qr_cam_result_t *res) {
    mp_obj_t dict = mp_obj_new_dict(6);
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_text), mp_obj_new_str(res->text, res->text_len));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_version), MP_OBJ_NEW_SMALL_INT(res->version));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_ecc_level), MP_OBJ_NEW_SMALL_INT(res->ecc_level));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_mask), MP_OBJ_NEW_SMALL_INT(res->mask));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_frame_id), MP_OBJ_NEW_SMALL_INT(res->frame_id));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_decode_ms), MP_OBJ_NEW_SMALL_INT(res->decode_ms));
    return dict;
}

// ---------------------------------------------------------------------------
// QrScanner type
// ---------------------------------------------------------------------------

enum {
    ARG_frame_size,
    ARG_fb_count,
    ARG_xclk_freq,
    ARG_decode_tasks,
    ARG_max_decode_per_frame,
    ARG_frames_q,
    ARG_results_q,
    ARG_capture_core,
    ARG_decode_core,
    ARG_mirror,
    ARG_flip_retry,
    ARG_pins,
};

static mp_obj_t mp_qr_cam_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_frame_size, MP_ARG_INT, {.u_int = FRAMESIZE_QVGA} },
        { MP_QSTR_fb_count, MP_ARG_INT, {.u_int = 3} },
        { MP_QSTR_xclk_freq, MP_ARG_INT, {.u_int = 20000000} },
        { MP_QSTR_decode_tasks, MP_ARG_INT, {.u_int = 2} },
        { MP_QSTR_max_decode_per_frame, MP_ARG_INT, {.u_int = 3} },
        { MP_QSTR_frames_q, MP_ARG_INT, {.u_int = 4} },
        { MP_QSTR_results_q, MP_ARG_INT, {.u_int = 12} },
        { MP_QSTR_capture_core, MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_decode_core, MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_mirror, MP_ARG_BOOL, {.u_bool = false} },
        { MP_QSTR_flip_retry, MP_ARG_BOOL, {.u_bool = true} },
        { MP_QSTR_pins, MP_ARG_OBJ, {.u_obj = mp_const_none} },
    };

    mp_arg_val_t parsed[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, args, MP_ARRAY_SIZE(allowed_args), allowed_args, parsed);

    mp_qr_cam_obj_t *self = mp_obj_malloc_with_finaliser(mp_qr_cam_obj_t, type);
    self->base.type = type;
    memset(&self->ctx, 0, sizeof(self->ctx));
    qr_cam_config_init_defaults(&self->cfg);
    self->started = false;

    framesize_t fs = parsed[ARG_frame_size].u_int;
    if (fs <= FRAMESIZE_96X96 || fs >= FRAMESIZE_INVALID) {
        mp_raise_ValueError(MP_ERROR_TEXT("unsupported frame size"));
    }
    self->cfg.frame_size = fs;
    self->cfg.cam.frame_size = fs;

    int fb_count = parsed[ARG_fb_count].u_int;
    if (fb_count < 2) fb_count = 2;
    if (fb_count > 4) fb_count = 4;
    self->cfg.cam.fb_count = fb_count;

    int xclk = parsed[ARG_xclk_freq].u_int;
    if (xclk < 1000000 || xclk > 40000000) {
        mp_raise_ValueError(MP_ERROR_TEXT("xclk range 1MHz-40MHz"));
    }
    self->cfg.cam.xclk_freq_hz = xclk;

    int decode_tasks = parsed[ARG_decode_tasks].u_int;
    if (decode_tasks < 1) decode_tasks = 1;
    if (decode_tasks > QR_CAM_MAX_DECODERS) decode_tasks = QR_CAM_MAX_DECODERS;
    self->cfg.decode_tasks = (uint8_t)decode_tasks;

    int max_decode = parsed[ARG_max_decode_per_frame].u_int;
    if (max_decode < 1) max_decode = 1;
    if (max_decode > 8) max_decode = 8;
    self->cfg.max_decode_per_frame = (uint8_t)max_decode;

    int frames_q = parsed[ARG_frames_q].u_int;
    if (frames_q < 2) frames_q = 2;
    if (frames_q > 32) frames_q = 32;
    self->cfg.frames_q_len = (uint8_t)frames_q;

    int results_q = parsed[ARG_results_q].u_int;
    if (results_q < 2) results_q = 2;
    if (results_q > 32) results_q = 32;
    self->cfg.results_q_len = (uint8_t)results_q;

    self->cfg.capture_core = parsed[ARG_capture_core].u_int;
    self->cfg.decode_core = parsed[ARG_decode_core].u_int;
    self->cfg.mirror = parsed[ARG_mirror].u_bool;
    self->cfg.flip_retry = parsed[ARG_flip_retry].u_bool;

    mp_qr_cam_apply_pin_overrides(parsed[ARG_pins].u_obj, &self->cfg.cam);
    mp_qr_cam_validate_pins(&self->cfg.cam);

    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t mp_qr_cam_start(mp_obj_t self_in) {
    mp_qr_cam_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->started) {
        return mp_const_none;
    }
    if (s_active_scanner && s_active_scanner != self) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("qr scanner already active"));
    }
    if (!qr_cam_start(&self->ctx, &self->cfg)) {
        mp_raise_OSError(MP_EIO);
    }
    self->started = true;
    s_active_scanner = self;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp_qr_cam_start_obj, mp_qr_cam_start);

static mp_obj_t mp_qr_cam_stop(mp_obj_t self_in) {
    mp_qr_cam_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_qr_cam_stop_internal(self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp_qr_cam_stop_obj, mp_qr_cam_stop);

static mp_obj_t mp_qr_cam_available(mp_obj_t self_in) {
    mp_qr_cam_obj_t *self = MP_OBJ_TO_PTR(self_in);
    uint32_t n = qr_cam_results_available(&self->ctx);
    return MP_OBJ_NEW_SMALL_INT((mp_int_t)n);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp_qr_cam_available_obj, mp_qr_cam_available);

static mp_obj_t mp_qr_cam_get_result(size_t n_args, const mp_obj_t *args) {
    mp_qr_cam_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    mp_int_t timeout = 0;
    if (n_args > 1) {
        timeout = mp_obj_get_int(args[1]);
    }

    qr_cam_result_t res;
    if (timeout == 0) {
        if (qr_cam_try_get_result(&self->ctx, &res)) {
            return mp_qr_cam_result_to_dict(&res);
        }
        return mp_const_none;
    }

    if (timeout < 0) {
        for (;;) {
            if (qr_cam_try_get_result(&self->ctx, &res)) {
                return mp_qr_cam_result_to_dict(&res);
            }
            mp_hal_delay_ms(5);
        }
    }

    mp_int_t remaining = timeout;
    while (remaining >= 0) {
        if (qr_cam_try_get_result(&self->ctx, &res)) {
            return mp_qr_cam_result_to_dict(&res);
        }
        if (remaining == 0) {
            break;
        }
        mp_hal_delay_ms(5);
        if (remaining >= 5) {
            remaining -= 5;
        } else {
            remaining = 0;
        }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mp_qr_cam_get_result_obj, 1, 2, mp_qr_cam_get_result);

static mp_obj_t mp_qr_cam_preview_into(size_t n_args, const mp_obj_t *args) {
    mp_qr_cam_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    mp_buffer_info_t bi;
    mp_get_buffer_raise(args[1], &bi, MP_BUFFER_WRITE);
    bool mirror = false;
    if (n_args > 2) {
        mirror = mp_obj_is_true(args[2]);
    }
    size_t copied = qr_cam_preview_copy(&self->ctx, (uint8_t *)bi.buf, bi.len, mirror);
    return MP_OBJ_NEW_SMALL_INT((mp_int_t)copied);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mp_qr_cam_preview_into_obj, 2, 3, mp_qr_cam_preview_into);

static mp_obj_t mp_qr_cam_preview_rgb565_into(size_t n_args, const mp_obj_t *args) {
    mp_qr_cam_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    mp_buffer_info_t bi;
    mp_get_buffer_raise(args[1], &bi, MP_BUFFER_WRITE);
    bool mirror = false;
    if (n_args > 2) {
        mirror = mp_obj_is_true(args[2]);
    }
    size_t copied = qr_cam_preview_rgb565(&self->ctx, (uint8_t *)bi.buf, bi.len, mirror);
    return MP_OBJ_NEW_SMALL_INT((mp_int_t)copied);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mp_qr_cam_preview_rgb565_into_obj, 2, 3, mp_qr_cam_preview_rgb565_into);

static mp_obj_t mp_qr_cam_preview_rgb565_scaled_into(size_t n_args, const mp_obj_t *args) {
    if (n_args < 4) {
        mp_raise_TypeError(MP_ERROR_TEXT("expected (buf, out_w, out_h[, mirror])"));
    }
    mp_qr_cam_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    mp_buffer_info_t bi;
    mp_get_buffer_raise(args[1], &bi, MP_BUFFER_WRITE);
    int out_w = mp_obj_get_int(args[2]);
    int out_h = mp_obj_get_int(args[3]);
    bool mirror = false;
    if (n_args > 4) {
        mirror = mp_obj_is_true(args[4]);
    }
    size_t copied = qr_cam_preview_rgb565_scaled(&self->ctx, (uint8_t *)bi.buf, out_w, out_h, bi.len, mirror);
    return MP_OBJ_NEW_SMALL_INT((mp_int_t)copied);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mp_qr_cam_preview_rgb565_scaled_into_obj, 4, 5, mp_qr_cam_preview_rgb565_scaled_into);

static mp_obj_t mp_qr_cam_preview_size(mp_obj_t self_in) {
    mp_qr_cam_obj_t *self = MP_OBJ_TO_PTR(self_in);
    uint16_t w = 0, h = 0;
    if (!qr_cam_preview_size(&self->ctx, &w, &h)) {
        mp_raise_OSError(MP_EINVAL);
    }
    mp_obj_t items[2] = {
        MP_OBJ_NEW_SMALL_INT(w),
        MP_OBJ_NEW_SMALL_INT(h),
    };
    return mp_obj_new_tuple(2, items);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp_qr_cam_preview_size_obj, mp_qr_cam_preview_size);

static mp_obj_t mp_qr_cam_stats(mp_obj_t self_in) {
    mp_qr_cam_obj_t *self = MP_OBJ_TO_PTR(self_in);
    qr_cam_stats_t st = {0};
    qr_cam_get_stats(&self->ctx, &st);
    mp_obj_t dict = mp_obj_new_dict(9);
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_cap_frames), MP_OBJ_NEW_SMALL_INT(st.cap_frames));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_decoded_ok), MP_OBJ_NEW_SMALL_INT(st.decoded_ok));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_decoded_candidates), MP_OBJ_NEW_SMALL_INT(st.decoded_candidates));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_decode_errors), MP_OBJ_NEW_SMALL_INT(st.decode_errors));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_dropped_frames), MP_OBJ_NEW_SMALL_INT(st.dropped_frames));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_preview_seq), MP_OBJ_NEW_SMALL_INT(st.preview_seq));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_preview_drops), MP_OBJ_NEW_SMALL_INT(st.preview_drops));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_result_drops), MP_OBJ_NEW_SMALL_INT(st.result_drops));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_last_error), MP_OBJ_NEW_SMALL_INT(st.last_error));
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp_qr_cam_stats_obj, mp_qr_cam_stats);

// ---------------------------------------------------------------------------
// Types and module definitions
// ---------------------------------------------------------------------------

static const mp_rom_map_elem_t qr_cam_framesize_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_QQVGA), MP_ROM_INT(FRAMESIZE_QQVGA) },
    { MP_ROM_QSTR(MP_QSTR_R240X240), MP_ROM_INT(FRAMESIZE_240X240) },
    { MP_ROM_QSTR(MP_QSTR_QVGA), MP_ROM_INT(FRAMESIZE_QVGA) },
    { MP_ROM_QSTR(MP_QSTR_R320X320), MP_ROM_INT(FRAMESIZE_320X320) },
    { MP_ROM_QSTR(MP_QSTR_VGA), MP_ROM_INT(FRAMESIZE_VGA) },
    { MP_ROM_QSTR(MP_QSTR_SVGA), MP_ROM_INT(FRAMESIZE_SVGA) },
    { MP_ROM_QSTR(MP_QSTR_XGA), MP_ROM_INT(FRAMESIZE_XGA) },
    { MP_ROM_QSTR(MP_QSTR_SXGA), MP_ROM_INT(FRAMESIZE_SXGA) },
    { MP_ROM_QSTR(MP_QSTR_HD), MP_ROM_INT(FRAMESIZE_HD) },
    { MP_ROM_QSTR(MP_QSTR_FHD), MP_ROM_INT(FRAMESIZE_FHD) },
};
static MP_DEFINE_CONST_DICT(qr_cam_framesize_locals_dict, qr_cam_framesize_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(
    qr_cam_framesize_type,
    MP_QSTR_FrameSize,
    MP_TYPE_FLAG_NONE,
    locals_dict, &qr_cam_framesize_locals_dict
    );

static const mp_rom_map_elem_t qr_cam_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_start), MP_ROM_PTR(&mp_qr_cam_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&mp_qr_cam_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&mp_qr_cam_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&mp_qr_cam___del___obj) },
    { MP_ROM_QSTR(MP_QSTR_available), MP_ROM_PTR(&mp_qr_cam_available_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_result), MP_ROM_PTR(&mp_qr_cam_get_result_obj) },
    { MP_ROM_QSTR(MP_QSTR_preview_into), MP_ROM_PTR(&mp_qr_cam_preview_into_obj) },
    { MP_ROM_QSTR(MP_QSTR_preview_rgb565_into), MP_ROM_PTR(&mp_qr_cam_preview_rgb565_into_obj) },
    { MP_ROM_QSTR(MP_QSTR_preview_rgb565_scaled_into), MP_ROM_PTR(&mp_qr_cam_preview_rgb565_scaled_into_obj) },
    { MP_ROM_QSTR(MP_QSTR_preview_size), MP_ROM_PTR(&mp_qr_cam_preview_size_obj) },
    { MP_ROM_QSTR(MP_QSTR_stats), MP_ROM_PTR(&mp_qr_cam_stats_obj) },
};
static MP_DEFINE_CONST_DICT(qr_cam_locals_dict, qr_cam_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    qr_cam_scanner_type,
    MP_QSTR_QrScanner,
    MP_TYPE_FLAG_NONE,
    make_new, mp_qr_cam_make_new,
    locals_dict, &qr_cam_locals_dict
    );

static const mp_rom_map_elem_t qr_cam_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_qr_cam) },
    { MP_ROM_QSTR(MP_QSTR_QrScanner), MP_ROM_PTR(&qr_cam_scanner_type) },
    { MP_ROM_QSTR(MP_QSTR_FrameSize), MP_ROM_PTR(&qr_cam_framesize_type) },
    { MP_ROM_QSTR(MP_QSTR_frame_size_dims), MP_ROM_PTR(&mp_qr_cam_framesize_dims_obj) },
    { MP_ROM_QSTR(MP_QSTR_default_pins), MP_ROM_PTR(&mp_qr_cam_default_pins_obj) },
};
static MP_DEFINE_CONST_DICT(qr_cam_module_globals, qr_cam_module_globals_table);

const mp_obj_module_t qr_cam_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&qr_cam_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_qr_cam, qr_cam_module);

