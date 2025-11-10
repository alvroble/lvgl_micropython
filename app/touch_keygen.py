"""Touch-driven secp256k1 key generator for MicroPython + LVGL

Behavior:
- Collect 40 touches from the configured touch controller (CST816S if available).
- For each touch append x,y,index into a running SHA-256 digest (using uhashlib).
- After 40 touches derive a private scalar = (H mod (n-1)) + 1 (n is curve order).
- Compute the public key by scalar * G using a small pure-Python EC implementation
  for secp256k1 (affine coordinates). Display and print the results.

Notes:
- This file purposely implements the EC math locally so it works without
  precompiled secp256k1 bindings. The cryptography demonstrates how touch
  entropy can produce deterministic key material (for demo/learning only).
"""
import lvgl as lv
import time
from machine import Pin, I2C
import uhashlib

try:
    from cst816s_2 import CST816S
except Exception:
    CST816S = None

# --- Pin configuration (adjust for your board) ---
_TP_SDA = 48
_TP_SCL = 47

# Number of touches to collect
TOUCH_TARGET = 40

# secp256k1 curve parameters (constants)
p = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEFFFFFC2F
n = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141
Gx = 0x79BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798
Gy = 0x483ADA7726A3C4655DA4FBFC0E1108A8FD17B448A68554199C47D08FFB10D4B8


def modinv(a, m=p):
    """Modular inverse using Fermat's little theorem (p is prime)."""
    return pow(a, m - 2, m)


def point_add(P, Q):
    """Add two affine points P and Q on secp256k1. Use None for infinity."""
    if P is None:
        return Q
    if Q is None:
        return P
    x1, y1 = P
    x2, y2 = Q
    if x1 == x2:
        if (y1 + y2) % p == 0:
            return None
        # point doubling
        s = (3 * x1 * x1) * modinv(2 * y1) % p
    else:
        s = (y2 - y1) * modinv((x2 - x1) % p) % p
    xr = (s * s - x1 - x2) % p
    yr = (s * (x1 - xr) - y1) % p
    return xr, yr


def scalar_mult(k, P=(Gx, Gy)):
    """Multiply point P by scalar k using double-and-add."""
    if k % n == 0 or P is None:
        return None
    k = k % n
    result = None
    addend = P
    while k:
        if k & 1:
            result = point_add(result, addend)
        addend = point_add(addend, addend)
        k >>= 1
    return result


def format_pubkey_uncompressed(point):
    x, y = point
    return b"\x04" + x.to_bytes(32, "big") + y.to_bytes(32, "big")


def format_pubkey_compressed(point):
    x, y = point
    prefix = b"\x02" if (y % 2 == 0) else b"\x03"
    return prefix + x.to_bytes(32, "big")


def bytes_to_hex(b):
    """Return a hex string for bytes `b` without relying on ubinascii."""
    # Use a small generator to avoid depending on ubinascii/ubinascii.hexlify
    return ''.join('{:02x}'.format(x) for x in b)


