// bleterm: C-level BLE terminal for the Pico W BLE IDE.
//
// This header is included by the bleterm module itself and by the patched
// upstream files (ports/rp2/mphalport.c, ports/rp2/main.c,
// ports/rp2/machine_pin_cyw43.c, extmod/btstack/modbluetooth_btstack.c).
// All bleterm state is static C memory; nothing here may allocate on the GC
// heap except the worker (see SPEC 5.2).
#ifndef BLETERM_H
#define BLETERM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BLETERM_VERSION_MAJOR 0
#define BLETERM_VERSION_MINOR 1
#define BLETERM_VERSION_PATCH 0

// ---- Hooks called from ports/rp2/mphalport.c (patch 0001) ----

// Extra poll flags (MP_STREAM_POLL_WR when the BLE terminal can take output).
uintptr_t bleterm_stdio_poll(uintptr_t poll_flags);
// Called after a byte was taken from stdin_ringbuf: releases a pending
// NUS write once there is room for it.
void bleterm_stdin_consumed(void);
// Copy stdout bytes to the BLE TX ring (discarded without a subscribed,
// unlocked IDE link). Never fails.
void bleterm_tx_strn(const char *str, size_t len);

// ---- Hooks called from extmod/btstack/modbluetooth_btstack.c (0002, 0004) ----

// True between bleterm.start() and bleterm.stop().
bool bleterm_is_started(void);
// True for the handles of the system block that bleterm serves (GATT
// Service Changed, NUS, PBL and their CCCDs). The GAP name (handle 3) is
// not included: it is served by bleterm only while no user state exists.
bool bleterm_att_owns_handle(uint16_t att_handle);
uint16_t bleterm_att_read(uint16_t con_handle, uint16_t att_handle, uint16_t offset, uint8_t *buffer, uint16_t buffer_size);
int bleterm_att_write(uint16_t con_handle, uint16_t att_handle, uint16_t transaction_mode, uint16_t offset, uint8_t *buffer, uint16_t buffer_size);
// Adds GAP, GATT, NUS and PBL right after att_db_util_init().
void bleterm_att_add_system_services(void);
// Called each time BTstack is (re)initialised (hci_init clears handlers).
void bleterm_on_stack_init(void);
// Name used for the GAP device name characteristic and the scan response.
const char *bleterm_name(size_t *len);
// User code took over / released the advertising set.
void bleterm_user_adv_begin(void);
void bleterm_user_adv_end(void);
// User BLE state was torn down while bleterm keeps the stack up.
void bleterm_user_state_destroyed(void);
// The ATT DB was rebuilt (user services registered or dropped).
void bleterm_db_changed(void);

// Provided by the patched modbluetooth_btstack.c for bleterm.
int mp_bluetooth_btstack_stack_up(void);
int mp_bluetooth_btstack_stack_down(void);
void mp_bluetooth_btstack_reset_db(void);
bool mp_bluetooth_btstack_user_state_present(void);

// ---- Hooks called from ports/rp2/main.c and the board (0005) ----

// MICROPY_BOARD_START_SOFT_RESET: VM is about to go down.
void bleterm_soft_reset_begin(void);
// After mp_init() and mp_bluetooth_hci_init(): VM is usable again.
void bleterm_vm_ready(void);
// True if this boot skips boot.py/main.py.
bool bleterm_safe_mode(void);

// ---- Hook called from ports/rp2/machine_pin_cyw43.c (0006) ----

// User code created or wrote Pin("LED"): bleterm stops driving the LED
// until the next soft reset.
void bleterm_led_release(void);

#endif // BLETERM_H
