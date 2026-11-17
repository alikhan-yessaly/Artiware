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

WHITE = 0xFFFF
BLACK = 0x0000
BLUE = 0x041F   # (0, 130, 255)
GREEN = 0x0540  # (0, 170, 0)
GREY = 0x8C51
RED = 0xF800

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


def _status_text(v):
    if v & AUTH:
        return "Connected", GREEN
    if v & LINK:
        return "Connecting...", BLUE
    if v & PAIRING:
        return "Waiting to pair", BLUE
    return "Waiting for IDE", BLUE


def _paint_icon(v, x=4):
    """Bluetooth rune (12x14) at x, blank in the off phase of a blink."""
    _d.fill_rect(x, 0, 13, BAR_H - 1, WHITE)
    if _blinking(v) and not _blink_on:
        return
    c = x + 5
    for o in (0, 1):
        _d.line(c + o, 1, c + o, 13, BLUE)
        _d.line(c - 4 + o, 4, c + 4 + o, 10, BLUE)
        _d.line(c + 4 + o, 10, c + o, 13, BLUE)
        _d.line(c - 4 + o, 10, c + 4 + o, 4, BLUE)
        _d.line(c + 4 + o, 4, c + o, 1, BLUE)


def _paint_link(v, x=18):
    """Green link mark next to the icon while the IDE is connected."""
    _d.fill_rect(x, 0, 14, BAR_H - 1, WHITE)
    if v & AUTH:
        _d.fill_circle(x + 3, 7, 2, GREEN)
        _d.fill_circle(x + 10, 7, 2, GREEN)
        _d.fill_rect(x + 3, 6, 8, 3, GREEN)


def _paint_running(v):
    _d.fill_rect(DOT[0], 0, DOT[2], BAR_H - 1, WHITE)
    if not v & IDLE:
        _d.fill_circle(DOT[0] + 7, 7, 4, GREEN)


def _paint_bar(v):
    _d.fill_rect(0, 0, W, BAR_H - 1, WHITE)
    _text(_name[:23], 36, 0, _fonts.font_small, BLACK, WHITE)
    _d.hline(0, BAR_H - 1, W, BLACK)
    _paint_icon(v)
    _paint_link(v)
    _paint_running(v)


def _paint_dot(v):
    x, y, w, h = DOT
    cx, cy = x + w // 2, y + h // 2
    if v & AUTH:
        color = GREEN
    elif _blinking(v) and not _blink_on:
        color = WHITE
    else:
        color = BLUE
    _d.fill_circle(cx, cy, 6, WHITE)
    _d.fill_circle(cx, cy, 5, color)


def _paint_logo(v):
    _d.fill(WHITE)
    _poly(120, 120, 70, 8, 0.7, BLACK)
    _poly(120, 120, 49, 4, 0.3, WHITE, angle_offset=0)
    _center(_name[:15], 214, _fonts.font_bold, BLACK, WHITE)
    _paint_logo_status(v)


def _paint_logo_status(v):
    _d.fill_rect(0, 256, W, 24, WHITE)
    s, color = _status_text(v)
    _center(s, 258, _fonts.font_medium, color, WHITE)


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
    if not _cfg_display:
        return
    d = _panel()
    f = _fonts
    d.fill(WHITE)
    if stage == "safe":
        import json
        try:
            on = json.loads(open("/ble_config.json").read()).get("ble_on_boot", False)
        except Exception:
            on = False
        _center("Release for", 120, f.font_medium, BLACK, WHITE)
        _center("BLE mode " + ("OFF" if on else "ON"), 150, f.font_bold, RED if on else BLUE, WHITE)
        _center("Hold 5 s: pairing", 220, f.font_small, GREY, WHITE)
    else:
        _center("Pairing mode", 150, f.font_bold, BLUE, WHITE)


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
