// bleterm PBL service: session, pairing, keys and file transfer (protocol.md 2).
//
// BTstack context only reassembles fragments into rxq and sends fragments
// from txq. Every message is handled in the worker (scheduler context), where
// VFS and Python calls are allowed, always inside nlr_push.
#include <string.h>

#include "py/runtime.h"
#include "py/mphal.h"
#include "py/gc.h"
#include "py/stream.h"
#include "py/objtuple.h"
#include "extmod/vfs.h"
#include "extmod/modmachine.h"
#include "lib/btstack/src/btstack.h"
#include "mbedtls/sha256.h"
#include "hardware/watchdog.h"
#include "hardware/sync.h"
#include "hardware/regs/addressmap.h"

#include "bleterm_int.h"
#include "pbl_codec.h"

// The open file or directory iterator of the current transfer.
MP_REGISTER_ROOT_POINTER(mp_obj_t bleterm_pbl_obj);

enum {
    ST_OK = 0, ST_EPROTO, ST_EAUTH, ST_EBUSY, ST_ENOENT, ST_EIO, ST_ECRC,
    ST_ENOSPC, ST_EINVAL, ST_ELOCKED, ST_EEXIST, ST_EABORT,
};

enum {
    T_HELLO = 0x01, T_AUTH = 0x02, T_STATUS = 0x03, T_RESET = 0x04, T_PAIR = 0x05,
    T_UNPAIR = 0x06, T_KEYS = 0x07,
    T_FS_LIST = 0x10, T_FS_STAT, T_FS_READ, T_FS_WRITE, T_FS_DATA, T_FS_END, T_FS_ABORT,
    T_FS_DELETE, T_FS_MKDIR, T_FS_RENAME,
    T_FS_ENTRIES = 0xD0, T_FS_CHUNK, T_FS_ACK, T_FS_DONE,
    T_EVT_BOOT = 0xE0, T_EVT_STATE,
};

#define FS_WINDOW (4)
#define MAX_WRONG_PROOFS (5)
#define LOCKOUT_MS (30000)
#define NUM_KEYS (4)
#define KEY_NAME_MAX (16)
#define KEYS_PATH "/ble_keys.bin"

// ---------------------------------------------------------------------------
// Message queues: byte rings of [len u16][con u16][message].

#define Q_SIZE (2048)
typedef struct {
    uint8_t buf[Q_SIZE];
    uint16_t head;
    uint16_t tail;
} msgq_t;

static msgq_t rxq, txq;

static size_t q_used(const msgq_t *q) {
    return (uint16_t)(q->head - q->tail) % Q_SIZE;
}

static size_t q_free(const msgq_t *q) {
    return Q_SIZE - 1 - q_used(q);
}

static void q_put(msgq_t *q, const uint8_t *d, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        q->buf[q->head] = d[i];
        q->head = (q->head + 1) % Q_SIZE;
    }
}

static uint8_t q_at(const msgq_t *q, size_t i) {
    return q->buf[(q->tail + i) % Q_SIZE];
}

static bool q_push(msgq_t *q, uint16_t con, const uint8_t *msg, uint16_t len) {
    if (q_free(q) < (size_t)len + 4) {
        return false;
    }
    uint8_t hdr[4] = {len & 0xff, len >> 8, con & 0xff, con >> 8};
    q_put(q, hdr, 4);
    q_put(q, msg, len);
    return true;
}

static int q_pop(msgq_t *q, uint16_t *con, uint8_t *out) {
    if (q_used(q) < 4) {
        return -1;
    }
    uint16_t len = q_at(q, 0) | (q_at(q, 1) << 8);
    *con = q_at(q, 2) | (q_at(q, 3) << 8);
    for (size_t i = 0; i < len; ++i) {
        out[i] = q_at(q, 4 + i);
    }
    q->tail = (q->tail + 4 + len) % Q_SIZE;
    return len;
}

static void q_reset(msgq_t *q) {
    q->head = q->tail = 0;
}

// ---------------------------------------------------------------------------
// State

static pbl_rx_t rx;
static uint16_t rx_con = BLETERM_CON_NONE;
static bool rx_pending;
static uint16_t rx_pending_con;

static uint8_t tx_msg[PBL_MSG_MAX];
static pbl_tx_t tx;
static bool tx_active;
static uint16_t tx_con;
static btstack_context_callback_registration_t tx_reg;

static uint8_t req[PBL_MSG_MAX];  // message being handled
static uint8_t out[PBL_MSG_MAX];  // message being built

typedef struct {
    uint8_t id[4];
    uint8_t key[32];
    char name[KEY_NAME_MAX];
} key_rec_t;

static key_rec_t keys[NUM_KEYS];
static uint8_t nkeys;
static bool keys_loaded;

static uint8_t wrong_proofs;
static uint32_t lock_until_ms;    // 0 = not locked

enum { X_NONE, X_LIST, X_READ, X_WRITE };
static struct {
    uint8_t kind;
    uint8_t seq;
    uint16_t con;
    uint32_t size;
    uint32_t off;
    uint32_t crc;
    bool abort_pending;           // link dropped: close in the worker
    char path[256];
} xfer;
static uint8_t held_entry[262];   // directory entry that did not fit
static uint16_t held_len;

static bool boot_evt_pending;
static uint16_t evt_con = BLETERM_CON_NONE;
static uint8_t evt_flags;

