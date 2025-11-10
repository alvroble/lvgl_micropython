# Fast QR code scanning using camera + quirc with LVGL preview.

import lvgl as lv
import time
import machine
import lcd_bus
import st7789
import qrdecode
import task_handler

try:
    from camera import Camera, FrameSize, PixelFormat, GrabMode
except ImportError as e:
    raise RuntimeError("Camera API not available") from e

# Display config (same pins as your other scripts)
_WIDTH = 240
_HEIGHT = 320
_BL = 1
_DC = 42
_MOSI = 38
_MISO = 40
_SCK = 39
_HOST = 2
_LCD_CS = 45
_LCD_FREQ = 40_000_000

# Init LVGL + Display
lv.init()
spi_bus = machine.SPI.Bus(host=_HOST, mosi=_MOSI, miso=_MISO, sck=_SCK)
display_bus = lcd_bus.SPIBus(spi_bus=spi_bus, freq=_LCD_FREQ, dc=_DC, cs=_LCD_CS)
display = st7789.ST7789(
    data_bus=display_bus,
    display_width=_WIDTH,
    display_height=_HEIGHT,
    backlight_pin=_BL,
    color_space=lv.COLOR_FORMAT.RGB565,
    color_byte_order=st7789.BYTE_ORDER_BGR,   # Known good
    rgb565_byte_swap=False,
)
display.set_power(True)
display.init()
display.set_backlight(80)

# Camera target config
PREFERRED_SIZES = [getattr(FrameSize, 'R240X240', FrameSize.QVGA), FrameSize.QVGA, FrameSize.VGA]
# Try GRAYSCALE first (ideal for quirc); fallback to RGB565
PREFERRED_FORMATS = [PixelFormat.GRAYSCALE, PixelFormat.RGB565]

cam = None
for pf in PREFERRED_FORMATS:
    for fs in PREFERRED_SIZES:
        try:
            cam = Camera(pixel_format=pf, frame_size=fs, fb_count=2, grab_mode=GrabMode.LATEST)
            break
        except Exception:
            cam = None
    if cam:
        break
if cam is None:
    raise RuntimeError("Unable to init camera")

cam_w = cam.get_pixel_width()
cam_h = cam.get_pixel_height()
cam_pf = cam.get_pixel_format()
print("Camera: %dx%d pf=%d" % (cam_w, cam_h, cam_pf))

# Allocate preview buffer (always RGB565 for LVGL)
rgb_buf = bytearray(cam_w * cam_h * 2)

img_dsc = lv.image_dsc_t(dict(
    header=dict(cf=lv.COLOR_FORMAT.RGB565, w=cam_w, h=cam_h),
    data_size=len(rgb_buf),
    data=rgb_buf,
))
img = lv.image(lv.screen_active())
img.set_src(img_dsc)
img.align(lv.ALIGN.CENTER, 0, 0)

label = lv.label(lv.screen_active())
label.align(lv.ALIGN.TOP_LEFT, 4, 4)
label.set_style_text_color(lv.color_hex(0xFFFFFF), 0)
label.set_text("Init %dx%d pf=%d" % (cam_w, cam_h, cam_pf))

# Optional center crop (speeds decode if large frames)
ENABLE_CROP = False
CROP_RATIO = 0.75  # take 75% of width/height
if ENABLE_CROP:
    crop_w = int(cam_w * CROP_RATIO)
    crop_h = int(cam_h * CROP_RATIO)
    crop_x0 = (cam_w - crop_w) // 2
    crop_y0 = (cam_h - crop_h) // 2
else:
    crop_w = cam_w
    crop_h = cam_h
    crop_x0 = 0
    crop_y0 = 0

# Preallocate decode grayscale buffer (only if needed)
if cam_pf == PixelFormat.GRAYSCALE:
    decode_gray = None  # we will use captured frame directly
else:
    decode_gray = bytearray(crop_w * crop_h)

# Fast RGB565 <- GRAY (LUT)
_GRAY2RGB565 = bytearray(512)
for g in range(256):
    r = g >> 3
    g6 = g >> 2
    b = g >> 3
    p = (r << 11) | (g6 << 5) | b
    i = g * 2
    _GRAY2RGB565[i] = (p >> 8) & 0xFF
    _GRAY2RGB565[i + 1] = p & 0xFF

