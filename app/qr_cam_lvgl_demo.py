# QR Cam LVGL demo: ESP32-S3 camera capture + quirc decode pipeline
# ---------------------------------------------------------------
# - Initializes ST7789 display over SPI (RGB565)
# - Configures qr_cam module to manage esp_camera + quirc decoder tasks
# - Streams RGB565 preview onto the display (auto scaling)
# - Polls decoded QR payloads and displays them as scrolling text
#
# Adjust CAMERA_PINS below to match your module. The defaults use the board
# definition selected via MICROPY_CAMERA_MODEL_* macros.

import lvgl as lv
import time
import sys

try:
    import qr_cam
except ImportError as e:
    raise RuntimeError("qr_cam module not available; ensure qr_cam ext_mod is built") from e

# Optional: boost CPU clock for better decode throughput
if sys.platform == "esp32":
    try:
        import machine
        if machine.freq() != 240_000_000:
            machine.freq(240_000_000)
            print("[qr_cam_lvgl_demo] set CPU freq to 240MHz")
    except Exception:
        pass

# -----------------
# Display setup (ST7789 over SPI)
# -----------------
import machine
import task_handler
import lcd_bus
import st7789

_WIDTH = 240
_HEIGHT = 320
_BL = 1
_RST = -1
_DC = 42
_MOSI = 38
_MISO = 40
_SCK = 39
_HOST = 2
_LCD_CS = 45
_LCD_FREQ = 40_000_000

DISPLAY_BGR = True
DISPLAY_BYTE_SWAP = True

# Set to True if your camera module produces mirrored images.
MIRROR_IMAGE = True

# Override pin map here if the default board definition is incorrect.
CAMERA_PINS = qr_cam.default_pins()
# Example:
# CAMERA_PINS["pwdn"] = -1

FRAME_SIZE = qr_cam.FrameSize.QVGA

print("[qr_cam_lvgl_demo] starting")
lv.init()
print("[qr_cam_lvgl_demo] lvgl ready")

spi_bus = machine.SPI.Bus(host=_HOST, mosi=_MOSI, miso=_MISO, sck=_SCK)
display_bus = lcd_bus.SPIBus(spi_bus=spi_bus, freq=_LCD_FREQ, dc=_DC, cs=_LCD_CS)
display = st7789.ST7789(
    data_bus=display_bus,
    display_width=_WIDTH,
    display_height=_HEIGHT,
    backlight_pin=_BL,
    color_space=lv.COLOR_FORMAT.RGB565,
    color_byte_order=(st7789.BYTE_ORDER_BGR if DISPLAY_BGR else st7789.BYTE_ORDER_RGB),
    rgb565_byte_swap=DISPLAY_BYTE_SWAP,
)
display.set_power(True)
display.init()
display.set_backlight(80)
print("[qr_cam_lvgl_demo] display ready")

th = task_handler.TaskHandler()
if hasattr(th, "enable"):
    th.enable()

# -----------------
# qr_cam setup
# -----------------
scanner = qr_cam.QrScanner(
    frame_size=FRAME_SIZE,
    mirror=MIRROR_IMAGE,
    decode_tasks=2,
    max_decode_per_frame=4,
    frames_q=5,
    results_q=16,
    pins=CAMERA_PINS,
)
scanner.start()

cam_w, cam_h = scanner.preview_size()
print("[qr_cam_lvgl_demo] camera ready %dx%d (GRAYSCALE)" % (cam_w, cam_h))

# -----------------
# LVGL widgets
# -----------------
scr = lv.screen_active()
scr.set_style_bg_color(lv.color_hex(0x101010), 0)


def _fit_preview(src_w, src_h, max_w, max_h):
    if src_w <= 0 or src_h <= 0:
        return max_w, max_h
    if src_w * max_h > src_h * max_w:
        out_w = max_w
        out_h = (src_h * max_w) // src_w
    else:
        out_h = max_h
        out_w = (src_w * max_h) // src_h
    if out_w & 1:
        out_w -= 1
    if out_h & 1:
        out_h -= 1
    return max(2, out_w), max(2, out_h)


prev_w, prev_h = _fit_preview(cam_w, cam_h, _WIDTH, _HEIGHT)
frame_buf = bytearray(prev_w * prev_h * 2)