static uint8_t reset_mode;        // 0 none, 1 hard, 2 bootloader
static uint8_t reset_stage;
static uint32_t reset_t;
static uint16_t disconnect_con = BLETERM_CON_NONE; // drop after the reply left
static uint32_t disconnect_t;

// ---------------------------------------------------------------------------
// TX pump (BTstack context)

static void tx_pump(void *context) {
    (void)context;
    for (;;) {
        if (!tx_active) {
            int n = q_pop(&txq, &tx_con, tx_msg);
            if (n < 0) {
                return;
            }
            pbl_tx_begin(&tx, tx_msg, (uint16_t)n);
            tx_active = true;
            bleterm_schedule_worker(); // room in txq for more stream data
        }
        bleterm_link_t *link = bleterm_link_find(tx_con);
        if (link == NULL || !(link->cccd_pbl & 1)) {
            tx_active = false; // nobody to send to: drop
            continue;
        }
        size_t max = bleterm_mtu(tx_con) - 3;
        if (max > 244) {
            max = 244;
        }
        pbl_tx_t save = tx;
        uint8_t frag[244];
        size_t m = pbl_tx_next(&tx, frag, max);
        if (m == 0) {
            tx_active = false;
            continue;
        }
        if (att_server_notify(tx_con, bleterm_g.h_pbl_tx, frag, m) != ERROR_CODE_SUCCESS) {
            tx = save;
            tx_reg.callback = &tx_pump;
            att_server_request_to_send_notification(&tx_reg, tx_con);
            return;
        }
        if (tx.off >= tx.len) {
            tx_active = false;
        }
        // Keep sending until the stack has no buffers left.
    }
}

static bool send_msg(uint16_t con, uint8_t type, uint8_t seq, const uint8_t *payload, size_t n) {
    size_t len = pbl_msg_encode(out, type, seq, payload, n);
    if (len == 0 || !q_push(&txq, con, out, (uint16_t)len)) {
        return false;
    }
    tx_pump(NULL);
    return true;
}

static void respond(uint16_t con, uint8_t type, uint8_t seq, uint8_t status, const uint8_t *payload, size_t n) {
    uint8_t p[PBL_MSG_MAX - 2];
    p[0] = status;
    if (n > sizeof(p) - 1) {
        n = sizeof(p) - 1;
    }
    if (n) {
        memcpy(p + 1, payload, n);
    }
    send_msg(con, type | 0x80, seq, p, n + 1);
}

static bool tx_idle(void) {
    return !tx_active && q_used(&txq) == 0;
}

// A reply queued at t0 has had time to go over the air: tx_idle() only means BTstack
// took it, so wait a few connection intervals more before dropping the link.
static bool reply_flushed(uint32_t now, uint32_t t0) {
    uint32_t dt = now - t0;
    return (tx_idle() && dt >= 150) || dt > 600;
}

// ---------------------------------------------------------------------------
// RX (BTstack context)

int bleterm_pbl_rx(bleterm_link_t *link, const uint8_t *data, uint16_t len, bool is_request) {
    if (len == 0) {
        return 0;
    }
    if (rx_con != link->handle) {
        pbl_rx_reset(&rx);
        rx_con = link->handle;
    }
    if (q_free(&rxq) < (size_t)rx.len + len + 4) {
        if (is_request) {
            // Answered once the worker has drained rxq.
            rx_pending = true;
            rx_pending_con = link->handle;
            bleterm_schedule_worker();
            return ATT_ERROR_WRITE_RESPONSE_PENDING;
        }
        pbl_rx_reset(&rx); // dropped; FS_DATA is resent by the host
        return 0;
    }
    if (pbl_rx_fragment(&rx, data, len) == PBL_RX_DONE) {
        if (rx.len >= 2) {
            q_push(&rxq, link->handle, rx.buf, rx.len);
        }
        bleterm_schedule_worker();
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Flags, keys

uint8_t bleterm_pbl_flags(const bleterm_link_t *link) {
    uint8_t f = 0;
    f |= bleterm_g.safe_mode ? 0x01 : 0;
    f |= bleterm_g.require_pairing ? 0x02 : 0;
    f |= (!bleterm_g.require_pairing || (link != NULL && link->auth)) ? 0x04 : 0;
    f |= mp_bluetooth_btstack_user_state_present() ? 0x08 : 0;
    f |= xfer.kind != X_NONE ? 0x10 : 0;
    f |= bleterm_g.pairing_until_ms != 0 ? 0x20 : 0;
    f |= nkeys > 0 ? 0x40 : 0;
    return f;
}

static mp_obj_t open_file(const char *path, const char *mode) {
    mp_obj_t args[2] = {mp_obj_new_str_from_cstr(path), mp_obj_new_str_from_cstr(mode)};
    return mp_vfs_open(2, args, (mp_map_t *)&mp_const_empty_map);
}

static void keys_load(void) {
    keys_loaded = true;
    nkeys = 0;
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_obj_t f = open_file(KEYS_PATH, "rb");
        int err;
        mp_uint_t n = mp_stream_rw(f, keys, sizeof(keys), &err, MP_STREAM_RW_READ);
        mp_stream_close(f);
        nlr_pop();
        if (n != MP_STREAM_ERROR) {
            nkeys = n / sizeof(key_rec_t);
        }
    }
}

static bool keys_save(void) {
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_obj_t f = open_file(KEYS_PATH, "wb");
        int err;
        mp_uint_t n = mp_stream_rw(f, keys, nkeys * sizeof(key_rec_t), &err, MP_STREAM_RW_WRITE);
        mp_stream_close(f);
        nlr_pop();
        return n == nkeys * sizeof(key_rec_t);
    }
    return false;
}