def gray_to_rgb565(src, dst):
    lut = _GRAY2RGB565
    mv_dst = memoryview(dst)
    di = 0
    for g in src:
        gi = g * 2
        mv_dst[di] = lut[gi]
        mv_dst[di + 1] = lut[gi + 1]
        di += 2

def rgb565_to_gray(src_mv, dst_mv, w, h, sx=0, sy=0):
    # Convert cropped region (sx,sy) of RGB565 frame to grayscale
    row_bytes = cam_w * 2
    di = 0
    for y in range(sy, sy + h):
        off = y * row_bytes + sx * 2
        line = src_mv[off:off + w * 2]
        for i in range(0, len(line), 2):
            # 5-6-5 bits
            b1 = line[i]
            b2 = line[i + 1]
            r = (b1 & 0xF8)      # 5 bits at top
            g = ((b1 & 0x07) << 5) | ((b2 & 0xE0) >> 3)  # spread 6 bits
            b = (b2 & 0x1F) << 3
            # Approx luminance (integer weights ~0.299,0.587,0.114)
            dst_mv[di] = (r * 25 + g * 129 + b * 13) >> 8
            di += 1

frames = 0
last_fps_ts = time.ticks_ms()
last_decode_ts = 0
DECODE_INTERVAL_MS = 200
FOUND = False

# Task handler (LVGL)
try:
    th = task_handler.TaskHandler()
    if hasattr(th, 'enable'):
        th.enable()
except Exception:
    pass

def update_fps():
    global frames, last_fps_ts
    now = time.ticks_ms()
    frames += 1
    if time.ticks_diff(now, last_fps_ts) >= 1000:
        label.set_text(("Found" if FOUND else "Scan") + " %d FPS" % frames)
        frames = 0
        last_fps_ts = now

print("Starting loop (crop=%s %dx%d)" % (ENABLE_CROP, crop_w, crop_h))

try:
    while True:
        if not cam.frame_available():
            time.sleep_ms(5)
            continue
        frame = cam.capture()
        try:
            # Preview
            if cam_pf == PixelFormat.GRAYSCALE:
                gray_to_rgb565(frame, rgb_buf)
                decode_src = frame  # direct
            else:
                # RGB565 direct copy
                mv_dst = memoryview(rgb_buf)
                mv_dst[:len(frame)] = frame
                decode_src = None  # will derive later only if decoding
            img.invalidate()

            # Decode periodically
            now = time.ticks_ms()
            if (not FOUND) and time.ticks_diff(now, last_decode_ts) >= DECODE_INTERVAL_MS:
                last_decode_ts = now

                if cam_pf == PixelFormat.GRAYSCALE:
                    if ENABLE_CROP:
                        # Crop view for decode
                        # (quirc expects full width*height buffer; provide cropped if you pass crop dims)
                        # Here we decode only cropped region:
                        # Extract crop into a contiguous buffer (avoid big alloc by reuse)
                        # Minimal copy:
                        crop_buf = bytearray(crop_w * crop_h)
                        mv_frame = memoryview(frame)
                        row_src_len = cam_w
                        pos = 0
                        for y in range(crop_y0, crop_y0 + crop_h):
                            start = y * row_src_len + crop_x0
                            end = start + crop_w
                            crop_buf[pos:pos + crop_w] = mv_frame[start:end]
                            pos += crop_w
                        try:
                            res = qrdecode.qrdecode(crop_buf, crop_w, crop_h)
                        except Exception:
                            res = None
                    else:
                        try:
                            res = qrdecode.qrdecode(decode_src, cam_w, cam_h)
                        except Exception:
                            res = None
                else:
                    # Need grayscale for decode
                    if decode_gray:
                        mv_gray = memoryview(decode_gray)
                        rgb565_to_gray(memoryview(frame), mv_gray,
                                       crop_w, crop_h, crop_x0, crop_y0)
                        try:
                            res = qrdecode.qrdecode(decode_gray, crop_w, crop_h)
                        except Exception:
                            res = None
                    else:
                        res = None

                if res:
                    FOUND = True
                    print("QR:", res)
                    label.set_text("QR OK")
            update_fps()
        finally:
            cam.free_buffer()

        if FOUND:
            time.sleep_ms(50)
except KeyboardInterrupt:
    pass
finally:
    try:
        cam.deinit()
    except Exception:
        pass
    print("Exit QR scan")