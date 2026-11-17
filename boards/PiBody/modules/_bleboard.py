# PiBody board hooks for the BLE REPL (_bleboot.py): board defaults, the BLE
# mode toggle, the logo screen and the top bar.
#
# BLE mode: hold BUTTON_LEFT (GP20) 1 s while switching on to turn it on or
# off (saved as "ble_on_boot" in /ble_config.json); 5 s also opens pairing.
# Outside BLE mode nothing here touches the display: the board is the plain
# USB PiBody.
#
# In BLE mode a 16 px bar at the top shows the Bluetooth state, the board name
# and a running dot. The st7789 driver keeps it on top: drawing into the bar
# rows is dropped (st7789._overlay). Programs control it with display.bar:
#   display.bar.show()     full bar (default)
#   display.bar.dot()      a single status dot in the top-right corner
#   display.bar.hide()     nothing, full screen, until show() or the next boot
#   display.bar.retreat()  hidden while this program runs, back at the prompt
# Any error here is printed and leaves the BLE REPL working.

DEFAULTS = {
    "button_pin": 20,            # BUTTON_LEFT
    "button_active_high": True,  # reads high when pressed (pulled down on the board)
    "display_status": True,
}
TOGGLE = True

BAR_H = 16
TICK_MS = 250
W, H = 240, 320
DOT = (226, 0, 14, 14)  # x, y, w, h of the dot area

WHITE      = 0xFFFF  # (255, 255, 255)
BLACK      = 0x0000  # (0, 0, 0)
INK        = 0x18E4  # (28, 31, 38)
MUTED      = 0x7BF1  # (120, 126, 138)
LINE       = 0xD6DC  # (214, 218, 224)
BLUE       = 0x0B9E  # (10, 112, 245)
BLUE_TINT  = 0xE77F  # (228, 238, 254)
GREEN      = 0x2DAA  # (40, 180, 80)
GREEN_TINT = 0xDFBC  # (222, 245, 229)
AMBER      = 0xF4A1  # (245, 150, 10)
AMBER_TINT = 0xFF9A  # (254, 240, 215)
RED        = 0xE186  # (230, 50, 50)
GREY_BADGE = 0xC65A  # (196, 202, 212)

# _bar_state() bits (bleterm.c)
STARTED, LINK, AUTH, PAIRING, IDLE = 0x01, 0x02, 0x04, 0x08, 0x10

_mode = "off"      # off (not in BLE mode), full, dot, hidden, retreat
_cfg_display = True
_d = None          # own st7789.ST7789 (never the DisplayBase singleton)
_fonts = None      # DisplayBase package: fonts
_name = ""
_timer = None
_last = None       # state key last painted
_blink_on = True
_ticks = 0
_logo = False      # the logo screen is still on the panel (no user drawing yet)


# --- panel ------------------------------------------------------------------

def _panel():
    """Own driver object on the display pins, initialised (panel on)."""
    global _d, _fonts
    if _d is None:
        import DisplayBase as db
        from machine import Pin, SPI
        import st7789
        _fonts = db.DisplayBase
        _d = st7789.ST7789(
            SPI(1, baudrate=db.BAUDRATE, sck=Pin("DISPLAY_SCK"), mosi=Pin("DISPLAY_MOSI")),
            W, H,
            reset=Pin("DISPLAY_RESET", Pin.OUT),
            dc=Pin("DISPLAY_DC", Pin.OUT),
            cs=Pin("DISPLAY_CS", Pin.OUT),
            rotation=2, options=0, buffer_size=2048)
        _d.init()
    return _d


def _bypass(on):
    import st7789
    st7789._overlay_bypass(on)


def _text(s, x, y, font, fg, bg):
    _d.text(font, s, x, y, fg, bg)