static int key_find(const uint8_t *id) {
    for (int i = 0; i < nkeys; ++i) {
        if (memcmp(keys[i].id, id, 4) == 0) {
            return i;
        }
    }
    return -1;
}

static void key_remove_at(int i) {
    memmove(&keys[i], &keys[i + 1], (nkeys - i - 1) * sizeof(key_rec_t));
    --nkeys;
    memset(&keys[nkeys], 0, sizeof(key_rec_t));
}

// Adds (or replaces) a record; a fifth record replaces the oldest.
static void key_add(const uint8_t *id, const uint8_t *key, const char *name, size_t name_len) {
    int i = key_find(id);
    if (i >= 0) {
        key_remove_at(i);
    }
    if (nkeys == NUM_KEYS) {
        key_remove_at(0);
    }
    key_rec_t *r = &keys[nkeys++];
    memcpy(r->id, id, 4);
    memcpy(r->key, key, 32);
    memset(r->name, 0, sizeof(r->name));
    memcpy(r->name, name, name_len > KEY_NAME_MAX ? KEY_NAME_MAX : name_len);
}

static size_t key_name_len(const key_rec_t *r) {
    size_t n = 0;
    while (n < KEY_NAME_MAX && r->name[n]) {
        ++n;
    }
    return n;
}

void bleterm_pbl_on_start(void) {
    if (!keys_loaded) {
        keys_load();
    }
}

// ---------------------------------------------------------------------------
// Transfer helpers (worker)

static uint8_t errno_status(mp_obj_t exc) {
    if (mp_obj_is_subclass_fast(MP_OBJ_FROM_PTR(mp_obj_get_type(exc)), MP_OBJ_FROM_PTR(&mp_type_OSError))) {
        mp_obj_t v = mp_obj_exception_get_value(exc);
        if (mp_obj_is_small_int(v)) {
            switch (MP_OBJ_SMALL_INT_VALUE(v)) {
                case MP_ENOENT:
                    return ST_ENOENT;
                case MP_EEXIST:
                    return ST_EEXIST;
                case MP_ENOSPC:
                    return ST_ENOSPC;
                case MP_EINVAL:
                    return ST_EINVAL;
                default:
                    break;
            }
        }
    }
    return ST_EIO;
}

static bool path_ok(const uint8_t *p, size_t n) {
    if (n == 0 || n > 255 || p[0] != '/') {
        return false;
    }
    return memchr(p, 0, n) == NULL;
}

static void xfer_close(bool remove_tmp) {
    mp_obj_t obj = MP_STATE_PORT(bleterm_pbl_obj);
    MP_STATE_PORT(bleterm_pbl_obj) = MP_OBJ_NULL;
    uint8_t kind = xfer.kind;
    xfer.kind = X_NONE;
    held_len = 0;
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        if (obj != MP_OBJ_NULL && kind != X_LIST) {
            mp_stream_close(obj);
        }
        if (kind == X_WRITE && remove_tmp) {
            char tmp[262];
            strcpy(tmp, xfer.path);
            strcat(tmp, ".pbl~");
            mp_vfs_remove(mp_obj_new_str_from_cstr(tmp));
        }
        nlr_pop();
    }
}

static void xfer_abort_notify(uint8_t status) {
    uint16_t con = xfer.con;
    uint8_t seq = xfer.seq;
    bool was_active = xfer.kind != X_NONE;
    xfer_close(true);
    if (was_active && bleterm_link_find(con) != NULL) {
        uint8_t p[5] = {status, 0, 0, 0, 0};
        send_msg(con, T_FS_DONE, seq, p, 5);
    }
}

// One FS_ENTRIES message, or FS_DONE at the end.
static void list_step(void) {
    uint8_t p[PBL_MSG_MAX - 2];
    size_t n = 0;
    if (held_len) {
        memcpy(p, held_entry, held_len);
        n = held_len;
        held_len = 0;
    }
    bool done = false;
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        for (;;) {
            mp_obj_t item = mp_iternext(MP_STATE_PORT(bleterm_pbl_obj));
            if (item == MP_OBJ_STOP_ITERATION) {
                done = true;
                break;
            }
            size_t len;
            mp_obj_t *items;
            mp_obj_tuple_get(item, &len, &items);
            size_t name_len;
            const char *name = mp_obj_str_get_data(items[0], &name_len);
            if (name_len > 255) {
                continue;
            }
            mp_int_t type = mp_obj_get_int(items[1]);
            uint32_t size = len >= 4 ? (uint32_t)mp_obj_get_int(items[3]) : 0;
            uint8_t *e = held_entry;
            e[0] = (type & 0x4000) ? 1 : 0;
            pbl_put_u32(e + 1, e[0] ? 0 : size);
            e[5] = (uint8_t)name_len;
            memcpy(e + 6, name, name_len);
            size_t elen = 6 + name_len;
            if (n + elen > sizeof(p)) {
                held_len = elen;
                break;
            }
            memcpy(p + n, e, elen);
            n += elen;
        }
        nlr_pop();
    } else {
        xfer_abort_notify(errno_status(MP_OBJ_FROM_PTR(nlr.ret_val)));
        return;
    }
    if (n) {
        send_msg(xfer.con, T_FS_ENTRIES, xfer.seq, p, n);
    }
    if (done) {
        uint16_t con = xfer.con;
        uint8_t seq = xfer.seq;
        xfer_close(false);
        uint8_t d[5] = {ST_OK, 0, 0, 0, 0};
        send_msg(con, T_FS_DONE, seq, d, 5);
    }
}

