

import gc
import io
import png
import qrdecode
import time
import sys

gc.enable()

if sys.platform == 'esp32':
    import machine
    if machine.freq() != 240_000_000:
        machine.freq(240_000_000)  # 240MHz
        print("Set CPU frequency to 240MHz")
elif sys.platform == 'linux':
    img_location = "/code/tests/img/" + img_location


def run_test(quirc_max_resolution: int = 120):
    print(f"{quirc_max_resolution=}")

    if quirc_max_resolution == 240:
#        img_location = f"psbt_{quirc_max_resolution}x{quirc_max_resolution}.png"
#        img_location = f"9638917d_compact_{quirc_max_resolution}x{quirc_max_resolution}.png"
        img_location = f"psbt_{quirc_max_resolution}x{quirc_max_resolution}.png"
#        img_location = f"c0ffee_{quirc_max_resolution}x{quirc_max_resolution}.png"
    else:
        img_location = f"hola_{quirc_max_resolution}x{quirc_max_resolution}.png"

    print(f"Before `quirc.init()`: {gc.mem_free()}")
    

    def read_image_as_grayscale():
        start = time.ticks_ms()
        reader = png.Reader(filename=img_location)
        print(f"png.Reader: {time.ticks_ms() - start}ms")

        start = time.ticks_ms()
        width, height, pixels, metadata = reader.asDirect()
        print(f"{width=}, {height=}, {metadata=}")
        print(f"reader.asDirect: {time.ticks_ms() - start}ms")

        start = time.ticks_ms()
        # Build an 8-bit grayscale byte buffer of length width*height
        # If the image is already single-channel grayscale, just flatten rows.
        if metadata.get('greyscale') and not metadata.get('alpha') and metadata.get('planes') == 1:
            buf = bytearray(width * height)
            pos = 0
            for row in pixels:
                buf[pos:pos+width] = row
                pos += width
            img_bytes = bytes(buf)
        else:
            # Take the first channel from each pixel (e.g., convert RGB/RGBA/L+A to L)
            planes = metadata.get('planes', 1)
            buf = bytearray(width * height)
            pos = 0
            for row in pixels:
                idx = 0
                for x in range(width):
                    buf[pos + x] = row[idx]
                    idx += planes
                pos += width
            img_bytes = bytes(buf)
        print(f"grayscale convert: {time.ticks_ms() - start}ms")
        return img_bytes
    
    grayscale = read_image_as_grayscale()
    
    gc.collect()
    gc.threshold(gc.mem_free() // 4 + gc.mem_alloc())

    start = time.ticks_ms()
    img_data = grayscale  # already bytes
    print(f"{len(grayscale)=}")
    print(f"cast to bytes: {time.ticks_ms() - start}ms")
    
    print(f"Before freeing `grayscale`: {gc.mem_free()}")
    grayscale = None
    gc.collect()
    gc.threshold(gc.mem_free() // 4 + gc.mem_alloc())
    print(f"After freeing `grayscale`: {gc.mem_free()}")
    
    start = time.ticks_ms()
    try:
        result = qrdecode.qrdecode(img_data, quirc_max_resolution, quirc_max_resolution)
    except Exception as e:
        print(f"Error: {e}")
        result = None
    print(f"qrdecode (grayscale): {time.ticks_ms() - start}ms")
    
    return result



def qr_buf_debug(buf, w, h, rgb565=False, stride=None):
    # Compute byte length the same way mp_get_buffer_raise sees it.
    mv = memoryview(buf)
    itemsize = getattr(mv, 'itemsize', 1)  # bytes per element
    byte_len = len(mv) * itemsize          # total bytes
    bpp = 2 if rgb565 else 1
    expected = w * h * bpp
    print("type:", type(buf))
    print("itemsize:", itemsize, "elements:", len(mv), "bytes:", byte_len)
    print("expected bytes:", expected, "match:", byte_len == expected)

    # If rows are padded, you can normalize to w*h*(bpp) here:
    if stride is not None and stride != w * bpp:
        if byte_len < stride * h:
            print("warning: provided buffer smaller than stride*h")
            return None
        # Build a compact copy without row padding.
        out = bytearray(expected)
        row_sz = w * bpp
        for y in range(h):
            src_off = y * stride
            dst_off = y * row_sz
            out[dst_off:dst_off+row_sz] = mv[src_off:src_off+row_sz]
        print("made compact buffer:", len(out), "bytes")
        return bytes(out)
    return buf