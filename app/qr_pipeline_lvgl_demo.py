# QR Pipeline LVGL demo: camera preview + native qr_pipeline polling
# - Initializes ST7789 display (RGB565)
# - Configures camera in GRAYSCALE at a supported frame size
# - Starts qr_pipeline (capture + 2 decoders) to decode in background
# - Streams preview frames to display and shows decoded text
#
# Note: The preview uses direct camera capture which can reduce decoder throughput
# since the camera driver is single-producer. For best decode FPS, lower preview
# rate (timer period) or disable preview.

import lvgl as lv
import time
import sys

try:
    from camera import Camera, FrameSize, PixelFormat
except ImportError as e:
    raise RuntimeError("Camera API not available; ensure camera module is built") from e

try:
    import qr_pipeline
except ImportError as e:
    raise RuntimeError("qr_pipeline module not available; ensure it is included in build") from e

# Optional: run CPU at 240MHz on ESP32 for best throughput
if sys.platform == 'esp32':
    try:
        import machine
        if machine.freq() != 240_000_000:
            machine.freq(240_000_000)
            print("[qr_pipeline_lvgl_demo] set CPU freq to 240MHz")
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
DISPLAY_BYTE_SWAP = False

print("[qr_pipeline_lvgl_demo] starting")
lv.init()
print("[qr_pipeline_lvgl_demo] lvgl ready")

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
print("[qr_pipeline_lvgl_demo] display ready")

th = task_handler.TaskHandler()
if hasattr(th, 'enable'):
    th.enable()

# -----------------
# Camera setup and pipeline start (GRAYSCALE)
# -----------------
FS_R240 = getattr(FrameSize, 'R240X240', None)
# Prefer VGA for better decoding of dense QR codes; fall back to square 240 or QVGA
FRAME_SIZE = getattr(FrameSize, 'VGA', None) or (FS_R240 if FS_R240 is not None else FrameSize.QVGA)
cam = Camera(pixel_format=PixelFormat.GRAYSCALE, frame_size=FRAME_SIZE, fb_count=2)

cam_w = cam.get_pixel_width()
cam_h = cam.get_pixel_height()
print("[qr_pipeline_lvgl_demo] camera ready %dx%d (GRAYSCALE, fb=2)" % (cam_w, cam_h))

print("[qr_pipeline_lvgl_demo] starting qr_pipeline")
# Many ESP32 camera modules produce mirrored images; enabling mirror helps quirc decode
qr_pipeline.start(cam, mirror=True)

# -----------------
# LVGL widgets
# -----------------
scr = lv.screen_active()
scr.set_style_bg_color(lv.color_hex(0x101010), 0)

# Determine preview size to fit display while camera may be high-res (e.g., VGA)
def _fit_preview(src_w, src_h, max_w, max_h):
    # Preserve aspect ratio, fit within max_w x max_h
    if src_w <= 0 or src_h <= 0:
        return max_w, max_h
    if src_w * max_h > src_h * max_w:
        # width-limited
        out_w = max_w
        out_h = (src_h * max_w) // src_w
    else:
        # height-limited
        out_h = max_h
        out_w = (src_w * max_h) // src_h
    # Round to even to keep DMA alignment friendly
    if out_w & 1:
        out_w -= 1
    if out_h & 1:
        out_h -= 1
    return max(2, out_w), max(2, out_h)

prev_w, prev_h = _fit_preview(cam_w, cam_h, _WIDTH, _HEIGHT)

# Buffers: RGB565 framebuffer for LVGL image source at preview size
frame_buf = bytearray(prev_w * prev_h * 2)

img_dsc = lv.image_dsc_t(dict(
    header=dict(cf=lv.COLOR_FORMAT.RGB565, w=prev_w, h=prev_h),
    data_size=len(frame_buf),
    data=frame_buf,
))

img = lv.image(scr)
img.set_src(img_dsc)
img.align(lv.ALIGN.CENTER, 0, 0)

fps_label = lv.label(scr)
fps_label.align(lv.ALIGN.TOP_LEFT, 4, 4)
fps_label.set_style_text_color(lv.color_hex(0xFFFFFF), 0)