// One FS_CHUNK, or FS_DONE at the end.
static void read_step(void) {
    size_t chunk = bleterm_mtu(xfer.con) - 10;
    if (chunk > PBL_MSG_MAX - 6) {
        chunk = PBL_MSG_MAX - 6;
    }
    uint8_t p[PBL_MSG_MAX - 2];
    int err = 0;
    mp_uint_t n = 0;
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        n = mp_stream_rw(MP_STATE_PORT(bleterm_pbl_obj), p + 4, chunk, &err, MP_STREAM_RW_READ | MP_STREAM_RW_ONCE);
        nlr_pop();
    } else {
        xfer_abort_notify(ST_EIO);
        return;
    }
    if (n == MP_STREAM_ERROR) {
        xfer_abort_notify(ST_EIO);
        return;
    }
    if (n == 0) {
        uint16_t con = xfer.con;
        uint8_t seq = xfer.seq;
        uint32_t crc = xfer.crc;
        xfer_close(false);
        uint8_t d[5] = {ST_OK};
        pbl_put_u32(d + 1, crc);
        send_msg(con, T_FS_DONE, seq, d, 5);
        return;
    }
    pbl_put_u32(p, xfer.off);
    xfer.crc = pbl_crc32(xfer.crc, p + 4, n);
    xfer.off += n;
    send_msg(xfer.con, T_FS_CHUNK, xfer.seq, p, 4 + n);
}

// ---------------------------------------------------------------------------
// Request handlers (worker)

static bool authorised(bleterm_link_t *link, uint8_t *status) {
    bleterm_link_t *ide = bleterm_g.ide >= 0 ? &bleterm_g.links[(int)bleterm_g.ide] : NULL;
    if (!bleterm_g.require_pairing) {
        if (ide == NULL) {
            bleterm_become_ide(link);
            return true;
        }
        if (ide == link) {
            return true;
        }
        *status = ST_EBUSY;
        return false;
    }
    if (link->auth && ide == link) {
        return true;
    }
    *status = ST_EAUTH;
    return false;
}

static void sha256(const uint8_t *a, size_t an, const uint8_t *b, size_t bn, uint8_t *digest) {
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, a, an);
    mbedtls_sha256_update(&ctx, b, bn);
    mbedtls_sha256_finish(&ctx, digest);
    mbedtls_sha256_free(&ctx);
}

static void set_on_boot(void) {
    // Python helper in the frozen _bleboot module keeps other config keys.
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_obj_t mod = mp_import_name(MP_QSTR__bleboot, mp_const_none, MP_OBJ_NEW_SMALL_INT(0));
        mp_call_function_0(mp_load_attr(mod, MP_QSTR_set_on_boot));
        nlr_pop();
    }
}

