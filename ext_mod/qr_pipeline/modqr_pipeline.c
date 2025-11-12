// MicroPython module: qr_pipeline
// A multithreaded QR decoding pipeline for ESP32-S3 using FreeRTOS and quirc
//
// Design
// - 3 tasks: capture (camera_fb_get loop), and two decoder tasks running quirc
// - Frame queue: capture pushes camera_fb_t*; decoders pop one each and return to driver after decode
// - Result queue: decoders push decoded payloads; Python can poll non-blocking
// - Camera configuration is enforced to 320x240 GRAYSCALE, grab latest
//
// MicroPython usage outline
//
// import camera
// import qr_pipeline
//
// # Create and configure camera (pins per your board)
// cam = camera.Camera(
//     data_pins=[...], pclk_pin=..., vsync_pin=..., href_pin=...,
//     sda_pin=..., scl_pin=..., xclk_pin=..., xclk_freq=20,
// )
//
// # Optional: adjust sensor settings (exposure, gain, etc.)
// cam.set_frame_size(camera.FrameSize.QVGA)
// cam.set_quality(10)
//
// # Start pipeline
// qr_pipeline.start(cam)
//
// try:
//     while True:
//         while qr_pipeline.available():
//             res = qr_pipeline.get_result()
//             # res is a dict: {'text': str, 'version': int, 'ecc_level': int, 'mask': int, 'decode_ms': int}
//             print(res)
// finally:
//     qr_pipeline.stop()

#include <string.h>
#include "py/obj.h"
#include "py/runtime.h"
#include "py/mperrno.h"
#include "py/mphal.h"

#include "qr_pipeline.h"
#include "modcamera.h"

static qr_pipeline_t s_ctx;

// start(camera_obj, frames_q_depth=4, results_q_depth=16, capture_core=-1, decode_core=-1, mirror=False)
static mp_obj_t mp_qr_start(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_camera, ARG_frames_q_depth, ARG_results_q_depth, ARG_capture_core, ARG_decode_core, ARG_mirror };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_camera, MP_ARG_OBJ | MP_ARG_REQUIRED, {.u_obj = MP_ROM_NONE} },
        { MP_QSTR_frames_q_depth, MP_ARG_INT, {.u_int = 4} },
        { MP_QSTR_results_q_depth, MP_ARG_INT, {.u_int = 16} },
        { MP_QSTR_capture_core, MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_decode_core, MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_mirror, MP_ARG_BOOL, {.u_bool = false} },
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_obj_t cam_obj = args[ARG_camera].u_obj;
    if (!mp_obj_is_obj(cam_obj)) {
        mp_raise_TypeError(MP_ERROR_TEXT("camera must be a Camera object"));
    }

    mp_camera_obj_t *camera = MP_OBJ_TO_PTR(cam_obj);

    if (!mp_camera_hal_initialized(camera)) {
        mp_camera_hal_init(camera);
    }

    bool ok = qr_pipeline_start(&s_ctx, camera,
        args[ARG_frames_q_depth].u_int,
        args[ARG_results_q_depth].u_int,
        args[ARG_capture_core].u_int,
        args[ARG_decode_core].u_int,
        args[ARG_mirror].u_bool);

    if (!ok) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("failed to start qr pipeline"));
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(mp_qr_start_obj, 1, mp_qr_start);

// stop()
static mp_obj_t mp_qr_stop(void) {
    qr_pipeline_stop(&s_ctx);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(mp_qr_stop_obj, mp_qr_stop);

// available() -> int
static mp_obj_t mp_qr_available(void) {
    return MP_OBJ_NEW_SMALL_INT((mp_int_t)qr_pipeline_results_available(&s_ctx));
}
static MP_DEFINE_CONST_FUN_OBJ_0(mp_qr_available_obj, mp_qr_available);

// get_result(block=False) -> dict | None
static mp_obj_t mp_qr_get_result(size_t n_args, const mp_obj_t *args) {
    bool block = false;
    if (n_args > 0) {
        block = mp_obj_is_true(args[0]);
    }

    qr_result_t res;
    if (!block) {
        if (qr_pipeline_try_get_result(&s_ctx, &res)) {
            mp_obj_t dict = mp_obj_new_dict(5);
            mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_text), mp_obj_new_str(res.text, res.text_len));
            mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_version), MP_OBJ_NEW_SMALL_INT(res.version));
            mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_ecc_level), MP_OBJ_NEW_SMALL_INT(res.ecc_level));
            mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_mask), MP_OBJ_NEW_SMALL_INT(res.mask));
            mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_decode_ms), MP_OBJ_NEW_SMALL_INT(res.decode_ms));
            return dict;
        }
        return mp_const_none;
    }

    // Blocking wait
    for (;;) {
        if (qr_pipeline_try_get_result(&s_ctx, &res)) {
            mp_obj_t dict = mp_obj_new_dict(5);
            mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_text), mp_obj_new_str(res.text, res.text_len));
            mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_version), MP_OBJ_NEW_SMALL_INT(res.version));
            mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_ecc_level), MP_OBJ_NEW_SMALL_INT(res.ecc_level));
            mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_mask), MP_OBJ_NEW_SMALL_INT(res.mask));
            mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_decode_ms), MP_OBJ_NEW_SMALL_INT(res.decode_ms));
            return dict;
        }
        // Yield MicroPython VM thread for a short time
        mp_hal_delay_ms(5);
    }
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mp_qr_get_result_obj, 0, 1, mp_qr_get_result);

