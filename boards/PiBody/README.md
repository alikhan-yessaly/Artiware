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

Banner: `MicroPython v1.29.0-pibody-ble.v1.0.0 on <date>; PiBody v1.1 (BLE REPL) with RP2040`
(version set in `board.mk`, no MicroPython edit needed).

### Build
MicroPython **v1.29.0** with the patches in `patches/micropython-v1.29.0/`:
```bash
make mp-prepare        # once: branch artiware-v1.29.0 + patches, mpy-cross, submodules
make                   # build/PiBody/firmware.uf2
```

### Use
- Hold **BUTTON_LEFT** (GP20, high when pressed) at reset:
  - 1 s: safe mode (boot.py/main.py skipped), BLE on
  - 5 s: pairing window open for 60 s
- `/ble_config.json` holds the settings. It is written with the PiBody defaults on
  first boot; the IDE Settings dialog edits it. `"ble_on_boot": true` starts BLE
  on every boot.
- Display: while BLE is on, the screen shows the BLE status (searching,
  connected, locked, pairing, safe mode). The first `DisplayBase()` from user
  code takes the display over and the status screen stops.
  `"display_status": false` turns it off. Code that drives the ST7789 through
  its own `SPI(1)` (not `DisplayBase`) should set that, or the status screen
  can draw over it.

### Files
- `cmodules/bleterm/`: BLE terminal C module (copy of picow-ble-ide `firmware/modules/bleterm`)
- `modules/_bleboot.py`: boot control (copy of picow-ble-ide); `modules/_bleboard.py`: PiBody defaults and status screen
- `modules/DisplayBase/__init__.py`: `_on_claim` hook for the display hand-off
