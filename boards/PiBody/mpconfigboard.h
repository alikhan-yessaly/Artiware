// Board and hardware specific configuration
#define MICROPY_HW_BOARD_NAME                   "Raspberry Pi Pico W"

// Filesystem size: set in mpconfigboard.cmake (MicroPython >= 1.29 checks it at link time).

// Enable networking.
#define MICROPY_PY_NETWORK 1
#define MICROPY_PY_NETWORK_HOSTNAME_DEFAULT     "PiBody Wireless"

// CYW43 driver configuration.
#define CYW43_USE_SPI (1)
#define CYW43_LWIP (1)
#define CYW43_GPIO (1)
#define CYW43_SPI_PIO (1)

// For debugging mbedtls - also set
// Debug level (0-4) 1=warning, 2=info, 3=debug, 4=verbose
// #define MODUSSL_MBEDTLS_DEBUG_LEVEL 1

#define MICROPY_HW_PIN_EXT_COUNT    CYW43_WL_GPIO_COUNT

// If this returns true for a pin then its irq will not be disabled on a soft reboot
int mp_hal_is_pin_reserved(int n);
#define MICROPY_HW_PIN_RESERVED(i) mp_hal_is_pin_reserved(i)

#define MICROPY_BANNER_MACHINE "PiBody v1.1 (BLE REPL) with RP2040"

// --- BLE REPL (bleterm) ---
// stdin ring shared by USB and the BLE terminal (raw-paste window is derived from it).
#define MICROPY_HW_STDIN_BUFFER_LEN             2048
#define MICROPY_PY_BLUETOOTH_DEFAULT_GAP_NAME   "PiBody"
#define MICROPY_BLETERM_NAME_PREFIX             "PiBody"

// BLE terminal: stdio hooks, system ATT services, stack kept up across soft reset.
#define MICROPY_PY_BLETERM                      (1)
#define MICROPY_BOARD_START_SOFT_RESET() \
    do { extern void bleterm_soft_reset_begin(void); bleterm_soft_reset_begin(); } while (0)
