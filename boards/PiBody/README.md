# PiBody Firmware
This repository contains the **custom MicroPython firmware and its files** for the PiBody robot.

It includes:
- Python modules installed into the firmware `modules/` directory
- Custom C module for the ST7789 Display Driver by russhughes installed into the  `cmodules/` directory

## Firmware Versioning
The firmware version string is defined in the MicroPython build system.

To change the firmware version name, edit the following parameter in
`makeversionhdr.py`:

```python
git_tag = "v1.26.1-atlas.v1.0.0"
```

## Building the Firmware
```bash
cd ~/micropython/ports/rp2
rm -f -rf build-RPI_PICO_W/
make clean
make BOARD=RPI_PICO_W USER_C_MODULES=cmodules
```



## BLE REPL (branch `ble`)
This firmware adds a Bluetooth REPL and file access for the Pico W BLE web IDE
(from the picow-ble-ide project). The board advertises as `PiBody-XXXX`.

Banner: `MicroPython v1.29.0-pibody-ble.v1.1.0 on <date>; PiBody v1.1 (BLE REPL) with RP2040`
(version set in `board.mk`, no MicroPython edit needed).

### Build
MicroPython **v1.29.0** with the patches in `patches/micropython-v1.29.0/`:
```bash
make mp-prepare        # once: branch artiware-v1.29.0 + patches, mpy-cross, submodules
make                   # build/PiBody/firmware.uf2
```

### Use
- Switched on normally, the board is the plain USB PiBody: no BLE, display untouched.
- **BLE mode**: hold **BUTTON_LEFT** (GP20, high when pressed) for 1 s while
  switching on. The screen says "Release for BLE mode ON"; the setting is saved
  (`"ble_on_boot"` in `/ble_config.json`), so later power-ons stay in BLE mode
  and run `main.py`. Hold 1 s again to switch back to USB only.
  - The switch-on boot shows the Artisan logo, the board name and the BLE state,
    and skips `main.py`.
  - Hold 5 s: also opens pairing for 60 s. Until the first IDE has paired,
    pairing stays open by itself.
- **Top bar** (BLE mode only): 16 px at the top: Bluetooth icon (blinking while
  pairing is open, steady when paired, with a green link mark while the IDE is
  connected), board name, Wi-Fi icon while Wi-Fi is on (grey: on, amber:
  connecting, dark: connected, red: connect failed, blue: pibody
  `wifi_setup()` running; read from the driver's flags, no radio traffic),
  green dot while a program runs. The ST7789 driver
  drops drawing into those rows, so programs keep 240x320 coordinates.
  `display.bar.show()`, `.dot()` (one status dot in the corner), `.hide()`,
  `.retreat()` (hidden until the program ends), `.height`, `.mode`.
  `"display_status": false` in `/ble_config.json` turns screen and bar off.
- Fonts: one anti-aliased, proportional family (Roboto) replaces the VGA
  fonts. `display.font_small`, `font_medium`, `font_large`, `font_bold` are
  `DisplayBase.sans16`, `sans20`, `sans24` (Regular) and `sans32b` (Bold), with
  the old line heights 16/20/24/32. Letters: ASCII, Cyrillic U+0400-U+045F
  (Russian etc.) and Kazakh (Ә Ғ Қ Ң Ө Ұ Ү Һ І), Tajik, Tatar and Bashkir.
  `display.text(text, x, y, font, fg, bg)` returns the x after the text;
  `display.text_width(text, font)` measures it. The st7789 driver draws them in
  C (about 2 ms per 30-letter line). `vga2_8x16` etc. remain as names for these fonts;
  CP437 symbols (codes 0-31) and rare Cyrillic letters (U+0460 up) are gone.
- Display speed: SPI runs at 62.5 MHz (was 24 MHz). `st7789.spi_clock()` sets
  the peripheral clock to the system clock; `DisplayBase.display_spi()` builds
  the SPI with `min(62.5 MHz, clock/2)`. Calling `machine.freq()` later resets
  the peripheral clock to 48 MHz, so SPI then runs at 24 MHz (slower, still
  correct). Fills, blits and text go out by DMA in one transfer per call, and
  text and `fill_shapes` compute the next band while DMA sends the last one.
  Full-screen fill 21.7 ms (was 61.7), 120x120 blit 3.9 ms (was 10.6), logo
  32.6 ms (was 106). Single pixels and short lines cost about 35-45 us each,
  mostly MicroPython call overhead, so draw many small parts in a canvas.
- `display.canvas(w, h)`: an off-screen RGB565 `framebuf.FrameBuffer` (w*h*2
  bytes of RAM). Draw into it with the framebuf methods (`fill`, `rect`,
  `line`, `text`, ...), then `canvas.show(x, y)` sends it in one transfer
  (120x120: 3.6 ms). `display.blit_framebuf(buf, x, y, w, h)` sends any
  little-endian RGB565 buffer the same way.
- IDE Stop (or Ctrl-C) ends a program; Ctrl-D soft reset keeps the BLE link.
- `pibody.ble` (pibody_libs) gives student code links between boards:
  `advertise`, `scan`, `connect`, `send`, `receive`, `on_receive`.

### Files
- `cmodules/bleterm/`: BLE terminal C module (copy of picow-ble-ide `firmware/modules/bleterm`)
- `modules/_bleboot.py`: boot control (copy of picow-ble-ide); `modules/_bleboard.py`: PiBody defaults, BLE mode toggle, logo screen and top bar
- `modules/DisplayBase/__init__.py`: `display.bar`
- `modules/DisplayBase/sans*.py`: the fonts (renderer: `smooth_text()` in
  `cmodules/st7789/st7789.c`). Regenerate with `tools/makefont.swift` (macOS,
  CoreText) from Roboto TTFs (Apache-2.0, fonts.google.com/specimen/Roboto);
  `hN` picks the largest size whose lines fit N rows:
  `swift tools/makefont.swift Roboto-Regular.ttf h16 modules/DisplayBase/sans16.py`
  (h20 sans20, h24 sans24; Roboto-Bold.ttf h32 sans32b)
- `tools/screensim.py`: renders the BLE screens, the bar and pibody Display
  code to PNGs on the host (fake st7789, network), so the look can be checked
  without a board. Needs Python 3.10 or newer:
  `python3.13 tools/screensim.py [OUTDIR]`
- `cmodules/st7789/st7789.c`: DMA SPI layer (`spi_start`/`spi_wait`, hot
  functions in RAM), `fill_shapes` (fixed-point edge list, 4x4 anti-aliasing),
  `blit_framebuf`, `spi_clock()`; overlay clip (`_overlay`, `_overlay_bypass`, `_overlay_dirty`) keeping the bar on top
