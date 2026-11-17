#!/usr/bin/env python3
"""PiBody screen simulator: runs the real _bleboard.py (and pibody Display
code) on the host against a fake st7789 driver and writes PNGs.

    python3 boards/PiBody/tools/screensim.py [out_dir] [--libs ~/Tools/pibody_libs]

Writes one PNG per scenario plus sheet.png (all frames side by side, labelled).
No dependencies beyond the standard library. The fake driver follows the C
driver where it matters for looks: VGA bitmap fonts with a solid background
box, the fill_circle midpoint algorithm, the scanline polygon fill, and the
overlay clip that keeps drawing out of the bar rows.
"""
import math
import os
import struct
import sys
import types
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
MODULES = os.path.join(HERE, "..", "modules")
W, H = 240, 320


# --- fake st7789 ---------------------------------------------------------------

class _Clip:
    top = 0
    dot = None
    bypass = False
    dirty = False


def smooth_layout(font, s):
    """Like st7789.c smooth_text(): [(glyph, x)], left, right, pen."""
    chars = font.CHARS
    q = chars.find("?")
    pen = left = right = 0
    out = []
    for ch in s:
        i = chars.find(ch)
        if i < 0:
            i = q
        g = pen + font.XOFF[i] - 128
        if font.GW[i]:
            out.append((i, g))
            left, right = min(left, g), max(right, g + font.GW[i])
        pen += font.ADV[i]
    return out, left, max(right, pen), pen


def smooth_alpha(font, s):
    """Alpha (0..15) rows of a line of text, and its left offset and advance."""
    glyphs, left, right, pen = smooth_layout(font, s)
    a = [[0] * (right - left) for _ in range(font.HEIGHT)]
    for i, gx in glyphs:
        off = font.OFFS[2 * i] | font.OFFS[2 * i + 1] << 8
        w = font.GW[i]
        for r in range(font.GH[i]):
            for c in range(w):
                bit = r * w + c
                b = font.BITMAP[off + (bit >> 1)]
                v = b & 15 if bit & 1 else b >> 4
                row = a[font.YOFF[i] + r]
                row[gx - left + c] = max(row[gx - left + c], v)
    return a, left, pen