static void handle_session(bleterm_link_t *link, uint8_t type, uint8_t seq, const uint8_t *p, size_t n) {
    uint16_t con = link->handle;
    uint8_t st = ST_OK;
    uint8_t r[64];
    switch (type) {
        case T_HELLO: {
            if (n < 1 || p[0] != 1) {
                respond(con, type, seq, ST_EPROTO, NULL, 0);
                return;
            }
            if (!link->nonce_valid) {
                mp_hal_get_random(8, link->nonce);
                link->nonce_valid = true;
            }
            r[0] = 1;
            r[1] = bleterm_pbl_flags(link);
            pbl_put_u16(r + 2, bleterm_mtu(con));
            r[4] = BLETERM_VERSION_MAJOR;
            r[5] = BLETERM_VERSION_MINOR;
            r[6] = BLETERM_VERSION_PATCH;
            memcpy(r + 7, link->nonce, 8);
            respond(con, type, seq, ST_OK, r, 15);
            return;
        }
        case T_AUTH: {
            uint32_t now = mp_hal_ticks_ms();
            if (lock_until_ms != 0) {
                int32_t left = (int32_t)(lock_until_ms - now);
                if (left > 0) {
                    r[0] = (uint8_t)((left + 999) / 1000);
                    respond(con, type, seq, ST_ELOCKED, r, 1);
                    return;
                }
                lock_until_ms = 0;
            }
            if (n != 20) {
                respond(con, type, seq, ST_EPROTO, NULL, 0);
                return;
            }
            int k = key_find(p);
            bool good = false;
            if (k >= 0 && link->nonce_valid) {
                uint8_t d[32];
                sha256(keys[k].key, 32, link->nonce, 8, d);
                uint8_t diff = 0;
                for (int i = 0; i < 16; ++i) {
                    diff |= d[i] ^ p[4 + i];
                }
                good = diff == 0;
            }
            if (!good) {
                if (++wrong_proofs >= MAX_WRONG_PROOFS) {
                    wrong_proofs = 0;
                    lock_until_ms = (now + LOCKOUT_MS) | 1;
                    r[0] = LOCKOUT_MS / 1000;
                    respond(con, type, seq, ST_ELOCKED, r, 1);
                    disconnect_con = con;
                    disconnect_t = now;
                } else {
                    respond(con, type, seq, ST_EAUTH, NULL, 0);
                }
                return;
            }
            if (bleterm_g.ide >= 0 && &bleterm_g.links[(int)bleterm_g.ide] != link) {
                respond(con, type, seq, ST_EBUSY, NULL, 0);
                return;
            }
            wrong_proofs = 0;
            bleterm_become_ide(link);
            respond(con, type, seq, ST_OK, NULL, 0);
            return;
        }
        case T_PAIR: {
            if (bleterm_g.pairing_until_ms == 0) {
                respond(con, type, seq, ST_EAUTH, NULL, 0);
                return;
            }
            if (n > KEY_NAME_MAX) {
                respond(con, type, seq, ST_EINVAL, NULL, 0);
                return;
            }
            if (bleterm_g.ide >= 0 && &bleterm_g.links[(int)bleterm_g.ide] != link) {
                respond(con, type, seq, ST_EBUSY, NULL, 0);
                return;
            }
            uint8_t id[4], key[32];
            do {
                mp_hal_get_random(4, id);
            } while (key_find(id) >= 0 || memcmp(id, "\xff\xff\xff\xff", 4) == 0);
            mp_hal_get_random(32, key);
            key_add(id, key, (const char *)p, n);
            if (!keys_save()) {
                respond(con, type, seq, ST_EIO, NULL, 0);
                return;
            }
            set_on_boot();
            bleterm_g.pairing_until_ms = 0;
            bleterm_become_ide(link);
            memcpy(r, id, 4);
            memcpy(r + 4, key, 32);
            respond(con, type, seq, ST_OK, r, 36);
            return;
        }
        default:
            break;
    }
    if (!authorised(link, &st)) {
        respond(con, type, seq, st, NULL, 0);
        return;
    }
    switch (type) {
        case T_UNPAIR: {
            if (n != 4) {
                respond(con, type, seq, ST_EPROTO, NULL, 0);
                return;
            }
            if (memcmp(p, "\xff\xff\xff\xff", 4) == 0) {
                nkeys = 0;
                memset(keys, 0, sizeof(keys));
            } else {
                int k = key_find(p);
                if (k < 0) {
                    respond(con, type, seq, ST_ENOENT, NULL, 0);
                    return;
                }
                key_remove_at(k);
            }
            respond(con, type, seq, keys_save() ? ST_OK : ST_EIO, NULL, 0);
            return;
        }
        case T_KEYS: {
            uint8_t b[1 + NUM_KEYS * (5 + KEY_NAME_MAX)];
            size_t m = 0;
            for (int i = 0; i < nkeys; ++i) {
                size_t nl = key_name_len(&keys[i]);
                memcpy(b + m, keys[i].id, 4);
                b[m + 4] = (uint8_t)nl;
                memcpy(b + m + 5, keys[i].name, nl);
                m += 5 + nl;
            }
            respond(con, type, seq, ST_OK, b, m);
            return;
        }
        case T_STATUS: {
            gc_info_t info;
            gc_info(&info);
            uint32_t fs_kb = 0;
            nlr_buf_t nlr;
            if (nlr_push(&nlr) == 0) {
                mp_obj_t t = mp_vfs_statvfs(MP_OBJ_NEW_QSTR(MP_QSTR__slash_));
                size_t len;
                mp_obj_t *it;
                mp_obj_tuple_get(t, &len, &it);
                uint64_t frsize = mp_obj_get_int(it[1]);
                uint64_t bavail = mp_obj_get_int(it[4]);
                fs_kb = (uint32_t)(frsize * bavail / 1024);
                nlr_pop();
            }
            r[0] = bleterm_pbl_flags(link);
            pbl_put_u16(r + 1, bleterm_mtu(con));
            pbl_put_u32(r + 3, info.free);
            pbl_put_u32(r + 7, info.used);
            pbl_put_u32(r + 11, fs_kb);
            pbl_put_u32(r + 15, mp_hal_ticks_ms());
            respond(con, type, seq, ST_OK, r, 19);
            return;
        }
        case T_RESET: {
            if (n != 1 || p[0] > 2) {
                respond(con, type, seq, ST_EINVAL, NULL, 0);
                return;
            }
            respond(con, type, seq, ST_OK, NULL, 0);
            if (p[0] == 0) {
                static const uint8_t keys_soft[4] = {0x03, 0x03, 0x02, 0x04};
                bleterm_stdin_inject(keys_soft, 4);
            } else {
                reset_mode = p[0];
                reset_stage = 1;
                reset_t = mp_hal_ticks_ms();
                // machine.reset() keeps RAM: the IDE reconnects after a hard reset.
                bleterm_g.keep_session = reset_mode == 1;
            }
            return;
        }
        default:
            respond(con, type, seq, ST_EPROTO, NULL, 0);
            return;
    }
}

static mp_obj_t path_obj(const uint8_t *p, size_t n) {
    return mp_obj_new_str((const char *)p, n);
}