def run():
    # Ensure LVGL is initialized (harmless if already initialized)
    try:
        lv.init()
    except Exception:
        # some builds raise if already inited; ignore
        pass

    # Follow the same pattern used in prueba.py: use lv.screen_active()
    scr = None
    if callable(getattr(lv, 'screen_active', None)):
        try:
            scr = lv.screen_active()
        except Exception:
            scr = None
    if scr is None and callable(getattr(lv, 'scr_act', None)):
        try:
            scr = lv.scr_act()
        except Exception:
            scr = None
    # If still None, try to create and load a new screen (matches prueba flow where
    # lv.init() and display init happen before creating UI)
    if scr is None:
        try:
            s = lv.obj()
            if callable(getattr(lv, 'scr_load', None)):
                try:
                    lv.scr_load(s)
                except Exception:
                    pass
            scr = s
        except Exception:
            scr = None

    if scr is None:
        raise RuntimeError('Could not obtain LVGL screen; ensure lv.init() and display initialization ran (as in prueba.py)')

    # Now build UI (use same style calls as prueba.py)
    scr.set_style_bg_color(lv.color_hex(0x101018), 0)

    title = lv.label(scr)
    title.set_text("Touch → secp256k1 Key Generator")
    title.align(lv.ALIGN.TOP_MID, 0, 6)
    title.set_style_text_color(lv.color_hex(0xE0E6F0), 0)

    info = lv.label(scr)
    info.set_text("Tap the screen 40 times anywhere to collect entropy")
    info.align(lv.ALIGN.TOP_MID, 0, 28)
    info.set_style_text_color(lv.color_hex(0xA0A8B8), 0)

    count_lbl = lv.label(scr)
    count_lbl.set_text("Touches: 0/%d" % TOUCH_TARGET)
    count_lbl.align(lv.ALIGN.TOP_LEFT, 6, 56)
    count_lbl.set_style_text_color(lv.color_hex(0xC0C8D8), 0)

    result_lbl = lv.label(scr)
    result_lbl.set_long_mode(lv.label.LONG.BREAK)
    result_lbl.set_width(220)
    result_lbl.align(lv.ALIGN.BOTTOM_MID, 0, -6)
    result_lbl.set_style_text_color(lv.color_hex(0xC0E6C8), 0)

    # Initialize touch controller if available
    touch = None
    try:
        if CST816S is not None:
            i2c = I2C(1, scl=Pin(_TP_SCL), sda=Pin(_TP_SDA), freq=100000)
            touch = CST816S(i2c)
            # try-disable auto-sleep for responsiveness
            try:
                touch.auto_sleep = False
            except Exception:
                pass
    except Exception as e:
        print("Touch init error:", e)

    if touch is None:
        if info is not None:
            try:
                info.set_text("Touch controller not found. This demo requires a touch device.")
            except Exception:
                pass
        print("No touch driver available (CST816S). Exiting.")
        return

    # SHA-256 accumulator
    h = uhashlib.sha256()
    touches = []

    print("Start touching the screen...")
    start = time.ticks_ms()
    while len(touches) < TOUCH_TARGET:
        try:
            coords = touch._get_coords()
            if coords:
                state, x, y = coords
                # Only register TOUCH DOWN events (state==1) if supported, otherwise accept all
                # Many drivers return (state, x, y) where state 1 means press.
                if state is None:
                    state = 1
                if state:
                    idx = len(touches)
                    touches.append((x, y, idx))
                    # update digest incrementally
                    h.update(x.to_bytes(2, "big"))
                    h.update(y.to_bytes(2, "big"))
                    h.update(idx.to_bytes(2, "big"))

                    # Draw a small marker
                    try:
                        dot = lv.obj(scr)
                        dot.set_size(8, 8)
                        dot.align(lv.ALIGN.TOP_LEFT, x - 4, y - 4)
                        dot.set_style_bg_color(lv.color_hex(0x70E070), 0)
                        dot.set_style_radius(4, 0)
                    except Exception as e:
                        print("[touch_keygen] Failed to draw marker:", e)
                    # Update counter label
                    try:
                        count_lbl.set_text("Touches: %d/%d" % (len(touches), TOUCH_TARGET))
                    except Exception:
                        pass
                    print("Touch %d: (%d,%d)" % (len(touches), x, y))

                    # small debounce
                    time.sleep_ms(150)
            time.sleep_ms(20)
        except Exception as e:
            print("Error reading touch:", e)
            time.sleep_ms(200)

    elapsed = time.ticks_diff(time.ticks_ms(), start)
    print("Collected %d touches in %d ms" % (len(touches), elapsed))

    digest = h.digest()
    print("SHA-256 digest:", bytes_to_hex(digest))

    hash_int = int.from_bytes(digest, "big")
    priv_int = (hash_int % (n - 1)) + 1
    priv_bytes = priv_int.to_bytes(32, "big")

    # compute public point
    pub = scalar_mult(priv_int, (Gx, Gy))
    if pub is None:
        result_lbl.set_text("Invalid public point (infinity)")
        print("Public point is infinity — unexpected")
        return

    pub_uncomp = format_pubkey_uncompressed(pub)
    pub_comp = format_pubkey_compressed(pub)

    s = []
    s.append("Private (hex): %s" % bytes_to_hex(priv_bytes))
    s.append("Pub X: %s" % hex(pub[0]))
    s.append("Pub Y: %s" % hex(pub[1]))
    s.append("Pub uncompressed: %s" % bytes_to_hex(pub_uncomp))
    s.append("Pub compressed: %s" % bytes_to_hex(pub_comp))

    result_text = "\n".join(s)
    print("\n" + result_text)
    result_lbl.set_text(result_text)


if __name__ == "__main__":
    run()