def _make_st7789():
    m = types.ModuleType("st7789")
    clip = _Clip()
    fb = [[0] * W for _ in range(H)]
    m._fb = fb
    m._clip = clip

    def put(x, y, c):
        if not (0 <= x < W and 0 <= y < H):
            return
        if not clip.bypass:
            if y < clip.top:
                return
            d = clip.dot
            if d and d[0] <= x < d[0] + d[2] and d[1] <= y < d[1] + d[3]:
                clip.dirty = True
        fb[y][x] = c & 0xFFFF

    def color(r, g, b):
        return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)

    class ST7789:
        BLACK, BLUE, RED, GREEN = 0x0000, 0x001F, 0xF800, 0x07E0
        CYAN, MAGENTA, YELLOW, WHITE = 0x07FF, 0xF81F, 0xFFE0, 0xFFFF

        def __init__(self, *a, **k):
            self._w, self._h = W, H

        def init(self):
            clip.dirty = True

        def hard_reset(self):
            clip.dirty = True

        soft_reset = hard_reset

        def width(self):
            return W

        def height(self):
            return H

        def pixel(self, x, y, c):
            put(x, y, c)

        def fill_rect(self, x, y, w, h, c):
            for yy in range(y, y + h):
                for xx in range(x, x + w):
                    put(xx, yy, c)

        def fill(self, c):
            self.fill_rect(0, 0, W, H, c)

        def hline(self, x, y, w, c):
            self.fill_rect(x, y, w, 1, c)

        def vline(self, x, y, h, c):
            self.fill_rect(x, y, 1, h, c)

        def rect(self, x, y, w, h, c):
            self.hline(x, y, w, c)
            self.hline(x, y + h - 1, w, c)
            self.vline(x, y, h, c)
            self.vline(x + w - 1, y, h, c)

        def line(self, x0, y0, x1, y1, c):
            dx, dy = abs(x1 - x0), -abs(y1 - y0)
            sx, sy = (1 if x0 < x1 else -1), (1 if y0 < y1 else -1)
            err = dx + dy
            while True:
                put(x0, y0, c)
                if x0 == x1 and y0 == y1:
                    break
                e2 = 2 * err
                if e2 >= dy:
                    err += dy
                    x0 += sx
                if e2 <= dx:
                    err += dx
                    y0 += sy

        def fill_circle(self, xm, ym, r, c):
            f, ddx, ddy, x, y = 1 - r, 1, -2 * r, 0, r
            self.vline(xm, ym - y, 2 * y + 1, c)
            while x < y:
                if f >= 0:
                    y -= 1
                    ddy += 2
                    f += ddy
                x += 1
                ddx += 2
                f += ddx
                self.vline(xm + x, ym - y, 2 * y + 1, c)
                self.vline(xm + y, ym - x, 2 * x + 1, c)
                self.vline(xm - x, ym - y, 2 * y + 1, c)
                self.vline(xm - y, ym - x, 2 * x + 1, c)

        def circle(self, xm, ym, r, c):
            f, ddx, ddy, x, y = 1 - r, 1, -2 * r, 0, r
            for px, py in ((0, r), (0, -r), (r, 0), (-r, 0)):
                put(xm + px, ym + py, c)
            while x < y:
                if f >= 0:
                    y -= 1
                    ddy += 2
                    f += ddy
                x += 1
                ddx += 2
                f += ddx
                for px, py in ((x, y), (-x, y), (x, -y), (-x, -y),
                               (y, x), (-y, x), (y, -x), (-y, -x)):
                    put(xm + px, ym + py, c)

        def fill_polygon(self, pts, x, y, c, angle=0, cx=0, cy=0):
            pts = [(int(p[0]), int(p[1])) for p in pts]
            if angle:
                ca, sa = math.cos(angle), math.sin(angle)
                pts = [(int(cx + (px - cx) * ca - (py - cy) * sa),
                        int(cy + (px - cx) * sa + (py - cy) * ca)) for px, py in pts]
            ys = [p[1] for p in pts]
            n = len(pts)
            for py in range(min(ys), max(ys)):
                nodes = []
                j = n - 1
                for i in range(n):
                    (xi, yi), (xj, yj) = pts[i], pts[j]
                    if (yi < py <= yj) or (yj < py <= yi):
                        nodes.append(int(xi + (py - yi) / (yj - yi) * (xj - xi)))
                    j = i
                nodes.sort()
                for k in range(0, len(nodes) - 1, 2):
                    for px in range(nodes[k], nodes[k + 1] + 1):
                        put(px + x, py + y, c)

        def fill_shapes(self, bx, by, bw, bh, bg, shapes):
            """Same coverage as st7789.c fill_shapes (4 sub-rows, span overlap)."""
            def blend(b, f, a):
                out = 0
                for sh, m in ((11, 31), (5, 63), (0, 31)):
                    cb, cf = b >> sh & m, f >> sh & m
                    out |= (cb + (((cf - cb) * a + 128) >> 8)) << sh
                return out
            bx0, by0 = max(0, bx), max(0, by)
            bx1, by1 = min(W, bx + bw), min(H, by + bh)
            w = bx1 - bx0
            box = [[bg] * w for _ in range(by1 - by0)]
            for pts, color in shapes:
                n = len(pts)
                for y in range(by0, by1):
                    cov = [0] * w
                    for sub in range(4):
                        fy = y + (sub + 0.5) / 4
                        xs = []
                        for i in range(n):
                            (x0, y0), (x1, y1) = pts[i], pts[(i + 1) % n]
                            if (y0 <= fy < y1) or (y1 <= fy < y0):
                                xs.append(x0 + (fy - y0) * (x1 - x0) / (y1 - y0))
                        xs.sort()
                        for i in range(0, len(xs) - 1, 2):
                            xa, xb = max(0, xs[i] - bx0), min(w, xs[i + 1] - bx0)
                            if xb <= xa:
                                continue
                            pa, pb = int(xa), int(xb)
                            if pa == pb:
                                cov[pa] += int((xb - xa) * 64 + 0.5)
                                continue
                            cov[pa] += int((pa + 1 - xa) * 64 + 0.5)
                            for q in range(pa + 1, pb):
                                cov[q] += 64
                            if pb < w:
                                cov[pb] += int((xb - pb) * 64 + 0.5)
                    row = box[y - by0]
                    for q in range(w):
                        c = min(256, cov[q])
                        if c >= 255:
                            row[q] = color
                        elif c:
                            row[q] = blend(row[q], color, c)
            for r, row in enumerate(box):
                for q, c in enumerate(row):
                    put(bx0 + q, by0 + r, c)

        def blit_framebuf(self, buf, x, y, w, h):
            buf = bytes(buf)
            for i in range(min(len(buf) // 2, w * h)):
                put(x + i % w, y + i // w, buf[2 * i] | buf[2 * i + 1] << 8)

        def clear(self):
            self.fill(0)

        def vscrdef(self, *a):
            pass

        def vscsad(self, a):
            pass  # hardware scrolling is not simulated

        def blit_buffer(self, buf, x, y, w, h):
            buf = bytes(buf)
            for i in range(min(len(buf) // 2, w * h)):
                put(x + i % w, y + i // w, buf[2 * i] << 8 | buf[2 * i + 1])

        def text(self, font, s, x0, y0, fg=0xFFFF, bg=0x0000):
            if isinstance(s, int):
                s = chr(s)
            if hasattr(font, "BITMAP"):
                a, left, pen = smooth_alpha(font, s)
                fr, fgg, fb = fg >> 11, fg >> 5 & 63, fg & 31
                br, bgg, bb = bg >> 11, bg >> 5 & 63, bg & 31
                lut = [(br + (fr - br) * k // 15) << 11 | (bgg + (fgg - bgg) * k // 15) << 5
                       | (bb + (fb - bb) * k // 15) for k in range(16)]
                for r, row in enumerate(a):
                    for c, v in enumerate(row):
                        put(x0 + left + c, y0 + r, lut[v])
                return x0 + pen
            w, h = font.WIDTH, font.HEIGHT
            data = font.FONT if hasattr(font, "FONT") else font._FONT
            per = round(h * (w // 8 + (w % 8) / 8.0))
            if isinstance(s, int):
                s = chr(s)
            for ch in s:
                code = ord(ch)
                if 0x401 <= code <= 0x4E9 and code > font.LAST:
                    code = code - 0x401 + font.LAST + 1
                if code < font.FIRST:
                    continue
                base = (code - font.FIRST) * per
                if x0 + w - 1 < W and base + per <= len(data):
                    for i in range(w * h):
                        b = data[base + i // 8] >> (7 - i % 8) & 1
                        put(x0 + i % w, y0 + i // w, fg if b else bg)
                x0 += w

    m.ST7789 = ST7789
    m.color = color
    for k in ("BLACK", "BLUE", "RED", "GREEN", "CYAN", "MAGENTA", "YELLOW", "WHITE"):
        setattr(m, k, getattr(ST7789, k))

    m.spi_clock = lambda: 125_000_000

    def _overlay(top, dot=None):
        clip.top = top
        clip.dot = tuple(dot) if dot else None
        clip.dirty = False

    def _overlay_bypass(on):
        clip.bypass = bool(on)

    def _overlay_dirty():
        d = clip.dirty
        clip.dirty = False
        return d

    m._overlay = _overlay
    m._overlay_bypass = _overlay_bypass
    m._overlay_dirty = _overlay_dirty
    return m


def _make_framebuf():
    """Enough of framebuf for DisplayBase.Canvas: RGB565 fill, pixel, fill_rect."""
    m = types.ModuleType("framebuf")
    m.RGB565 = 1

    class FrameBuffer:
        def __init__(self, buf, w, h, fmt):
            self._b, self._w, self._h = buf, w, h

        def pixel(self, x, y, c=None):
            if 0 <= x < self._w and 0 <= y < self._h:
                i = 2 * (y * self._w + x)
                if c is None:
                    return self._b[i] | self._b[i + 1] << 8
                self._b[i], self._b[i + 1] = c & 0xFF, c >> 8 & 0xFF

        def fill_rect(self, x, y, w, h, c):
            for yy in range(y, y + h):
                for xx in range(x, x + w):
                    self.pixel(xx, yy, c)

        def fill(self, c):
            self.fill_rect(0, 0, self._w, self._h, c)

        def rect(self, x, y, w, h, c, f=False):
            if f:
                return self.fill_rect(x, y, w, h, c)
            self.fill_rect(x, y, w, 1, c)
            self.fill_rect(x, y + h - 1, w, 1, c)
            self.fill_rect(x, y, 1, h, c)
            self.fill_rect(x + w - 1, y, 1, h, c)

    m.FrameBuffer = FrameBuffer
    return m


def _make_micropython():
    """viper on the host: ptr16 arguments become 16-bit views."""
    import builtins
    m = types.ModuleType("micropython")

    class ptr8: pass
    class ptr16: pass
    class ptr32: pass
    builtins.ptr8, builtins.ptr16, builtins.ptr32 = ptr8, ptr16, ptr32

    def viper(f):
        import inspect
        names = list(inspect.signature(f).parameters)
        wide = [i for i, n in enumerate(names) if f.__annotations__.get(n) is ptr16]

        def run(*a):
            a = list(a)
            for i in wide:
                a[i] = memoryview(a[i]).cast("B").cast("H")
            return f(*a)
        return run

    m.viper = viper
    m.native = lambda f: f
    m.const = lambda x: x
    return m


def _make_machine():
    m = types.ModuleType("machine")

    class Pin:
        OUT, IN = 1, 0

        def __init__(self, *a, **k):
            pass

    class SPI:
        def __init__(self, *a, **k):
            pass

    class Timer:
        last = None

        def __init__(self, *a, period=0, callback=None, **k):
            Timer.last = self
            self.callback = callback

        def deinit(self):
            pass

    m.Pin, m.SPI, m.Timer = Pin, SPI, Timer
    m.__getattr__ = lambda name: SPI  # I2C, SoftI2C, ADC, ...: inert stubs
    return m


def _make_bleterm():
    m = types.ModuleType("bleterm")
    m.state = 0x01 | 0x08 | 0x10  # started, pairing open, idle
    m._bar_state = lambda: m.state
    m.status = lambda: {"name": "PiBody-EEE6"}
    return m


def _make_network():
    """Wi-Fi flags only (what the bar reads): WLAN(i).active(), .status()."""
    m = types.ModuleType("network")
    m.STA_IF, m.AP_IF = 0, 1
    m.flags = {0: [False, 0], 1: [False, 0]}  # itf: [active, status]

    class WLAN:
        def __init__(self, itf=0):
            self.itf = itf

        def active(self, *a):
            if a:
                m.flags[self.itf][0] = bool(a[0])
            return m.flags[self.itf][0]

        def status(self, *a):
            return m.flags[self.itf][1]

    m.WLAN = WLAN
    return m


# --- PNG ------------------------------------------------------------------------

def _rgb(c):
    r, g, b = (c >> 11) & 0x1F, (c >> 5) & 0x3F, c & 0x1F
    return (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)


def write_png(path, rows, scale=1):
    h, w = len(rows), len(rows[0])
    raw = bytearray()
    for row in rows:
        line = bytearray()
        for c in row:
            line += bytes(_rgb(c) if isinstance(c, int) else c) * scale
        for _ in range(scale):
            raw += b"\x00" + line

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w * scale, h * scale, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 6)))
        f.write(chunk(b"IEND", b""))


# --- session --------------------------------------------------------------------

class Board:
    """Fresh fake board with _bleboard loaded."""

    def __init__(self, libs=None):
        for k in list(sys.modules):
            if k in ("st7789", "machine", "bleterm", "_bleboard", "DisplayBase") or \
                    k.startswith(("DisplayBase.", "pibody")):
                del sys.modules[k]
        self.st = sys.modules["st7789"] = _make_st7789()
        self.machine = sys.modules["machine"] = _make_machine()
        sys.modules["micropython"] = _make_micropython()
        sys.modules["framebuf"] = _make_framebuf()
        self.bt = sys.modules["bleterm"] = _make_bleterm()
        self.net = sys.modules["network"] = _make_network()
        for p in (MODULES, libs):
            if p and p not in sys.path:
                sys.path.insert(0, p)
        import _bleboard
        self.b = _bleboard

    def state(self, v):
        self.bt.state = v

    def wifi(self, sta=False, status=0, ap=False):
        self.net.flags[0][:] = [sta, status]
        self.net.flags[1][:] = [ap, 0]

    def tick(self, n=1):
        for _ in range(n):
            self.b._tick()

    def frame(self):
        return [row[:] for row in self.st._fb]

    def display(self):
        from pibody.Display import Display
        return Display()


def sheet(frames, path, scale=2):
    """frames: [(label, rows)] side by side with labels under each."""
    import importlib
    font = importlib.import_module("DisplayBase.sans16")
    gap, lab = 8, 20
    n = len(frames)
    sw, sh = n * (W + gap) + gap, H + lab + 2 * gap
    bgc = (40, 40, 44)
    out = [[bgc] * sw for _ in range(sh)]
    for i, (label, rows) in enumerate(frames):
        ox = gap + i * (W + gap)
        for y in range(H):
            for x in range(W):
                out[gap + y][ox + x] = _rgb(rows[y][x])
        a, left, _ = smooth_alpha(font, label)
        for r, row in enumerate(a):
            for c, v in enumerate(row[:W]):
                k = 40 + 190 * v // 15
                out[gap + H + 2 + r][ox + max(0, left + c)] = (k, k, k + 4 if k < 250 else k)
    write_png(path, out, scale)


# --- scenarios ------------------------------------------------------------------

STARTED, LINK, AUTH, PAIRING, IDLE = 0x01, 0x02, 0x04, 0x08, 0x10


def scenarios(libs):
    out = []
    cfg = {"display_status": True}

    # 1-3: entry boot (logo screen) in its states
    bd = Board(libs)
    bd.state(STARTED | PAIRING | IDLE)
    bd.b.status(cfg, True)
    out.append(("logo: pairing", bd.frame()))
    bd.tick(2)  # blink off phase
    out.append(("logo: blink off", bd.frame()))
    bd.state(STARTED | IDLE)
    bd.tick()
    out.append(("logo: paired", bd.frame()))
    bd.state(STARTED | LINK | AUTH | IDLE)
    bd.tick()
    out.append(("logo: IDE connected", bd.frame()))

    # 5: a program running with the bar (pibody logo demo drawing under it)
    if libs:
        bd.state(STARTED | LINK | AUTH)
        d = bd.display()
        d.logo()
        bd.tick()
        out.append(("program + bar", bd.frame()))
        d.bar.dot()
        d.fill(d.BLACK)
        d.text("Hello", 10, 4, d.font_bold, d.WHITE, d.BLACK)
        bd.tick()
        out.append(("bar.dot()", bd.frame()))
    # the four display fonts, Latin, Russian and Kazakh
    if libs:
        bd = Board(libs)
        d = bd.display()
        d.fill(d.BLACK)
        y = 4
        for f in (d.font_small, d.font_medium, d.font_large, d.font_bold):
            d.text(f.NAME, 6, y, f, d.WHITE, d.BLACK)
            y += f.HEIGHT
            d.text("Привет! Сәлем, Қазақстан", 6, y, f, d.YELLOW, d.BLACK)
            y += f.HEIGHT + 6
        d.text("ӘәҒғҚқҢңӨөҰұҮүҺһІі", 6, y, d.font_medium, d.CYAN, d.BLACK)
        y += 24
        d.fill_rect(0, y, 240, 32, 0x18E4)
        d.text("Score: 1280", 6, y + 4, d.font_large, d.GREEN, 0x18E4)
        out.append(("fonts", bd.frame()))
        # Display.print() console
        bd = Board(libs)
        d = bd.display()
        d.clear()
        for k in range(6):
            d.print("line", k, "temperature 23.5 C, влажность 40%")
        out.append(("display.print", bd.frame()))
        # canvas: off-screen drawing shown in one go
        bd = Board(libs)
        d = bd.display()
        d.fill(d.BLACK)
        c = d.canvas(120, 80)
        c.fill(0x18E4)
        c.rect(4, 4, 112, 72, d.YELLOW)
        c.fill_rect(20, 20, 40, 40, d.RED)
        c.fill_rect(60, 30, 40, 30, d.CYAN)
        c.show(60, 120)
        out.append(("canvas", bd.frame()))

    # Wi-Fi icon on the bar: each state's bar, stacked
    bd = Board(libs)
    bd.state(STARTED | LINK | AUTH | IDLE)
    bd.b.status(cfg, False)
    rows = [[0] * W for _ in range(H)]
    for i, (sta, st, ap) in enumerate(((False, 0, False), (True, 0, False), (True, 1, False),
                                       (True, 3, False), (True, -3, False), (False, 0, True))):
        bd.wifi(sta, st, ap)
        bd.tick()
        f = bd.frame()
        for r in range(16):
            rows[8 + i * 24 + r] = f[r][:]
    out.append(("wifi: off,idle,join,up,fail,setup", rows))
    # pibody.wifi_setup() screen (QR code to join the board's setup network)
    if libs:
        bd = Board(libs)
        bd.state(STARTED | LINK | AUTH | IDLE)
        bd.b.status(cfg, False)
        from pibody.IOT.WiFiSetup import _Setup
        su = _Setup()
        su.key = "48213907"
        su.show_join()
        bd.wifi(True, 0, True)
        bd.tick()
        out.append(("wifi_setup()", bd.frame()))
        su.show_join("Wi-Fi: wrong password for 'Home'.".replace("Wi-Fi: ", ""))
        out.append(("wifi_setup(): failed", bd.frame()))

    # button-hold feedback screens
    import io
    for on in (False, True):
        bd = Board(libs)
        bd.b.config(cfg)
        bd.b.open = lambda *a, _on=on: io.StringIO('{"ble_on_boot": %s}' % ("true" if _on else "false"))
        bd.b.feedback("safe")
        out.append(("hold 1s: " + ("off" if on else "on"), bd.frame()))
    bd.b.feedback("pairing")
    out.append(("hold 5s: pairing", bd.frame()))

    # normal boot (saved BLE mode), waiting at the prompt
    bd = Board(libs)
    bd.state(STARTED | IDLE)
    bd.b.status(cfg, False)
    out.append(("boot: BLE mode", bd.frame()))
    return out


def main():
    args = sys.argv[1:]
    libs = os.path.expanduser("~/Tools/pibody_libs")
    if "--libs" in args:
        i = args.index("--libs")
        libs = os.path.expanduser(args[i + 1])
        del args[i:i + 2]
    if not os.path.isdir(os.path.join(libs, "pibody")):
        libs = None
    out_dir = args[0] if args else os.path.join(HERE, "out")
    os.makedirs(out_dir, exist_ok=True)
    frames = scenarios(libs)
    for i, (label, rows) in enumerate(frames):
        name = "%d-%s.png" % (i + 1, "".join(c if c.isalnum() else "_" for c in label))
        write_png(os.path.join(out_dir, name), rows, 2)
    sheet(frames, os.path.join(out_dir, "sheet.png"), 1)
    print("wrote", len(frames), "frames to", out_dir)


if __name__ == "__main__":
    main()