static void handle_fs(bleterm_link_t *link, uint8_t type, uint8_t seq, const uint8_t *p, size_t n) {
    uint16_t con = link->handle;
    uint8_t st = ST_OK;
    uint8_t r[16];
    if (type == T_FS_DATA) {
        // No response; FS_ACK with the next expected offset.
        if (xfer.kind != X_WRITE || xfer.con != con || n < 4 || !authorised(link, &st)) {
            return;
        }
        uint32_t off = pbl_get_u32(p);
        size_t len = n - 4;
        if (off == xfer.off && xfer.off + len <= xfer.size && len > 0) {
            int err = 0;
            mp_uint_t w = MP_STREAM_ERROR;
            nlr_buf_t nlr;
            if (nlr_push(&nlr) == 0) {
                w = mp_stream_rw(MP_STATE_PORT(bleterm_pbl_obj), (void *)(p + 4), len, &err, MP_STREAM_RW_WRITE);
                nlr_pop();
            }
            if (w != len) {
                xfer_abort_notify(err == MP_ENOSPC ? ST_ENOSPC : ST_EIO);
                return;
            }
            xfer.crc = pbl_crc32(xfer.crc, p + 4, len);
            xfer.off += len;
        }
        pbl_put_u32(r, xfer.off);
        send_msg(con, T_FS_ACK, xfer.seq, r, 4);
        return;
    }
    if (!authorised(link, &st)) {
        respond(con, type, seq, st, NULL, 0);
        return;
    }
    if (type == T_FS_ABORT) {
        if (xfer.kind != X_NONE && xfer.con == con) {
            xfer_close(true);
        }
        respond(con, type, seq, ST_OK, NULL, 0);
        return;
    }
    if (type == T_FS_END) {
        if (xfer.kind != X_WRITE || xfer.con != con || n != 4) {
            respond(con, type, seq, ST_EINVAL, NULL, 0);
            return;
        }
        bool ok = xfer.off == xfer.size && pbl_get_u32(p) == xfer.crc;
        if (!ok) {
            xfer_close(true);
            respond(con, type, seq, ST_ECRC, NULL, 0);
            return;
        }
        // Close, then rename the temporary file over the target.
        mp_obj_t f = MP_STATE_PORT(bleterm_pbl_obj);
        MP_STATE_PORT(bleterm_pbl_obj) = MP_OBJ_NULL;
        xfer.kind = X_NONE;
        char tmp[262];
        strcpy(tmp, xfer.path);
        strcat(tmp, ".pbl~");
        nlr_buf_t nlr;
        if (nlr_push(&nlr) == 0) {
            mp_stream_close(f);
            mp_obj_t t = mp_obj_new_str_from_cstr(tmp);
            mp_obj_t d = mp_obj_new_str_from_cstr(xfer.path);
            nlr_buf_t nlr2;
            if (nlr_push(&nlr2) == 0) {
                mp_vfs_rename(t, d);
                nlr_pop();
            } else {
                // Some filesystems do not rename over an existing file.
                mp_vfs_remove(d);
                mp_vfs_rename(t, d);
            }
            nlr_pop();
            respond(con, type, seq, ST_OK, NULL, 0);
        } else {
            uint8_t s = errno_status(MP_OBJ_FROM_PTR(nlr.ret_val));
            nlr_buf_t nlr3;
            if (nlr_push(&nlr3) == 0) {
                mp_vfs_remove(mp_obj_new_str_from_cstr(tmp));
                nlr_pop();
            }
            respond(con, type, seq, s, NULL, 0);
        }
        return;
    }

    // Requests with paths.
    const uint8_t *path = p;
    size_t path_len = n;
    if (type == T_FS_WRITE) {
        if (n < 4) {
            respond(con, type, seq, ST_EPROTO, NULL, 0);
            return;
        }
        path = p + 4;
        path_len = n - 4;
        if (path_len > 250) {
            respond(con, type, seq, ST_EINVAL, NULL, 0);
            return;
        }
    } else if (type == T_FS_RENAME) {
        if (n < 1 || (size_t)p[0] + 1 > n) {
            respond(con, type, seq, ST_EPROTO, NULL, 0);
            return;
        }
        path = p + 1;
        path_len = p[0];
        if (!path_ok(p + 1 + p[0], n - 1 - p[0])) {
            respond(con, type, seq, ST_EINVAL, NULL, 0);
            return;
        }
    }
    if (!path_ok(path, path_len)) {
        respond(con, type, seq, ST_EINVAL, NULL, 0);
        return;
    }
    bool starts_transfer = type == T_FS_LIST || type == T_FS_READ || type == T_FS_WRITE;
    if (starts_transfer && xfer.kind != X_NONE) {
        respond(con, type, seq, ST_EBUSY, NULL, 0);
        return;
    }

    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_obj_t po = path_obj(path, path_len);
        size_t rn = 0;
        switch (type) {
            case T_FS_LIST: {
                mp_obj_t it = mp_vfs_ilistdir(1, &po);
                MP_STATE_PORT(bleterm_pbl_obj) = it;
                xfer.kind = X_LIST;
                break;
            }
            case T_FS_STAT: {
                mp_obj_t t = mp_vfs_stat(po);
                size_t len;
                mp_obj_t *it;
                mp_obj_tuple_get(t, &len, &it);
                mp_int_t mode = mp_obj_get_int(it[0]);
                r[0] = (mode & 0x4000) ? 1 : 0;
                pbl_put_u32(r + 1, (uint32_t)mp_obj_get_int_truncated(it[6]));
                pbl_put_u32(r + 5, (uint32_t)mp_obj_get_int_truncated(it[8]));
                rn = 9;
                break;
            }
            case T_FS_READ: {
                mp_obj_t t = mp_vfs_stat(po);
                size_t len;
                mp_obj_t *it;
                mp_obj_tuple_get(t, &len, &it);
                if (mp_obj_get_int(it[0]) & 0x4000) {
                    mp_raise_OSError(MP_EINVAL);
                }
                uint32_t size = (uint32_t)mp_obj_get_int_truncated(it[6]);
                mp_obj_t args[2] = {po, MP_OBJ_NEW_QSTR(MP_QSTR_rb)};
                MP_STATE_PORT(bleterm_pbl_obj) = mp_vfs_open(2, args, (mp_map_t *)&mp_const_empty_map);
                xfer.kind = X_READ;
                xfer.size = size;
                pbl_put_u32(r, size);
                rn = 4;
                break;
            }
            case T_FS_WRITE: {
                char tmp[262];
                memcpy(tmp, path, path_len);
                memcpy(tmp + path_len, ".pbl~", 6);
                mp_obj_t args[2] = {mp_obj_new_str_from_cstr(tmp), MP_OBJ_NEW_QSTR(MP_QSTR_wb)};
                MP_STATE_PORT(bleterm_pbl_obj) = mp_vfs_open(2, args, (mp_map_t *)&mp_const_empty_map);
                xfer.kind = X_WRITE;
                xfer.size = pbl_get_u32(p);
                uint16_t chunk = bleterm_mtu(con) - 10;
                r[0] = FS_WINDOW;
                pbl_put_u16(r + 1, chunk);
                rn = 3;
                break;
            }
            case T_FS_DELETE: {
                mp_obj_t t = mp_vfs_stat(po);
                size_t len;
                mp_obj_t *it;
                mp_obj_tuple_get(t, &len, &it);
                if (mp_obj_get_int(it[0]) & 0x4000) {
                    mp_vfs_rmdir(po);
                } else {
                    mp_vfs_remove(po);
                }
                break;
            }
            case T_FS_MKDIR:
                mp_vfs_mkdir(po);
                break;
            case T_FS_RENAME:
                mp_vfs_rename(po, path_obj(p + 1 + p[0], n - 1 - p[0]));
                break;
            default:
                nlr_pop();
                respond(con, type, seq, ST_EPROTO, NULL, 0);
                return;
        }
        nlr_pop();
        if (starts_transfer) {
            xfer.seq = seq;
            xfer.con = con;
            xfer.off = 0;
            xfer.crc = 0;
            xfer.abort_pending = false;
            held_len = 0;
            memcpy(xfer.path, path, path_len);
            xfer.path[path_len] = 0;
        }
        respond(con, type, seq, ST_OK, r, rn);
    } else {
        if (starts_transfer) {
            // Only a transfer this request opened; never one already running.
            if (MP_STATE_PORT(bleterm_pbl_obj) != MP_OBJ_NULL) {
                xfer_close(false);
            }
            xfer.kind = X_NONE;
        }
        respond(con, type, seq, errno_status(MP_OBJ_FROM_PTR(nlr.ret_val)), NULL, 0);
    }
}