qr_label = lv.label(scr)
qr_label.align(lv.ALIGN.BOTTOM_LEFT, 4, -4)
qr_label.set_style_text_color(lv.color_hex(0x00FF00), 0)
qr_label.set_width(_WIDTH - 8)
try:
    if hasattr(lv.label, 'LONG_MODE') and hasattr(qr_label, 'set_long_mode'):
        qr_label.set_long_mode(lv.label.LONG_MODE.SCROLL_CIRCULAR)
except Exception:
    pass

# No Python LUT; use native converter for speed

_last_ts = time.ticks_ms()
_frames = 0
_misses = 0


def _update_fps():
    global _last_ts, _frames
    _frames += 1
    now = time.ticks_ms()
    dt = time.ticks_diff(now, _last_ts)
    if dt >= 1000:
        fps = int(_frames * 1000 / dt)
        fps_label.set_text("%d FPS" % fps)
        _frames = 0
        _last_ts = now


"""Preview timer: copy latest grayscale from qr_pipeline.preview_into to avoid
contending with the camera driver. Returns quickly if no preview yet."""

def _preview_tick(_t):
    global _misses
    try:
        # Fill RGB565 preview using downscaled converter for speed when camera is high-res
        n = qr_pipeline.preview_rgb565_scaled_into(frame_buf, prev_w, prev_h)
        if not n:
            _misses += 1
            if _misses % 120 == 0:  # roughly every ~6s at 50ms period
                print("[qr_pipeline_lvgl_demo] no preview data (misses=%d)" % _misses)
            return
        _misses = 0
        img.invalidate()
        _update_fps()
    except Exception as e:
        try:
            print("preview error:", e)
        except Exception:
            pass


# Poll results from qr_pipeline and update label
_last_stat_print = 0

def _qr_poll_tick(_t):
    try:
        n = qr_pipeline.available()
        if n:
            while True:
                res = qr_pipeline.get_result(False)
                if not res:
                    break
                txt = res.get('text', '')
                ver = res.get('version')
                ecc = res.get('ecc_level')
                ms = res.get('decode_ms')
                print("[QR] v%d ecc%d %dms: %s" % (ver, ecc, ms, txt))
                qr_label.set_text(txt)
        # stats once per second
        global _last_stat_print
        now = time.ticks_ms()
        if time.ticks_diff(now, _last_stat_print) >= 1000:
            _last_stat_print = now
            try:
                s = qr_pipeline.stats()
                extra = ""
                if 'decoded_candidates' in s and 'decode_errors' in s:
                    extra = " cand=%s err=%s" % (str(s.get('decoded_candidates')), str(s.get('decode_errors')))
                print("[stats] avail=%d cap=%s ok=%s prev=%s%s" % (
                    n,
                    str(s.get('cap_frames')),
                    str(s.get('decoded_ok')),
                    str(s.get('preview_seq')),
                    extra,
                ))
            except Exception:
                print("[stats] avail=%d" % n)
    except Exception as e:
        try:
            print("qr poll error:", e)
        except Exception:
            pass


# Timers: ~15 FPS preview to leave bandwidth for decoder tasks
lv.timer_create(_preview_tick, 50, None)
print("[qr_pipeline_lvgl_demo] preview timer started")
# Poll results every 200 ms
lv.timer_create(_qr_poll_tick, 200, None)
print("[qr_pipeline_lvgl_demo] poll timer started")


def _prime_first_frame():
    # Try preview buffer to fill first image
    if qr_pipeline.preview_rgb565_scaled_into(frame_buf, prev_w, prev_h):
        img.invalidate()
        print("[qr_pipeline_lvgl_demo] primed first frame from pipeline")
        return
    print("[qr_pipeline_lvgl_demo] initial preview not yet available")


_prime_first_frame()

print("qr_pipeline demo running: cam %dx%d GRAYSCALE; display RGB565" % (cam_w, cam_h))

try:
    while True:
        time.sleep_ms(500)
        # periodic idle log
        print("[idle] misses=%d" % _misses)
finally:
    try:
        qr_pipeline.stop()
    except Exception:
        pass
    try:
        cam.deinit()
    except Exception:
        pass
