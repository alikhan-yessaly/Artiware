// bleterm NUS terminal: RX -> stdin_ringbuf, stdout -> TX ring -> notifications.
#include <string.h>

#include "py/runtime.h"
#include "py/mphal.h"
#include "py/ringbuf.h"
#include "py/stream.h"
#include "shared/runtime/interrupt_char.h"
#include "lib/btstack/src/btstack.h"

#include "bleterm_int.h"

#define TX_RING_SIZE (2048)
#define TX_WAIT_MS (500)
#define NOTIFY_MAX (244) // MTU 247 - 3

static uint8_t tx_ring[TX_RING_SIZE];
static volatile uint16_t tx_head; // write index (main thread)
static volatile uint16_t tx_tail; // read index (BTstack context)
static btstack_context_callback_registration_t tx_reg;

static inline size_t tx_avail(void) {
    return (uint16_t)(tx_head - tx_tail) % TX_RING_SIZE;
}

static inline size_t tx_free(void) {
    return TX_RING_SIZE - 1 - tx_avail();
}

size_t bleterm_nus_tx_pending(void) {
    return tx_avail();
}

void bleterm_nus_reset(void) {
    tx_head = tx_tail = 0;
    bleterm_g.rx_pending = false;
}

// can-send-now for the NUS TX characteristic (BTstack context).
static void tx_pump(void *context) {
    (void)context;
    bleterm_link_t *link = bleterm_nus_target();
    if (link == NULL) {
        return;
    }
    size_t n = tx_avail();
    if (n == 0) {
        return;
    }
    size_t max = bleterm_mtu(link->handle) - 3;
    if (max > NOTIFY_MAX) {
        max = NOTIFY_MAX;
    }
    if (n > max) {
        n = max;
    }
    uint8_t buf[NOTIFY_MAX];
    uint16_t t = tx_tail;
    for (size_t i = 0; i < n; ++i) {
        buf[i] = tx_ring[t];
        t = (t + 1) % TX_RING_SIZE;
    }
    uint8_t err = att_server_notify(link->handle, bleterm_g.h_nus_tx, buf, n);
    if (err == ERROR_CODE_SUCCESS) {
        tx_tail = t;
    }
    if (tx_avail() > 0) {
        att_server_request_to_send_notification(&tx_reg, link->handle);
    }
}

void bleterm_nus_kick(void) {
    bleterm_link_t *link = bleterm_nus_target();
    if (link == NULL || tx_avail() == 0) {
        return;
    }
    tx_reg.callback = &tx_pump;
    tx_reg.context = NULL;
    // Idempotent: BTstack ignores a registration that is already queued.
    att_server_request_to_send_notification(&tx_reg, link->handle);
}

static bool in_scheduler_or_irq(void) {
    uint32_t ipsr;
    __asm volatile ("mrs %0, ipsr" : "=r" (ipsr));
    if ((ipsr & 0x1ff) != 0) {
        return true;
    }
    return MP_STATE_VM(sched_state) == MP_SCHED_LOCKED;
}

void bleterm_tx_strn(const char *str, size_t len) {
    if (!bleterm_g.started || bleterm_nus_target() == NULL) {
        // No subscribed, unlocked client: output is discarded.
        return;
    }
    bool may_wait = !bleterm_g.vm_down && !in_scheduler_or_irq();
    uint32_t t0 = mp_hal_ticks_ms();
    while (len > 0) {
        size_t n = tx_free();
        if (n > len) {
            n = len;
        }
        uint16_t h = tx_head;
        for (size_t i = 0; i < n; ++i) {
            tx_ring[h] = (uint8_t)str[i];
            h = (h + 1) % TX_RING_SIZE;
        }
        tx_head = h;
        str += n;
        len -= n;
        if (n > 0) {
            bleterm_nus_kick();
            t0 = mp_hal_ticks_ms(); // progress: the 500 ms is per stall, not per call
        }
        if (len == 0) {
            break;
        }
        // Ring full: wait for the pump, at most TX_WAIT_MS without progress, then drop the rest.
        if (!may_wait || bleterm_nus_target() == NULL
            || (mp_hal_ticks_ms() - t0) >= TX_WAIT_MS) {
            bleterm_g.tx_dropped += len;
            return;
        }
        mp_event_wait_ms(1);
    }
}

uintptr_t bleterm_stdio_poll(uintptr_t poll_flags) {
    if ((poll_flags & MP_STREAM_POLL_WR) && bleterm_nus_target() != NULL && tx_free() > 0) {
        return MP_STREAM_POLL_WR;
    }
    return 0;
}

static void push_byte(uint8_t c) {
    #if MICROPY_KBD_EXCEPTION
    if (c == mp_interrupt_char) {
        // As USB CDC: clear the ring and raise KeyboardInterrupt.
        stdin_ringbuf.iget = stdin_ringbuf.iput = 0;
        mp_sched_keyboard_interrupt();
        return;
    }
    #endif
    if (ringbuf_put(&stdin_ringbuf, c) < 0) {
        bleterm_g.rx_dropped++;
    }
}

// NUS RX write (BTstack context). Returns an ATT error code or 0.
int bleterm_nus_rx(bleterm_link_t *link, uint8_t *buffer, uint16_t size, bool is_request) {
    (void)link;
    if (is_request && ringbuf_free(&stdin_ringbuf) < size) {
        // Answer later, once the REPL has drained the ring. BTstack calls
        // this callback again with the same data at that point.
        bleterm_g.rx_pending = true;
        bleterm_g.rx_pending_con = link->handle;
        bleterm_g.rx_pending_len = size;
        return ATT_ERROR_WRITE_RESPONSE_PENDING;
    }
    if (bleterm_g.rx_pending && bleterm_g.rx_pending_con == link->handle) {
        bleterm_g.rx_pending = false;
    }
    for (uint16_t i = 0; i < size; ++i) {
        push_byte(buffer[i]);
    }
    return 0;
}

// Used by PBL RESET soft: inject bytes as if typed.
void bleterm_stdin_inject(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        push_byte(data[i]);
    }
}

static volatile bool stdin_waiting;

void bleterm_stdin_wait(bool waiting) {
    stdin_waiting = waiting;
}

bool bleterm_stdin_waiting(void) {
    return stdin_waiting;
}

void bleterm_stdin_consumed(void) {
    if (bleterm_g.rx_pending && ringbuf_free(&stdin_ringbuf) >= bleterm_g.rx_pending_len) {
        bleterm_schedule_worker();
    }
}

// Worker (scheduler context): release a pending write once it fits.
void bleterm_nus_worker(void) {
    if (bleterm_g.rx_pending && ringbuf_free(&stdin_ringbuf) >= bleterm_g.rx_pending_len) {
        bleterm_g.rx_pending = false;
        att_server_response_ready(bleterm_g.rx_pending_con);
    }
    bleterm_nus_kick();
}
