# Smooth (anti-aliased) proportional text for the ST7789 display.
#
# Fonts are DisplayBase.sans13, sans13b, sans17b, sans24b (Roboto, generated
# by tools/makefont.swift): ASCII, Russian and Kazakh Cyrillic, 4-bit edges
# blended between the text colour and the background colour.
#
#   from DisplayBase import smoothtext, sans17b
#   smoothtext.draw(display, "Привет, PiBody!", 10, 40, sans17b, fg, bg)
#   w = smoothtext.width("Hello", sans17b)
#
# A line is drawn in one blit: text + background box, HEIGHT rows tall.
import micropython

_luts = {}


def _lut(fg, bg):
    """16 RGB565 colours from bg (0) to fg (15), byte-swapped for blit_buffer."""
    key = (fg << 16) | bg
    t = _luts.get(key)
    if t is None:
        if len(_luts) > 8:
            _luts.clear()
        t = bytearray(32)
        fr, fgg, fb = fg >> 11, (fg >> 5) & 0x3F, fg & 0x1F
        br, bgg, bb = bg >> 11, (bg >> 5) & 0x3F, bg & 0x1F
        for a in range(16):
            r = br + (fr - br) * a // 15
            g = bgg + (fgg - bgg) * a // 15
            b = bb + (fb - bb) * a // 15
            c = (r << 11) | (g << 5) | b
            t[2 * a] = c >> 8
            t[2 * a + 1] = c & 0xFF
        _luts[key] = t
    return t


@micropython.viper
def _put(alpha: ptr8, aw: int, x0: int, bmp: ptr8, off: int, gw: int, gh: int):
    i = 0
    for y in range(gh):
        row = y * aw + x0
        for x in range(gw):
            b = int(bmp[off + (i >> 1)])
            v = (b >> 4) if (i & 1) == 0 else (b & 15)
            if v > int(alpha[row + x]):
                alpha[row + x] = v
            i += 1


@micropython.viper
def _paint(alpha: ptr8, aw: int, cx: int, cw: int, h: int, lut: ptr16, out: ptr16):
    k = 0
    for y in range(h):
        row = y * aw + cx
        for x in range(cw):
            out[k] = lut[alpha[row + x]]
            k += 1


def _layout(s, font):
    """[(glyph index, pen x)], left, right (ink and advance extent)."""
    chars = font.CHARS
    adv, xoff, gw = font.ADV, font.XOFF, font.GW
    pen = 0
    left = 0
    right = 0
    out = []
    for ch in s:
        i = chars.find(ch)
        if i < 0:
            i = chars.find("?")
        gx = pen + xoff[i] - 128
        if gw[i]:
            out.append((i, gx))
            if gx < left:
                left = gx
            if gx + gw[i] > right:
                right = gx + gw[i]
        pen += adv[i]
    if pen > right:
        right = pen
    return out, left, right


def width(s, font):
    """Advance width of s in pixels."""
    adv = font.ADV
    chars = font.CHARS
    w = 0
    for ch in s:
        i = chars.find(ch)
        w += adv[i if i >= 0 else chars.find("?")]
    return w


def draw(dev, s, x, y, font, fg=0xFFFF, bg=0x0000, screen_w=240):
    """Draw s with its top-left at x, y (the box is font.HEIGHT tall).
    Returns the x after the text."""
    glyphs, left, right = _layout(s, font)
    h = font.HEIGHT
    aw = right - left
    if aw <= 0:
        return x
    alpha = bytearray(aw * h)
    offs, gw, bmp = font.OFFS, font.GW, font.BITMAP
    for i, gx in glyphs:
        _put(alpha, aw, gx - left, bmp, offs[2 * i] | offs[2 * i + 1] << 8, gw[i], h)
    # crop to the screen
    sx = x + left
    cx = 0
    if sx < 0:
        cx = -sx
        sx = 0
    cw = min(aw - cx, screen_w - sx)
    if cw > 0:
        out = bytearray(cw * h * 2)
        _paint(alpha, aw, cx, cw, h, _lut(fg, bg), out)
        dev.blit_buffer(out, sx, y, cw, h)
    return x + right
