# PiBody board hooks for the BLE REPL (_bleboot.py): board defaults and a BLE
# status screen on the ST7789 display.
#
# The display is only a helper: any error here is printed and the BLE REPL
# keeps working. The screen is used only after BLE is on (or the boot button
# is held), and user code takes it over by creating DisplayBase(): the status
# screen then stops and clears. Set "display_status": false in
# /ble_config.json to never touch the display.

DEFAULTS = {
    "button_pin": 20,            # BUTTON_LEFT
    "button_active_high": True,  # reads high when pressed (pulled down on the board)
    "display_status": True,
}

POLL_MS = 500

_d = None          # own st7789.ST7789 while the status screen shows
_db = None         # the DisplayBase package (fonts, size, pins)
_timer = None
_last = None
_released = False  # user code took the display: never draw again until reset


def _grey():
    import st7789
    return st7789.color(140, 140, 140)


def _display():
    """The display for the status screen, or None once user code owns it.

    A separate st7789.ST7789 on the same pins, never the DisplayBase singleton:
    creating that here would hand user code a plain DisplayBase for its
    Display(DisplayBase) subclass."""
    global _d, _db
    if _released:
        return None
    if _d is None:
        import DisplayBase as db
        from machine import Pin, SPI
        import st7789
        if db.DisplayBase._instance is not None:  # user code has it already
            _release()
            return None
        d = st7789.ST7789(
            SPI(1, baudrate=db.BAUDRATE, sck=Pin("DISPLAY_SCK"), mosi=Pin("DISPLAY_MOSI")),
            db.WIDTH, db.HEIGHT,
            reset=Pin("DISPLAY_RESET", Pin.OUT),
            dc=Pin("DISPLAY_DC", Pin.OUT),
            cs=Pin("DISPLAY_CS", Pin.OUT),
            rotation=2, options=0, buffer_size=db.DISPLAY_BUF_SIZE)
        d.init()
        d.fill(0)
        _d, _db = d, db
        db.DisplayBase._on_claim = _release
        _header()
    return _d


def _text(s, x, y, font, color):
    _d.text(font, s, x, y, color, 0)


def _header():
    import os
    import st7789
    f = _db.DisplayBase
    _text("BLE REPL", 8, 8, f.font_bold, st7789.CYAN)
    v = os.uname().version.split(" on ", 1)
    _text(v[0][:29], 8, 48, f.font_small, _grey())
    if len(v) > 1:
        _text(v[1][:29], 8, 66, f.font_small, _grey())


def _release():
    """User code wants the display: stop drawing and hand it over blank."""
    global _d, _db, _released
    _released = True
    stop()
    if _d is not None:
        try:
            _d.fill(0)
        except Exception:
            pass
    _d = _db = None


def _lines(s):
    """Status dict -> [(text, color, font)] for the state area."""
    import st7789
    st = s["state"]
    out = []
    if st == "off":
        out.append(("BLE off", _grey(), "large"))
        out.append(("Hold LEFT 1 s at reset", _grey(), "small"))
    elif st == "advertising":
        out.append(("Searching...", st7789.YELLOW, "large"))
        out.append(("Waiting for the IDE", st7789.WHITE, "small"))
    else:
        out.append(("Connected", st7789.GREEN, "large"))
        out.append(("Locked: pair or sign in" if s["locked"] else "IDE ready", st7789.WHITE, "small"))
    if st != "off" and s["links"] > 1:
        out.append(("%d links" % s["links"], st7789.WHITE, "small"))
    if s["pairing"]:
        out.append(("Pairing open", st7789.MAGENTA, "medium"))
    if s["safe_mode"]:
        out.append(("Safe mode: main.py skipped", st7789.RED, "small"))
    return out


def _draw(s):
    d = _display()
    if d is None:
        return
    b = _db.DisplayBase
    fonts = {"small": b.font_small, "medium": b.font_medium, "large": b.font_large}
    d.fill_rect(0, 92, _db.WIDTH, _db.HEIGHT - 92, 0)
    import st7789
    y = 96
    if s.get("name"):
        _text(s["name"][:23], 8, y, b.font_medium, st7789.WHITE)
    y += 32
    for text, color, font in _lines(s):
        f = fonts[font]
        _text(text[:29], 8, y, f, color)
        y += f.HEIGHT + 8


def _tick(_t=None):
    global _last
    if _released:  # a tick scheduled before the hand-off
        return
    try:
        import bleterm
        s = bleterm.status()
        key = (s["state"], s["name"], s["locked"], s["links"], s["pairing"], s["safe_mode"])
        if key == _last:
            return
        if _d is None and _last is None and s["state"] == "off":
            return  # BLE never on: leave the display alone
        _last = key
        _draw(s)
    except Exception as e:
        stop()
        print("_bleboard: display:", e)


def stop():
    global _timer
    if _timer is not None:
        _timer.deinit()
        _timer = None


# --- hooks called by _bleboot ------------------------------------------------

_cfg_display = True


def config(cfg):
    """Config loaded (before the button is sampled)."""
    global _cfg_display
    _cfg_display = cfg.get("display_status", True)


def feedback(stage):
    """Boot button held past a threshold ("safe" or "pairing")."""
    if not _cfg_display:
        return
    import st7789
    d = _display()
    if d is None:
        return
    b = _db.DisplayBase
    d.fill_rect(0, 92, _db.WIDTH, _db.HEIGHT - 92, 0)
    if stage == "safe":
        _text("Safe mode", 8, 128, b.font_large, st7789.RED)
        _text("Keep holding: pairing", 8, 160, b.font_small, st7789.WHITE)
    else:
        _text("Pairing mode", 8, 128, b.font_large, st7789.MAGENTA)


def status(cfg):
    """After _bleboot started (or not) BLE: draw now and follow changes."""
    global _timer
    config(cfg)
    if not _cfg_display:
        return
    _tick()
    if _timer is None and not _released:
        from machine import Timer
        _timer = Timer(-1, period=POLL_MS, callback=_tick)
