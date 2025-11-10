# Live camera stream to LVGL display (ST7789, RGB565)
# Recreated file: displays camera frames on screen with FPS indicator.

import lvgl as lv
import time
import machine
import task_handler
import lcd_bus
import st7789

try:
    from camera import Camera, FrameSize, PixelFormat
except ImportError as e:
    raise RuntimeError("Camera API not available; ensure camera module is built") from e

# Display configuration (same as prueba.py). If colors look wrong, prefer
# changing these DISPLAY_* flags rather than doing per-pixel conversion.
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

# Display interpretation of RGB565 data
# Try toggling these if camera colors look distorted:
#  - DISPLAY_BGR=True if red/blue are swapped
#  - DISPLAY_BYTE_SWAP=False if image looks "psychedelic" due to endianness
DISPLAY_BGR = True
DISPLAY_BYTE_SWAP = False

# Initialize LVGL
lv.init()

# SPI bus
spi_bus = machine.SPI.Bus(host=_HOST, mosi=_MOSI, miso=_MISO, sck=_SCK)
# LCD bus
display_bus = lcd_bus.SPIBus(spi_bus=spi_bus, freq=_LCD_FREQ, dc=_DC, cs=_LCD_CS)
# Display driver
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

# Enable task handler if present
th = task_handler.TaskHandler()
if hasattr(th, 'enable'):
    th.enable()

# Camera configuration
CAM_FRAME_SIZE = getattr(FrameSize, 'R240X240', FrameSize.QVGA)
CAM_PIXEL_FORMAT = PixelFormat.RGB565  # Prefer RGB565 to avoid conversion
cam = Camera(pixel_format=CAM_PIXEL_FORMAT, frame_size=CAM_FRAME_SIZE, fb_count=1)

cam_w = cam.get_pixel_width()
cam_h = cam.get_pixel_height()
cam_pf = cam.get_pixel_format()

# No per-pixel SWAP in Python to keep speed high. Use display flags above.

bpp = 2 if cam_pf == PixelFormat.RGB565 else 1
# Allocate RGB565 buffer for LVGL (convert if grayscale)
frame_buf = bytearray(cam_w * cam_h * 2)

img_dsc = lv.image_dsc_t(dict(
    header=dict(cf=lv.COLOR_FORMAT.RGB565, w=cam_w, h=cam_h),
    data_size=len(frame_buf),
    data=frame_buf,
))

img = lv.image(lv.screen_active())
img.set_src(img_dsc)
img.align(lv.ALIGN.CENTER, 0, 0)

fps_label = lv.label(lv.screen_active())
fps_label.align(lv.ALIGN.TOP_LEFT, 4, 4)
fps_label.set_style_text_color(lv.color_hex(0xFFFFFF), 0)

_last_ts = time.ticks_ms()
_frames = 0

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

def _copy_rgb565(src_mv, dst_buf):
    # Fast slice copy; adjust display config flags above if colors are wrong.
    dst_buf[:] = src_mv

def _gray8_to_rgb565(src_mv, dst_buf):
    mv = memoryview(dst_buf)
    di = 0
    for g in src_mv:
        r = g >> 3
        g6 = g >> 2
        b = g >> 3
        pixel = (r << 11) | (g6 << 5) | b
        mv[di] = (pixel >> 8) & 0xFF
        mv[di + 1] = pixel & 0xFF
        di += 2

def _on_timer_cb(_t):
    try:
        if not cam.frame_available():
            return
        frame = cam.capture()
        try:
            if cam_pf == PixelFormat.RGB565:
                _copy_rgb565(frame, frame_buf)
            elif cam_pf == PixelFormat.GRAYSCALE:
                _gray8_to_rgb565(frame, frame_buf)
            else:
                # Unsupported format for this simple viewer
                return
        finally:
            cam.free_buffer()
        img.invalidate()
        _update_fps()
    except Exception as e:
        try:
            print("camera stream error:", e)
        except Exception:
            pass

# ~30 FPS timer
lv.timer_create(_on_timer_cb, 33, None)

print("Camera stream running: %dx%d pf=%d -> RGB565" % (cam_w, cam_h, cam_pf))

try:
    while True:
        time.sleep_ms(100)
except KeyboardInterrupt:
    pass
finally:
    try:
        cam.deinit()
    except Exception:
        pass