def _center(s, y, font, fg, bg):
    _text(s, max(0, (W - len(s) * font.WIDTH) // 2), y, font, fg, bg)


def _poly(cx, cy, r, n, bump, color, angle_offset=None):
    """Same shape as pibody Display.draw_polygon (used by the Artisan logo)."""
    import math
    step = 360 / n
    if angle_offset is None:
        angle_offset = step / 2 if n % 2 == 0 else 90
    k = math.pi / 180
    pts = []
    a = 0
    for _ in range(n + 1):
        dx = cx + r * math.cos(k * (a - angle_offset))
        dy = cy + r * math.sin(k * (a - angle_offset))
        a += step
        ddx = cx + r * math.cos(k * (a - angle_offset))
        ddy = cy + r * math.sin(k * (a - angle_offset))
        pts.append((round(dx), round(dy)))
        pts.append((round(cx + (dx + (ddx - dx) / 2 - cx) * bump),
                    round(cy + (dy + (ddy - dy) / 2 - cy) * bump)))
    _d.fill_polygon(pts, 0, 0, color)


# --- painting (always with the overlay bypass on) ----------------------------

def _blinking(v):
    return bool(v & PAIRING) and not v & AUTH


def _status(v):
    """(text, dot colour, chip tint, hint) for the logo screen."""
    if v & AUTH:
        return "Connected", GREEN, GREEN_TINT, "Run code from the IDE"
    if v & LINK:
        return "Connecting", BLUE, BLUE_TINT, "Checking the IDE key"
    if v & PAIRING:
        return "Ready to pair", AMBER, AMBER_TINT, "Connect from the IDE"
    return "Waiting for IDE", BLUE, BLUE_TINT, "Open the IDE to connect"


def _pill(x, y, w, r, color):
    """Rounded bar 2r+1 tall from x to x+w-1."""
    _d.fill_rect(x + r, y, w - 2 * r, 2 * r + 1, color)
    _d.fill_circle(x + r, y + r, r, color)
    _d.fill_circle(x + w - 1 - r, y + r, r, color)


def _rune(cx, cy, color):
    """Bluetooth rune, 7x11, centred on cx, cy."""
    _d.vline(cx, cy - 5, 11, color)
    _d.line(cx - 3, cy - 3, cx + 3, cy + 2, color)
    _d.line(cx + 3, cy + 2, cx, cy + 5, color)
    _d.line(cx - 3, cy + 3, cx + 3, cy - 2, color)
    _d.line(cx + 3, cy - 2, cx, cy - 5, color)


def _paint_icon(v, x=3):
    """Bluetooth badge: blue circle with a white rune; pale while blinking off."""
    _d.fill_rect(x, 0, 15, BAR_H - 1, WHITE)
    off = _blinking(v) and not _blink_on
    _d.fill_circle(x + 7, 7, 7, GREY_BADGE if off else BLUE)
    _rune(x + 7, 7, WHITE)


def _paint_link(v, x=20):
    """Green link mark (two rings joined) while the IDE is connected."""
    _d.fill_rect(x, 0, 16, BAR_H - 1, WHITE)
    if v & AUTH:
        _d.fill_circle(x + 4, 7, 3, GREEN)
        _d.fill_circle(x + 11, 7, 3, GREEN)
        _d.fill_rect(x + 4, 6, 7, 3, GREEN)
        _d.fill_circle(x + 4, 7, 1, WHITE)
        _d.fill_circle(x + 11, 7, 1, WHITE)


def _paint_running(v):
    x = DOT[0]
    _d.fill_rect(x, 0, DOT[2], BAR_H - 1, WHITE)
    if not v & IDLE:
        _d.fill_circle(x + 7, 7, 5, GREEN_TINT)
        _d.fill_circle(x + 7, 7, 3, GREEN)


def _paint_bar(v):
    _d.fill_rect(0, 0, W, BAR_H - 1, WHITE)
    _text(_name[:22], 40, 0, _fonts.font_small, INK, WHITE)
    _d.hline(0, BAR_H - 1, W, INK)
    _paint_icon(v)
    _paint_link(v)
    _paint_running(v)


def _paint_dot(v):
    x, y, w, h = DOT
    cx, cy = x + w // 2, y + h // 2
    if v & AUTH:
        color = GREEN
    elif _blinking(v) and not _blink_on:
        color = GREY_BADGE
    else:
        color = BLUE
    _d.fill_circle(cx, cy, 6, WHITE)
    _d.fill_circle(cx, cy, 5, color)


LOGO_Y = 116
NAME_Y = 204
CHIP_Y = 242
HINT_Y = 288


def _paint_logo(v):
    _d.fill(WHITE)
    _poly(120, LOGO_Y, 68, 8, 0.7, BLACK)
    _poly(120, LOGO_Y, 48, 4, 0.3, WHITE, angle_offset=0)
    _center(_name[:20], NAME_Y, _fonts.font_large, INK, WHITE)
    _paint_logo_status(v)


def _paint_logo_status(v):
    s, dot, tint, hint = _status(v)
    f = _fonts.font_medium
    _d.fill_rect(0, CHIP_Y, W, H - CHIP_Y, WHITE)
    r = 14
    w = len(s) * f.WIDTH + 2 * r + 18
    x = (W - w) // 2
    _pill(x, CHIP_Y, w, r, tint)
    _d.fill_circle(x + r + 3, CHIP_Y + r, 4, dot)
    _text(s, x + r + 14, CHIP_Y + r - f.HEIGHT // 2 + 1, f, INK, tint)
    _center(hint, HINT_Y, _fonts.font_small, MUTED, WHITE)


def _clear_bar_area():
    """Bar rows back to the background when the bar goes away."""
    bg = WHITE if _logo else BLACK
    _d.fill_rect(0, 0, W, BAR_H, bg)


def _paint(v, full):
    _bypass(True)
    try:
        if _mode == "full":
            if full:
                _paint_bar(v)
            else:
                _paint_icon(v)
            if _logo and full:
                _paint_logo_status(v)
        elif _mode == "dot":
            _paint_dot(v)
    finally:
        _bypass(False)


# --- overlay and modes ------------------------------------------------------

def _apply_overlay():
    """Driver clip for the current mode. While the logo is up, watch the whole
    screen: the first drawing by user code ends the logo screen."""
    import st7789
    watch = (0, BAR_H, W, H - BAR_H) if _logo else None
    if _mode == "full":
        st7789._overlay(BAR_H, watch)
    elif _mode == "dot":
        st7789._overlay(0, DOT)
    else:
        st7789._overlay(0, watch)


def _set_mode(m):
    global _mode, _last
    if _mode == "off" or m == _mode:
        return
    was = _mode
    _mode = m
    _apply_overlay()
    if was == "full" and m != "full":
        _bypass(True)
        try:
            _clear_bar_area()
        finally:
            _bypass(False)
    _last = None  # repaint on the next tick
    _tick()


class _Bar:
    """display.bar: the BLE mode top bar (no-op outside BLE mode)."""
    HEIGHT = BAR_H

    @property
    def mode(self):
        """"off" (not BLE mode), "full", "dot", "hidden" or "retreat"."""
        return _mode

    @property
    def height(self):
        """Rows at the top covered by the bar (0 unless the full bar shows)."""
        return BAR_H if _mode == "full" else 0

    def show(self):
        _set_mode("full")

    def dot(self):
        _set_mode("dot")

    def hide(self):
        _set_mode("hidden")

    def retreat(self):
        _set_mode("retreat")


bar = _Bar()


# --- timer --------------------------------------------------------------------

def _tick(_t=None):
    global _last, _blink_on, _ticks, _logo
    if _mode == "off":
        return
    try:
        import bleterm
        import st7789
        v = bleterm._bar_state()
        if _mode == "retreat":
            if v & IDLE:  # the program ended: bar back
                _set_mode("full")
            return
        if _mode == "hidden":
            return
        _ticks += 1
        blink_changed = False
        if _blinking(v):
            if _ticks % 2 == 0:
                _blink_on = not _blink_on
                blink_changed = True
        elif not _blink_on:
            _blink_on = True
        dirty = st7789._overlay_dirty()
        if dirty and _logo:
            _logo = False  # user code drew or reset the panel
            _apply_overlay()
        key = v & 0x3F
        if dirty or key != _last:
            _last = key
            _paint(v, True)
        elif blink_changed:
            _paint(v, False)
    except Exception as e:
        stop()
        print("_bleboard: bar:", e)


def stop():
    """Stop the bar and give the whole screen back to user code."""
    global _timer, _mode
    if _timer is not None:
        _timer.deinit()
        _timer = None
    _mode = "off"
    try:
        import st7789
        st7789._overlay(0)
    except Exception:
        pass


# --- hooks called by _bleboot ------------------------------------------------

def config(cfg):
    """Config loaded (before the button is sampled)."""
    global _cfg_display, _name
    _cfg_display = cfg.get("display_status", True)
    _name = cfg.get("name") or ""


def feedback(stage):
    """Boot button held past a threshold: say what releasing it will do."""
    global _d
    if not _cfg_display:
        return
    _panel()
    import json
    _d.fill(WHITE)
    _poly(120, 100, 52, 8, 0.7, BLACK)
    _poly(120, 100, 37, 4, 0.3, WHITE, angle_offset=0)
    f = _fonts
    if stage == "safe":
        try:
            on = json.loads(open("/ble_config.json").read()).get("ble_on_boot", False)
        except Exception:
            on = False
        _center("Release now for", 178, f.font_small, MUTED, WHITE)
        s, dot, tint = ("Bluetooth OFF", RED, AMBER_TINT) if on else ("Bluetooth ON", BLUE, BLUE_TINT)
        hint = "Back to USB only" if on else "Code over Bluetooth"
    else:
        _center("Release now for", 178, f.font_small, MUTED, WHITE)
        s, dot, tint, hint = "Pairing", AMBER, AMBER_TINT, "Open for 60 seconds"
    r = 14
    fm = f.font_medium
    w = len(s) * fm.WIDTH + 2 * r + 18
    x = (W - w) // 2
    _pill(x, 204, w, r, tint)
    _d.fill_circle(x + r + 3, 204 + r, 4, dot)
    _text(s, x + r + 14, 204 + r - fm.HEIGHT // 2 + 1, fm, INK, tint)
    _center(hint, 250, f.font_small, MUTED, WHITE)
    if stage == "safe":
        _center("Keep holding 5 s: pairing", 290, f.font_small, MUTED, WHITE)


def status(cfg, entered=False):
    """After _bleboot: in BLE mode show the logo screen (entry boot) or a
    black screen, the bar on top, and follow the BLE state."""
    global _mode, _timer, _logo, _name, _last
    import st7789
    st7789._overlay(0)  # survives soft reset: start clean
    config(cfg)
    import bleterm
    v = bleterm._bar_state()
    if not _cfg_display or not v & STARTED:
        return  # USB board: display untouched
    _name = bleterm.status().get("name") or _name
    _panel()
    _mode = "full"
    _logo = bool(entered)
    _last = v & 0x3F
    _bypass(True)
    try:
        if _logo:
            _paint_logo(v)
        else:
            _d.fill(BLACK)
        _paint_bar(v)
    finally:
        _bypass(False)
    st7789._overlay_dirty()  # our own init/fill
    _apply_overlay()
    if _timer is None:
        from machine import Timer
        _timer = Timer(-1, period=TICK_MS, callback=_tick)