static void handle(uint16_t con, const uint8_t *msg, size_t len) {
    bleterm_link_t *link = bleterm_link_find(con);
    if (link == NULL || len < 2) {
        return;
    }
    uint8_t type = msg[0];
    uint8_t seq = msg[1];
    const uint8_t *p = msg + 2;
    size_t n = len - 2;
    if (type >= 0x10 && type <= 0x1f) {
        handle_fs(link, type, seq, p, n);
    } else if (type >= 0x01 && type <= 0x5f) {
        if (bleterm_g.require_pairing && !link->auth
            && type != T_HELLO && type != T_AUTH && type != T_PAIR) {
            respond(con, type, seq, ST_EAUTH, NULL, 0);
            return;
        }
        handle_session(link, type, seq, p, n);
    }
}

// ---------------------------------------------------------------------------
// Worker, events, lifecycle

void bleterm_pbl_worker(void) {
    if (xfer.abort_pending) {
        xfer.abort_pending = false;
        xfer_close(true);
    }

    // Requests: keep room in txq for the largest response.
    uint16_t con;
    while (q_free(&txq) >= PBL_MSG_MAX + 8) {
        int n = q_pop(&rxq, &con, req);
        if (n < 0) {
            break;
        }
        handle(con, req, n);
    }
    if (rx_pending && q_free(&rxq) >= PBL_MSG_MAX + 4) {
        rx_pending = false;
        att_server_response_ready(rx_pending_con);
    }

    // Streams, paced by free space in txq.
    for (int i = 0; i < 8 && xfer.kind != X_NONE && q_free(&txq) >= PBL_MSG_MAX + 8; ++i) {
        if (xfer.kind == X_LIST) {
            list_step();
        } else if (xfer.kind == X_READ) {
            read_step();
        } else {
            break;
        }
    }

    // Events to the IDE link.
    bleterm_link_t *ide = bleterm_g.ide >= 0 ? &bleterm_g.links[(int)bleterm_g.ide] : NULL;
    if (ide != NULL && (ide->cccd_pbl & 1)) {
        uint8_t f = bleterm_pbl_flags(ide);
        if (boot_evt_pending) {
            boot_evt_pending = false;
            send_msg(ide->handle, T_EVT_BOOT, 0, &f, 1);
            evt_con = ide->handle;
            evt_flags = f;
        } else if (evt_con != ide->handle) {
            evt_con = ide->handle;
            evt_flags = f;
        } else if (f != evt_flags) {
            evt_flags = f;
            send_msg(ide->handle, T_EVT_STATE, 0, &f, 1);
        }
    } else {
        boot_evt_pending = false;
    }

    uint32_t now = mp_hal_ticks_ms();
    if (disconnect_con != BLETERM_CON_NONE && reply_flushed(now, disconnect_t)) {
        gap_disconnect(disconnect_con);
        disconnect_con = BLETERM_CON_NONE;
    }

    // RESET hard/bootloader: reply, disconnect, then reset (SPEC 5.8).
    if (reset_stage == 1 && reply_flushed(now, reset_t)) {
        reset_stage = 2;
        reset_t = now;
        if (ide != NULL) {
            gap_disconnect(ide->handle);
        }
    } else if (reset_stage == 2 && (ide == NULL || now - reset_t > 200)) {
        if (reset_mode == 2) {
            machine_bootloader(0, NULL);
        }
        watchdog_reboot(0, SRAM_END, 0);
        for (;;) {
            __wfi();
        }
    }
}

