# Simple console test for the native qr_pipeline module.
#
# Requirements:
# - Camera built and wired; this repo's camera module should construct with defaults on supported boards.
# - The qr_pipeline native module compiled (ext_mod/qr_pipeline).
#
# Behavior:
# - Creates a camera, starts the 3-task pipeline (capture + 2 decoders),
# - Polls results non-blocking and prints decoded payloads with timing,
# - Prints a small heartbeat with throughput numbers.

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
            print("[test_qr_pipeline] set CPU freq to 240MHz")
    except Exception:
        pass

print("[test_qr_pipeline] creating camera (defaults)")
# Use defaults if your board defines MICROPY_CAMERA_* pins at build time.
# Otherwise, pass your pin mapping explicitly to Camera(...).
cam = Camera(pixel_format=PixelFormat.GRAYSCALE, frame_size=FrameSize.QVGA, fb_count=2)

w = cam.get_pixel_width()
h = cam.get_pixel_height()
print("[test_qr_pipeline] camera ready %dx%d GRAYSCALE" % (w, h))

print("[test_qr_pipeline] starting pipeline")
qr_pipeline.start(cam, mirror=True)

start_ms = time.ticks_ms()
last_ms = start_ms
count = 0
printed = 0

try:
    while True:
        # Poll all available results quickly to avoid backlog
        avail = qr_pipeline.available()
        if avail:
            while True:
                res = qr_pipeline.get_result(False)
                if not res:
                    break
                count += 1
                txt = res.get('text', '')
                ver = res.get('version')
                ecc = res.get('ecc_level')
                ms = res.get('decode_ms')
                # Print first few in full, then elide
                if printed < 10:
                    print("[QR] v%d ecc%d %dms: %s" % (ver, ecc, ms, txt))
                    printed += 1
        # Heartbeat once per second
        now = time.ticks_ms()
        if time.ticks_diff(now, last_ms) >= 1000:
            fps = count * 1000 // max(1, time.ticks_diff(now, start_ms))
            print("[stats] results=%d (~%d/sec) avail=%d" % (count, fps, avail))
            last_ms = now
        time.sleep_ms(20)
finally:
    print("[test_qr_pipeline] stopping pipeline")
    try:
        qr_pipeline.stop()
    except Exception:
        pass
    try:
        cam.deinit()
    except Exception:
        pass
