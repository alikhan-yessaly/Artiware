from machine import Pin, SPI
import st7789
# One font family (Roboto, anti-aliased, proportional) in four sizes, made
# by tools/makefont.swift: ASCII, Russian, Kazakh and other Cyrillic letters.
from . import sans16, sans20, sans24, sans32b

DISPLAY_BUF_SIZE = 80 * 80 * 2
BAUDRATE = 60_000_000
WIDTH = 240
HEIGHT = 320

def star_points(cx, cy, r, n, bump, angle_offset=None):
    """Points of the Artisan logo's star shapes (float, closed)."""
    import math
    step = 360 / n
    if angle_offset is None:
        angle_offset = step / 2 if n % 2 == 0 else 90
    k = math.pi / 180
    pts = []
    for i in range(n):
        a = k * (i * step - angle_offset)
        b = k * ((i + 1) * step - angle_offset)
        dx, dy = cx + r * math.cos(a), cy + r * math.sin(a)
        ex, ey = cx + r * math.cos(b), cy + r * math.sin(b)
        pts.append((dx, dy))
        pts.append((cx + ((dx + ex) / 2 - cx) * bump, cy + ((dy + ey) / 2 - cy) * bump))
    return pts


def artisan_logo(dev, cx, cy, r, fg=0x0000, bg=0xFFFF):
    """The Artisan logo with smooth edges, centred on cx, cy, radius r, drawn
    on a bg square (2r+4 wide)."""
    dev.fill_shapes(round(cx - r) - 2, round(cy - r) - 2, round(2 * r) + 5, round(2 * r) + 5, bg, (
        (star_points(cx, cy, r, 8, 0.7), fg),
        (star_points(cx, cy, r * 0.7, 4, 0.3, 0), bg)))


class _NoBar:
    HEIGHT = 0
    mode = "off"
    height = 0
    def show(self): pass
    def dot(self): pass
    def hide(self): pass
    def retreat(self): pass


class DisplayBase(st7789.ST7789):
    # line heights 16, 20, 24 and 32 rows, like the old VGA fonts
    font_small = sans16
    font_medium = sans20
    font_large = sans24
    font_bold = sans32b
    
    # Singleton instance
    _instance = None
    def __new__(cls):
        if cls._instance is None:
            cls._instance = super(DisplayBase, cls).__new__(cls)
            cls._instance._initialized = False
        return cls._instance

    @property
    def bar(self):
        """BLE mode top bar: bar.show(), bar.dot(), bar.hide(), bar.retreat(),
        bar.height (rows it covers), bar.mode. Does nothing in USB mode."""
        try:
            import _bleboard
            return _bleboard.bar
        except ImportError:
            return _NoBar()

    def __init__(self):
        if self._initialized:
            return
        self._initialized = True
        super().__init__(
            SPI(1, baudrate=BAUDRATE, sck=Pin("DISPLAY_SCK"), mosi=Pin("DISPLAY_MOSI")),
            WIDTH, HEIGHT,
            reset=Pin("DISPLAY_RESET", Pin.OUT),
            dc=Pin("DISPLAY_DC", Pin.OUT),
            cs=Pin("DISPLAY_CS", Pin.OUT),
            rotation=2,
            options=0,
            buffer_size=DISPLAY_BUF_SIZE)
        super().init()    
        
        self.width = WIDTH
        self.height = HEIGHT

    
    def text(self, text, x, y, font=font_small, fg=st7789.WHITE, bg=st7789.BLACK):
        """Draw text with its top-left at x, y on a bg box font.HEIGHT rows tall.
        Returns the x after the text."""
        return super().text(font, text, x, y, fg, bg)

    def text_width(self, text, font=font_small):
        """Width in pixels of text drawn with font."""
        chars, adv = font.CHARS, font.ADV
        q = chars.find("?")
        w = 0
        for ch in text:
            i = chars.find(ch)
            w += adv[i if i >= 0 else q]
        return w

    # names from the first smooth-text version
    def smooth_text(self, text, x, y, font=font_medium, fg=st7789.WHITE, bg=st7789.BLACK):
        return self.text(text, x, y, font, fg, bg)

    def smooth_width(self, text, font=font_medium):
        return self.text_width(text, font)

    def hsv(self, h, s, v):
        """
            Analogous to color() method, but uses hue, saturation, value as inputs.
            h: 0-360, 
            s: 0-1, 
            v: 0-1
        """
        if s == 0:
            r = g = b = int(v * 255)
        else:
            h %= 360
            i = int(h / 60)
            f = (h / 60) - i
            p = int(v * (1 - s) * 255)
            q = int(v * (1 - s * f) * 255)
            t = int(v * (1 - s * (1 - f)) * 255)
            v = int(v * 255)
            r, g, b = [
                (v, t, p),
                (q, v, p),
                (p, v, t),
                (p, q, v),
                (t, p, v),
                (v, p, q),
            ][i]

        return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
