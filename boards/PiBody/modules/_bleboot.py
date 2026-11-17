# Pico W BLE IDE boot control (SPEC 5.4). Frozen; run by main.c after _boot.py
# and before boot.py/main.py. It must never stop the boot: every error is
# printed and ignored.

DEFAULTS = {
    "ble_on_boot": False,
    "name": None,
    "require_pairing": True,
    "led_status": True,
    "button_pin": 20,
    "button_active_high": False,
}

# Optional board module (e.g. PiBody): DEFAULTS overrides and extra keys, and
# hooks config(cfg) after loading the config, feedback(stage) during the button
# hold and status(cfg) after the start.
try:
    import _bleboard
except ImportError:
    _bleboard = None
if _bleboard is not None:
    DEFAULTS.update(getattr(_bleboard, "DEFAULTS", {}))

CONFIG_PATH = "/ble_config.json"
SAFE_MS = 1000
PAIR_MS = 5000


def _valid(key, v):
    if key in ("ble_on_boot", "require_pairing", "led_status", "button_active_high"):
        return isinstance(v, bool)
    if key == "name":
        return v is None or (isinstance(v, str) and 0 < len(v.encode()) <= 29)
    if key == "button_pin":
        return v is None or (isinstance(v, int) and not isinstance(v, bool) and 0 <= v <= 29)
    if key in DEFAULTS:  # board key: same type as its default
        return type(v) is type(DEFAULTS[key])
    return False


def parse_config(text):
    """Config dict from JSON text; bad or missing keys fall back to defaults."""
    cfg = dict(DEFAULTS)
    try:
        import json
        data = json.loads(text)
    except Exception:
        return cfg
    if not isinstance(data, dict):
        return cfg
    for k in DEFAULTS:
        if k in data and _valid(k, data[k]):
            cfg[k] = data[k]
    return cfg


def load_config(path=CONFIG_PATH):
    try:
        with open(path) as f:
            text = f.read()
    except Exception:
        return dict(DEFAULTS)
    return parse_config(text)


def ensure_config(path=CONFIG_PATH):
    """Board with its own defaults: write them on first boot, so tools that assume
    the generic defaults for missing keys (the IDE settings) see the real ones."""
    import os
    try:
        os.stat(path)
        return False
    except OSError:
        pass
    import json
    with open(path, "w") as f:
        f.write(json.dumps(DEFAULTS))
    return True


def _hook(name, *args):
    fn = getattr(_bleboard, name, None)
    if fn is not None:
        try:
            fn(*args)
        except Exception as e:
            print("_bleboard.%s:" % name, e)


def set_on_boot(value=True, path=CONFIG_PATH):
    """Set ble_on_boot in the config file, keeping the other keys (SPEC 5.5)."""
    import json
    data = {}
    try:
        with open(path) as f:
            data = json.loads(f.read())
        if not isinstance(data, dict):
            data = {}
    except Exception:
        data = {}
    if data.get("ble_on_boot") is value:
        return
    data["ble_on_boot"] = value
    with open(path, "w") as f:
        f.write(json.dumps(data))


def decide(cfg, held_ms, session, running):
    """Return (safe_mode, pairing, start)."""
    safe = held_ms >= SAFE_MS
    pairing = held_ms >= PAIR_MS
    start = safe or cfg["ble_on_boot"] or session or running
    return safe, pairing, start


def pressed(value, active_high):
    """Pin level -> button pressed. Active low (default) uses the internal pull-up,
    active high the internal pull-down."""
    return bool(value) == bool(active_high)


def _button(pin_no, led_on, active_high=False):
    """Sample the button for up to 5 s; return held_ms."""
    import machine
    import time

    pull = machine.Pin.PULL_DOWN if active_high else machine.Pin.PULL_UP
    pin = machine.Pin(pin_no, machine.Pin.IN, pull)
    try:
        time.sleep_us(50)  # let the pull settle
        if not pressed(pin.value(), active_high):
            return 0
        led = machine.Pin("LED", machine.Pin.OUT) if led_on else None

        def set_led(v):
            if led is not None:
                led.value(v)

        t0 = time.ticks_ms()
        held = 0
        safe_shown = False
        set_led(0)
        while pressed(pin.value(), active_high) and held < PAIR_MS:
            time.sleep_ms(10)
            held = time.ticks_diff(time.ticks_ms(), t0)
            if held >= SAFE_MS and not safe_shown:
                safe_shown = True
                _hook("feedback", "safe")
                for _ in range(5):
                    set_led(1)
                    time.sleep_ms(50)
                    set_led(0)
                    time.sleep_ms(50)
                set_led(1)
                held = time.ticks_diff(time.ticks_ms(), t0)
        if held >= PAIR_MS:
            _hook("feedback", "pairing")
            for _ in range(10):
                set_led(1)
                time.sleep_ms(50)
                set_led(0)
                time.sleep_ms(50)
        set_led(0)
        return held
    finally:
        machine.Pin(pin_no, machine.Pin.IN, None)


def main():
    import bleterm

    if _bleboard is not None:
        try:
            ensure_config()
        except Exception as e:
            print("_bleboot: config:", e)
    cfg = load_config()
    _hook("config", cfg)
    held = 0
    if cfg["button_pin"] is not None:
        try:
            held = _button(cfg["button_pin"], cfg["led_status"], cfg["button_active_high"])
        except Exception as e:
            print("_bleboot: button:", e)
        bleterm._led_reclaim()  # _button may have used Pin("LED")
    running = bleterm.status()["state"] != "off"
    safe, pairing, start = decide(cfg, held, bleterm.session(), running)
    bleterm._set_safe_mode(safe)
    bleterm.configure(name=cfg["name"], require_pairing=cfg["require_pairing"], led=cfg["led_status"])
    if start:
        bleterm.start()
        if pairing:
            bleterm.pairing(60)
    bleterm._boot_done()
    _hook("status", cfg)
    if safe:
        print("_bleboot: safe mode (boot.py/main.py skipped)%s" % (", pairing open" if pairing else ""))


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        print("_bleboot:", e)
