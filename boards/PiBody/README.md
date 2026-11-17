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
  connected), board name, green dot while a program runs. The ST7789 driver
  drops drawing into those rows, so programs keep 240x320 coordinates.
  `display.bar.show()`, `.dot()` (one status dot in the corner), `.hide()`,
  `.retreat()` (hidden until the program ends), `.height`, `.mode`.
  `"display_status": false` in `/ble_config.json` turns screen and bar off.
- IDE Stop (or Ctrl-C) ends a program; Ctrl-D soft reset keeps the BLE link.
- `pibody.ble` (pibody_libs) gives student code links between boards:
  `advertise`, `scan`, `connect`, `send`, `receive`, `on_receive`.

### Files
- `cmodules/bleterm/`: BLE terminal C module (copy of picow-ble-ide `firmware/modules/bleterm`)
- `modules/_bleboot.py`: boot control (copy of picow-ble-ide); `modules/_bleboard.py`: PiBody defaults, BLE mode toggle, logo screen and top bar
- `modules/DisplayBase/__init__.py`: `display.bar`
- `cmodules/st7789/st7789.c`: overlay clip (`_overlay`, `_overlay_bypass`, `_overlay_dirty`) keeping the bar on top