void bleterm_pbl_link_event(bleterm_link_t *link, bool connected) {
    if (connected) {
        return;
    }
    if (xfer.kind != X_NONE && xfer.con == link->handle) {
        xfer.abort_pending = true; // VM work happens in the worker
    }
    if (rx_con == link->handle) {
        pbl_rx_reset(&rx);
        rx_con = BLETERM_CON_NONE;
    }
    if (rx_pending && rx_pending_con == link->handle) {
        rx_pending = false;
    }
    if (bleterm_g.ide >= 0 && link == &bleterm_g.links[(int)bleterm_g.ide]) {
        // IDE link gone: its queued traffic is stale.
        q_reset(&txq);
        tx_active = false;
        evt_con = BLETERM_CON_NONE;
    }
    bleterm_schedule_worker();
}

void bleterm_pbl_soft_reset(void) {
    // VM still alive: close the transfer now (SPEC 5.6).
    if (xfer.kind != X_NONE) {
        xfer_abort_notify(ST_EABORT);
    }
    MP_STATE_PORT(bleterm_pbl_obj) = MP_OBJ_NULL;
}

void bleterm_pbl_vm_ready(void) {
    MP_STATE_PORT(bleterm_pbl_obj) = MP_OBJ_NULL;
    xfer.kind = X_NONE;
}

// ---------------------------------------------------------------------------
// Python API: keys(), add_key(id, key, name), remove_key(id), _boot_done()

static mp_obj_t bleterm_keys(void) {
    if (!keys_loaded) {
        keys_load();
    }
    mp_obj_t l = mp_obj_new_list(0, NULL);
    for (int i = 0; i < nkeys; ++i) {
        mp_obj_t t[2] = {
            mp_obj_new_bytes(keys[i].id, 4),
            mp_obj_new_str(keys[i].name, key_name_len(&keys[i])),
        };
        mp_obj_list_append(l, mp_obj_new_tuple(2, t));
    }
    return l;
}
MP_DEFINE_CONST_FUN_OBJ_0(bleterm_keys_obj, bleterm_keys);

static mp_obj_t bleterm_add_key(mp_obj_t id_in, mp_obj_t key_in, mp_obj_t name_in) {
    mp_buffer_info_t id, key;
    mp_get_buffer_raise(id_in, &id, MP_BUFFER_READ);
    mp_get_buffer_raise(key_in, &key, MP_BUFFER_READ);
    size_t name_len;
    const char *name = mp_obj_str_get_data(name_in, &name_len);
    if (id.len != 4 || key.len != 32 || name_len > KEY_NAME_MAX
        || memcmp(id.buf, "\xff\xff\xff\xff", 4) == 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("id: 4 bytes, key: 32 bytes, name: <= 16 bytes"));
    }
    if (!keys_loaded) {
        keys_load();
    }
    key_add(id.buf, key.buf, name, name_len);
    if (!keys_save()) {
        mp_raise_OSError(MP_EIO);
    }
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_3(bleterm_add_key_obj, bleterm_add_key);

static mp_obj_t bleterm_remove_key(mp_obj_t id_in) {
    mp_buffer_info_t id;
    mp_get_buffer_raise(id_in, &id, MP_BUFFER_READ);
    if (!keys_loaded) {
        keys_load();
    }
    int k = id.len == 4 ? key_find(id.buf) : -1;
    if (k < 0) {
        return mp_const_false;
    }
    key_remove_at(k);
    if (!keys_save()) {
        mp_raise_OSError(MP_EIO);
    }
    return mp_const_true;
}
MP_DEFINE_CONST_FUN_OBJ_1(bleterm_remove_key_obj, bleterm_remove_key);

static mp_obj_t bleterm_boot_done(void) {
    boot_evt_pending = true;
    bleterm_schedule_worker();
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(bleterm_boot_done_obj, bleterm_boot_done);