img_dsc = lv.image_dsc_t(
    dict(
        header=dict(cf=lv.COLOR_FORMAT.RGB565, w=prev_w, h=prev_h),
        data_size=len(frame_buf),
        data=frame_buf,
    )
)

img = lv.image(scr)
img.set_src(img_dsc)
img.align(lv.ALIGN.CENTER, 0, 0)

fps_label = lv.label(scr)
fps_label.align(lv.ALIGN.TOP_LEFT, 4, 4)
fps_label.set_style_text_color(lv.color_hex(0xFFFFFF), 0)

stats_label = lv.label(scr)
stats_label.align(lv.ALIGN.TOP_LEFT, 4, 24)
stats_label.set_style_text_color(lv.color_hex(0xB0B0B0), 0)
stats_label.set_width(_WIDTH - 8)
stats_label.set_text("cap:0 ok:0 cand:0 err:0 prevΔ:0 fbΔ:0 last:0")

qr_label = lv.label(scr)
qr_label.align(lv.ALIGN.BOTTOM_LEFT, 4, -4)
qr_label.set_style_text_color(lv.color_hex(0x00FF00), 0)
qr_label.set_width(_WIDTH - 8)
try:
    if hasattr(lv.label, "LONG_MODE") and hasattr(qr_label, "set_long_mode"):
        qr_label.set_long_mode(lv.label.LONG_MODE.SCROLL_CIRCULAR)
except Exception:
    pass

_last_ts = time.ticks_ms()
_frames = 0
_misses = 0
_last_stat_print = 0


def _update_fps():
    global _last_ts, _frames
    _frames += 1
    now = time.ticks_ms()
    dt = time.ticks_diff(now, _last_ts)
    if dt >= 1000:
        fps_label.set_text("%d FPS" % int(_frames * 1000 / dt))
        _frames = 0
        _last_ts = now


def _preview_tick(_t):
    global _misses
    copied = scanner.preview_rgb565_scaled_into(frame_buf, prev_w, prev_h)
    if not copied:
        _misses += 1
        if _misses % 90 == 0:
            print("[qr_cam_lvgl_demo] preview underrun (misses=%d)" % _misses)
        return
    _misses = 0
    img.invalidate()
    _update_fps()


def _qr_poll_tick(_t):
    global _last_stat_print
    available = scanner.available()
    if available:
        while True:
            res = scanner.get_result()
            if not res:
                break
            txt = res.get("text", "")
            ver = res.get("version")
            ecc = res.get("ecc_level")
            ms = res.get("decode_ms")
            print("[QR] v%s ecc%s %sms: %s" % (str(ver), str(ecc), str(ms), txt))
            qr_label.set_text(txt or "<empty>")

    now = time.ticks_ms()
    if time.ticks_diff(now, _last_stat_print) >= 1000:
        _last_stat_print = now
        try:
            st = scanner.stats()
            stats_label.set_text(
                "cap:{cap} ok:{ok} cand:{cand} err:{err} prevΔ:{pd} fbΔ:{fb} last:{last}".format(
                    cap=st.get("cap_frames"),
                    ok=st.get("decoded_ok"),
                    cand=st.get("decoded_candidates"),
                    err=st.get("decode_errors"),
                    pd=st.get("preview_drops"),
                    fb=st.get("dropped_frames"),
                    last=st.get("last_error"),
                )
            )
        except Exception as exc:
            print("[stats] error:", exc)


lv.timer_create(_preview_tick, 50, None)
print("[qr_cam_lvgl_demo] preview timer started")
lv.timer_create(_qr_poll_tick, 200, None)
print("[qr_cam_lvgl_demo] poll timer started")

# Pre-prime preview buffer
if scanner.preview_rgb565_scaled_into(frame_buf, prev_w, prev_h):
    img.invalidate()
    print("[qr_cam_lvgl_demo] primed first frame")
else:
    print("[qr_cam_lvgl_demo] waiting for preview data")

print(
    "qr_cam demo running: cam %dx%d GRAYSCALE; display %dx%d RGB565"
    % (cam_w, cam_h, prev_w, prev_h)
)

try:
    while True:
        time.sleep_ms(500)
        print("[idle] misses=%d" % _misses)
finally:
    try:
        scanner.stop()
    except Exception:
        pass

