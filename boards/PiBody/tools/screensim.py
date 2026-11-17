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

        def text(self, font, s, x0, y0, fg=0xFFFF, bg=0x0000):
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
        self.bt = sys.modules["bleterm"] = _make_bleterm()
        for p in (MODULES, libs):
            if p and p not in sys.path:
                sys.path.insert(0, p)
        import _bleboard
        self.b = _bleboard

    def state(self, v):
        self.bt.state = v

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
    font = importlib.import_module("DisplayBase.vga2_8x16")
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
        data = font._FONT if hasattr(font, "_FONT") else font.FONT
        x0 = ox
        for ch in label[:W // 8]:
            base = (ord(ch) - font.FIRST) * 16
            for i2 in range(128):
                if data[base + i2 // 8] >> (7 - i2 % 8) & 1:
                    out[gap + H + 4 + i2 // 8][x0 + i2 % 8] = (230, 230, 230)
            x0 += 8
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