// preview_into(buf) -> int bytes copied or 0 if not ready
static mp_obj_t mp_qr_preview_into(mp_obj_t buf_in) {
    mp_buffer_info_t bi;
    mp_get_buffer_raise(buf_in, &bi, MP_BUFFER_WRITE);
    size_t copied = qr_pipeline_preview_copy(&s_ctx, (uint8_t *)bi.buf, (size_t)bi.len);
    return MP_OBJ_NEW_SMALL_INT((mp_int_t)copied);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp_qr_preview_into_obj, mp_qr_preview_into);

// preview_rgb565_into(buf) -> int bytes copied or 0 if not ready
static mp_obj_t mp_qr_preview_rgb565_into(mp_obj_t buf_in) {
    mp_buffer_info_t bi;
    mp_get_buffer_raise(buf_in, &bi, MP_BUFFER_WRITE);
    size_t copied = qr_pipeline_preview_rgb565_into(&s_ctx, (uint8_t *)bi.buf, (size_t)bi.len);
    return MP_OBJ_NEW_SMALL_INT((mp_int_t)copied);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp_qr_preview_rgb565_into_obj, mp_qr_preview_rgb565_into);

// preview_rgb565_scaled_into(buf, out_w, out_h) -> int bytes copied or 0 if not ready
static mp_obj_t mp_qr_preview_rgb565_scaled_into(size_t n_args, const mp_obj_t *args) {
    if (n_args != 3) {
        mp_raise_TypeError(MP_ERROR_TEXT("expected (buf, out_w, out_h)"));
    }
    mp_buffer_info_t bi;
    mp_get_buffer_raise(args[0], &bi, MP_BUFFER_WRITE);
    int out_w = mp_obj_get_int(args[1]);
    int out_h = mp_obj_get_int(args[2]);
    size_t copied = qr_pipeline_preview_rgb565_scaled_into(&s_ctx, (uint8_t *)bi.buf, out_w, out_h, (size_t)bi.len);
    return MP_OBJ_NEW_SMALL_INT((mp_int_t)copied);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mp_qr_preview_rgb565_scaled_into_obj, 3, 3, mp_qr_preview_rgb565_scaled_into);

// stats() -> dict
static mp_obj_t mp_qr_stats(void) {
    qr_stats_t st = (qr_stats_t){0};
    qr_pipeline_get_stats(&s_ctx, &st);
    mp_obj_t d = mp_obj_new_dict(5);
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_cap_frames), MP_OBJ_NEW_SMALL_INT((mp_int_t)st.cap_frames));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_decoded_ok), MP_OBJ_NEW_SMALL_INT((mp_int_t)st.decoded_ok));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_preview_seq), MP_OBJ_NEW_SMALL_INT((mp_int_t)st.preview_seq));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_decoded_candidates), MP_OBJ_NEW_SMALL_INT((mp_int_t)st.decoded_candidates));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_decode_errors), MP_OBJ_NEW_SMALL_INT((mp_int_t)st.decode_errors));
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_0(mp_qr_stats_obj, mp_qr_stats);

// Module globals
static const mp_rom_map_elem_t qr_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_qr_pipeline) },
    { MP_ROM_QSTR(MP_QSTR_start), MP_ROM_PTR(&mp_qr_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&mp_qr_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_available), MP_ROM_PTR(&mp_qr_available_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_result), MP_ROM_PTR(&mp_qr_get_result_obj) },
    { MP_ROM_QSTR(MP_QSTR_preview_into), MP_ROM_PTR(&mp_qr_preview_into_obj) },
    { MP_ROM_QSTR(MP_QSTR_preview_rgb565_into), MP_ROM_PTR(&mp_qr_preview_rgb565_into_obj) },
    { MP_ROM_QSTR(MP_QSTR_preview_rgb565_scaled_into), MP_ROM_PTR(&mp_qr_preview_rgb565_scaled_into_obj) },
    { MP_ROM_QSTR(MP_QSTR_stats), MP_ROM_PTR(&mp_qr_stats_obj) },
};
static MP_DEFINE_CONST_DICT(qr_globals, qr_globals_table);

const mp_obj_module_t qr_pipeline_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&qr_globals,
};

MP_REGISTER_MODULE(MP_QSTR_qr_pipeline, qr_pipeline_module);
