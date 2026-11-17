// bleterm: C-level BLE terminal for the Pico W BLE IDE (SPEC 5.2).
//
// Lifecycle, system ATT services, advertising, links and the Python API.
// All state is static. BTstack callbacks only touch static buffers and
// BTstack; VM work happens in the worker (scheduler node).
#include <string.h>
#include <stdio.h>

#include "py/runtime.h"
#include "py/mphal.h"
#include "shared/runtime/softtimer.h"
#include "lib/btstack/src/btstack.h"
#include "pico/platform.h"
#include "mpbthciport.h"
#include "extmod/modbluetooth.h"

#include "bleterm_int.h"

bleterm_state_t bleterm_g = {
    .require_pairing = true,
    .led_enabled = true,
    .ide = -1,
};

// Session flag: survives machine.reset() and watchdog resets, not power loss.
#define SESSION_MAGIC (0x424c4531u) // "BLE1"
static uint32_t __uninitialized_ram(bleterm_session_word);
static bool session_read_done;

static btstack_packet_callback_registration_t hci_reg;
static soft_timer_entry_t tick_timer;
static bool tick_timer_inited;
static mp_sched_node_t worker_node;

// UUIDs in big-endian (display) order, as att_db_util expects.
static const uint8_t uuid_nus[16] = {0x6e, 0x40, 0x00, 0x01, 0xb5, 0xa3, 0xf3, 0x93, 0xe0, 0xa9, 0xe5, 0x0e, 0x24, 0xdc, 0xca, 0x9e};
static const uint8_t uuid_nus_rx[16] = {0x6e, 0x40, 0x00, 0x02, 0xb5, 0xa3, 0xf3, 0x93, 0xe0, 0xa9, 0xe5, 0x0e, 0x24, 0xdc, 0xca, 0x9e};
static const uint8_t uuid_nus_tx[16] = {0x6e, 0x40, 0x00, 0x03, 0xb5, 0xa3, 0xf3, 0x93, 0xe0, 0xa9, 0xe5, 0x0e, 0x24, 0xdc, 0xca, 0x9e};
static const uint8_t uuid_pbl[16] = {0x3b, 0xd1, 0x00, 0x01, 0x63, 0xa3, 0x41, 0x14, 0xbd, 0xed, 0x13, 0x0f, 0xdb, 0x39, 0x8c, 0x87};
static const uint8_t uuid_pbl_rx[16] = {0x3b, 0xd1, 0x00, 0x02, 0x63, 0xa3, 0x41, 0x14, 0xbd, 0xed, 0x13, 0x0f, 0xdb, 0x39, 0x8c, 0x87};
static const uint8_t uuid_pbl_tx[16] = {0x3b, 0xd1, 0x00, 0x03, 0x63, 0xa3, 0x41, 0x14, 0xbd, 0xed, 0x13, 0x0f, 0xdb, 0x39, 0x8c, 0x87};

// ---------------------------------------------------------------------------
// Links

bleterm_link_t *bleterm_link_find(uint16_t con_handle) {
    for (int i = 0; i < BLETERM_MAX_LINKS; ++i) {
        if (bleterm_g.links[i].handle == con_handle) {
            return &bleterm_g.links[i];
        }
    }
    return NULL;
}

static bleterm_link_t *link_add(uint16_t con_handle, bool peripheral) {
    bleterm_link_t *link = bleterm_link_find(con_handle);
    if (link == NULL) {
        link = bleterm_link_find(BLETERM_CON_NONE);
    }
    if (link == NULL) {
        return NULL;
    }
    memset(link, 0, sizeof(*link));
    link->handle = con_handle;
    link->peripheral = peripheral;
    link->t_connect_ms = mp_hal_ticks_ms();
    return link;
}

static bleterm_link_t *ide_link(void) {
    if (bleterm_g.ide < 0) {
        return NULL;
    }
    return &bleterm_g.links[(int)bleterm_g.ide];
}

static int links_count(void) {
    int n = 0;
    for (int i = 0; i < BLETERM_MAX_LINKS; ++i) {
        n += bleterm_g.links[i].handle != BLETERM_CON_NONE;
    }
    return n;
}

bleterm_link_t *bleterm_nus_target(void) {
    bleterm_link_t *link = ide_link();
    if (link == NULL || !link->auth || !(link->cccd_nus & 1)) {
        return NULL;
    }
    return link;
}

void bleterm_become_ide(bleterm_link_t *link) {
    link->auth = true;
    bleterm_g.ide = (int8_t)(link - bleterm_g.links);
    bleterm_session_word = SESSION_MAGIC;
    bleterm_adv_update();
    bleterm_nus_kick();
}

uint16_t bleterm_mtu(uint16_t con_handle) {
    uint16_t mtu = att_server_get_mtu(con_handle);
    return mtu < ATT_DEFAULT_MTU ? ATT_DEFAULT_MTU : mtu;
}

// ---------------------------------------------------------------------------
// Advertising

static void adv_build(void) {
    uint8_t *a = bleterm_g.adv_data;
    a[0] = 2;
    a[1] = BLUETOOTH_DATA_TYPE_FLAGS;
    a[2] = 0x06;
    a[3] = 17;
    a[4] = BLUETOOTH_DATA_TYPE_COMPLETE_LIST_OF_128_BIT_SERVICE_CLASS_UUIDS;
    reverse_128(uuid_nus, &a[5]);
    bleterm_g.adv_len = 21;
    uint8_t *s = bleterm_g.sr_data;
    s[0] = 1 + bleterm_g.name_len;
    s[1] = BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME;
    memcpy(&s[2], bleterm_g.name, bleterm_g.name_len);
    bleterm_g.sr_len = 2 + bleterm_g.name_len;
}

static void adv_apply(void) {
    bd_addr_t null_addr = {0};
    // 100 ms, ADV_IND, all channels, no filter.
    gap_advertisements_set_params(160, 160, 0, 0, null_addr, 0x07, 0x00);
    gap_advertisements_set_data(bleterm_g.adv_len, bleterm_g.adv_data);
    gap_scan_response_set_data(bleterm_g.sr_len, bleterm_g.sr_data);
}

// Advertise whenever there is no IDE link and a connection slot is free.
void bleterm_adv_update(void) {
    if (!bleterm_g.started || bleterm_g.user_adv) {
        return;
    }
    bool want = bleterm_g.ide < 0 && links_count() < MAX_NR_HCI_CONNECTIONS;
    gap_advertisements_enable(want);
}

void bleterm_user_adv_begin(void) {
    bleterm_g.user_adv = true;
}

void bleterm_user_adv_end(void) {
    bleterm_g.user_adv = false;
    gap_advertisements_enable(false);
    adv_apply();
    bleterm_adv_update();
}

// ---------------------------------------------------------------------------
// ATT system block

void bleterm_att_add_system_services(void) {
    bleterm_state_t *g = &bleterm_g;
    // GAP: handles 1-3 (device name is handle 3, dynamic).
    att_db_util_add_service_uuid16(GAP_SERVICE_UUID);
    g->h_gap_name = att_db_util_add_characteristic_uuid16(GAP_DEVICE_NAME_UUID,
        ATT_PROPERTY_READ | ATT_PROPERTY_DYNAMIC, ATT_SECURITY_NONE, ATT_SECURITY_NONE, NULL, 0);
    // GATT: Service Changed, indicate + CCCD.
    g->h_first = att_db_util_add_service_uuid16(ORG_BLUETOOTH_SERVICE_GENERIC_ATTRIBUTE);
    static uint8_t sc_value[4] = {0, 0, 0, 0};
    g->h_sc = att_db_util_add_characteristic_uuid16(ORG_BLUETOOTH_CHARACTERISTIC_GATT_SERVICE_CHANGED,
        ATT_PROPERTY_INDICATE, ATT_SECURITY_NONE, ATT_SECURITY_NONE, sc_value, sizeof(sc_value));
    g->h_sc_cccd = g->h_sc + 1;
    // NUS.
    att_db_util_add_service_uuid128(uuid_nus);
    g->h_nus_rx = att_db_util_add_characteristic_uuid128(uuid_nus_rx,
        ATT_PROPERTY_WRITE | ATT_PROPERTY_WRITE_WITHOUT_RESPONSE | ATT_PROPERTY_DYNAMIC,
        ATT_SECURITY_NONE, ATT_SECURITY_NONE, NULL, 0);
    g->h_nus_tx = att_db_util_add_characteristic_uuid128(uuid_nus_tx,
        ATT_PROPERTY_NOTIFY, ATT_SECURITY_NONE, ATT_SECURITY_NONE, NULL, 0);
    g->h_nus_cccd = g->h_nus_tx + 1;
    // PBL.
    att_db_util_add_service_uuid128(uuid_pbl);
    g->h_pbl_rx = att_db_util_add_characteristic_uuid128(uuid_pbl_rx,
        ATT_PROPERTY_WRITE | ATT_PROPERTY_WRITE_WITHOUT_RESPONSE | ATT_PROPERTY_DYNAMIC,
        ATT_SECURITY_NONE, ATT_SECURITY_NONE, NULL, 0);
    g->h_pbl_tx = att_db_util_add_characteristic_uuid128(uuid_pbl_tx,
        ATT_PROPERTY_NOTIFY, ATT_SECURITY_NONE, ATT_SECURITY_NONE, NULL, 0);
    g->h_pbl_cccd = g->h_pbl_tx + 1;
    g->h_last = g->h_pbl_cccd;
    g->att_db_size = att_db_util_get_size();
}

bool bleterm_att_owns_handle(uint16_t att_handle) {
    return bleterm_g.started && bleterm_g.h_first != 0
           && att_handle >= bleterm_g.h_first && att_handle <= bleterm_g.h_last;
}

const char *bleterm_name(size_t *len) {
    *len = bleterm_g.name_len;
    return bleterm_g.name;
}

static uint16_t read_u16(uint16_t v, uint16_t offset, uint8_t *buffer, uint16_t buffer_size) {
    uint8_t b[2] = {v & 0xff, v >> 8};
    return att_read_callback_handle_blob(b, 2, offset, buffer, buffer_size);
}

uint16_t bleterm_att_read(uint16_t con_handle, uint16_t att_handle, uint16_t offset, uint8_t *buffer, uint16_t buffer_size) {
    bleterm_state_t *g = &bleterm_g;
    if (att_handle == g->h_gap_name) {
        return att_read_callback_handle_blob((const uint8_t *)g->name, g->name_len, offset, buffer, buffer_size);
    }
    bleterm_link_t *link = bleterm_link_find(con_handle);
    if (link == NULL) {
        return read_u16(0, offset, buffer, buffer_size);
    }
    if (att_handle == g->h_sc_cccd) {
        return read_u16(link->cccd_sc, offset, buffer, buffer_size);
    }
    if (att_handle == g->h_nus_cccd) {
        return read_u16(link->cccd_nus, offset, buffer, buffer_size);
    }
    if (att_handle == g->h_pbl_cccd) {
        return read_u16(link->cccd_pbl, offset, buffer, buffer_size);
    }
    return 0;
}

// A write request (with response) is processed from att_server's request
// buffer; a write command comes straight from the L2CAP packet.
static bool is_write_request(uint16_t con_handle, const uint8_t *buffer) {
    hci_connection_t *conn = hci_connection_for_handle(con_handle);
    return conn != NULL && buffer == conn->att_server.request_buffer + 3;
}

int bleterm_att_write(uint16_t con_handle, uint16_t att_handle, uint16_t transaction_mode, uint16_t offset, uint8_t *buffer, uint16_t buffer_size) {
    bleterm_state_t *g = &bleterm_g;
    if (transaction_mode != ATT_TRANSACTION_MODE_NONE || offset != 0) {
        // Prepared (long) writes are not supported: hosts send at most MTU-3.
        return ATT_ERROR_REQUEST_NOT_SUPPORTED;
    }
    bleterm_link_t *link = bleterm_link_find(con_handle);
    if (link == NULL) {
        // Link that existed before bleterm.start().
        link = link_add(con_handle, true);
        if (link == NULL) {
            return ATT_ERROR_INSUFFICIENT_RESOURCES;
        }
    }
    uint16_t v = buffer_size >= 2 ? little_endian_read_16(buffer, 0) : 0;
    if (att_handle == g->h_sc_cccd) {
        link->cccd_sc = v;
        return 0;
    }
    if (att_handle == g->h_nus_cccd) {
        link->cccd_nus = v;
        if ((v & 1) && g->ide < 0 && !g->require_pairing) {
            // Open device: the first NUS subscriber is the IDE link.
            bleterm_become_ide(link);
        }
        bleterm_nus_kick();
        return 0;
    }
    if (att_handle == g->h_pbl_cccd) {
        link->cccd_pbl = v;
        return 0;
    }
    if (att_handle == g->h_nus_rx) {
        if (g->ide < 0 && !g->require_pairing) {
            bleterm_become_ide(link);
        }
        if (link != ide_link() || !link->auth) {
            // LOCKED (or another link is the IDE link): input is ignored.
            return 0;
        }
        return bleterm_nus_rx(link, buffer, buffer_size, is_write_request(con_handle, buffer));
    }
    if (att_handle == g->h_pbl_rx) {
        return bleterm_pbl_rx(link, buffer, buffer_size, is_write_request(con_handle, buffer));
    }
    return 0;
}

void bleterm_db_changed(void) {
    // Indicate the user handle range to a subscribed IDE link (SPEC 5.7).
    bleterm_link_t *link = ide_link();
    if (!bleterm_g.started || link == NULL || !(link->cccd_sc & 2)) {
        return;
    }
    uint8_t range[4];
    little_endian_store_16(range, 0, bleterm_g.h_last + 1);
    little_endian_store_16(range, 2, 0xffff);
    att_server_indicate(link->handle, bleterm_g.h_sc, range, sizeof(range));
}

void bleterm_user_state_destroyed(void) {
    // Soft reset or BLE().active(False): only the IDE link stays.
    for (int i = 0; i < BLETERM_MAX_LINKS; ++i) {
        bleterm_link_t *link = &bleterm_g.links[i];
        if (link->handle != BLETERM_CON_NONE && i != bleterm_g.ide) {
            gap_disconnect(link->handle);
        }
    }
    bleterm_user_adv_end();
    bleterm_db_changed();
}

// ---------------------------------------------------------------------------
// HCI events (BTstack context)

static void hci_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET || !bleterm_g.started) {
        return;
    }
    switch (hci_event_packet_get_type(packet)) {
        case HCI_EVENT_LE_META:
            if (hci_event_le_meta_get_subevent_code(packet) == HCI_SUBEVENT_LE_CONNECTION_COMPLETE
                && hci_subevent_le_connection_complete_get_status(packet) == 0) {
                uint16_t h = hci_subevent_le_connection_complete_get_connection_handle(packet);
                bool peripheral = hci_subevent_le_connection_complete_get_role(packet) == 1;
                bleterm_link_t *link = link_add(h, peripheral);
                if (peripheral) {
                    // 7.5-15 ms, latency 0, 2 s supervision timeout. May be refused.
                    gap_request_connection_parameter_update(h, 6, 12, 0, 200);
                }
                if (link != NULL) {
                    bleterm_pbl_link_event(link, true);
                }
                bleterm_adv_update();
            }
            break;
        case HCI_EVENT_DISCONNECTION_COMPLETE: {
            uint16_t h = hci_event_disconnection_complete_get_connection_handle(packet);
            bleterm_link_t *link = bleterm_link_find(h);
            if (link != NULL) {
                bleterm_pbl_link_event(link, false);
                if (link == ide_link()) {
                    bleterm_g.ide = -1;
                    if (!bleterm_g.keep_session) {
                        bleterm_session_word = 0;
                    }
                    bleterm_nus_reset();
                }
                if (bleterm_g.rx_pending && bleterm_g.rx_pending_con == h) {
                    bleterm_g.rx_pending = false;
                }
                link->handle = BLETERM_CON_NONE;
            }
            bleterm_adv_update();
            break;
        }
        default:
            break;
    }
}

void bleterm_on_stack_init(void) {
    hci_reg.callback = &hci_handler;
    hci_add_event_handler(&hci_reg);
}

// ---------------------------------------------------------------------------
// Worker and tick

static void worker(mp_sched_node_t *node) {
    (void)node;
    if (bleterm_g.vm_down) {
        return;
    }
    if (bleterm_g.started) {
        bleterm_nus_worker();
        // Drop links that neither authenticate nor belong to user code.
        uint32_t now = mp_hal_ticks_ms();
        if (!mp_bluetooth_btstack_user_state_present()) {
            for (int i = 0; i < BLETERM_MAX_LINKS; ++i) {
                bleterm_link_t *link = &bleterm_g.links[i];
                if (link->handle != BLETERM_CON_NONE && i != bleterm_g.ide && !link->auth
                    && (now - link->t_connect_ms) > 10000) {
                    link->t_connect_ms = now; // retry later if refused
                    gap_disconnect(link->handle);
                }
            }
        }
        if (bleterm_g.pairing_until_ms != 0 && (int32_t)(now - bleterm_g.pairing_until_ms) >= 0) {
            bleterm_g.pairing_until_ms = 0;
        }
        bleterm_pbl_worker();
    }
    bleterm_led_worker();
}

void bleterm_schedule_worker(void) {
    mp_sched_schedule_node(&worker_node, worker);
}

static void tick_cb(soft_timer_entry_t *self) {
    (void)self;
    bleterm_schedule_worker();
}

static void tick_start(void) {
    if (!tick_timer_inited) {
        // Static entry: kept by soft_timer_deinit() across soft resets.
        soft_timer_static_init(&tick_timer, SOFT_TIMER_MODE_PERIODIC, 50, tick_cb);
        soft_timer_insert(&tick_timer, 50);
        tick_timer_inited = true;
    }
}

// ---------------------------------------------------------------------------
// Soft reset

void bleterm_soft_reset_begin(void) {
    bleterm_pbl_soft_reset();
    bleterm_g.vm_down = true;
}

void bleterm_vm_ready(void) {
    bleterm_g.vm_down = false;
    bleterm_g.led_released = false;
    if (!session_read_done) {
        session_read_done = true;
        bleterm_g.session_at_boot = bleterm_session_word == SESSION_MAGIC;
        bleterm_session_word = 0;
    }
    if (bleterm_g.started) {
        // Process anything queued while the VM was down.
        mp_bluetooth_hci_poll_now();
        bleterm_pbl_vm_ready();
    }
    bleterm_schedule_worker();
}

bool bleterm_safe_mode(void) {
    return bleterm_g.safe_mode;
}

bool bleterm_is_started(void) {
    return bleterm_g.started;
}

// ---------------------------------------------------------------------------
// Python API

// Default advertised name: <prefix>-XXXX (last two address bytes). Boards override it.
#ifndef MICROPY_BLETERM_NAME_PREFIX
#define MICROPY_BLETERM_NAME_PREFIX "PicoBLE"
#endif

static void name_update(void) {
    bleterm_state_t *g = &bleterm_g;
    if (g->cfg_name[0]) {
        strcpy(g->name, g->cfg_name);
    } else {
        bd_addr_t addr;
        gap_local_bd_addr(addr);
        snprintf(g->name, sizeof(g->name), MICROPY_BLETERM_NAME_PREFIX "-%02X%02X", addr[4], addr[5]);
    }
    g->name_len = strlen(g->name);
}

static void links_reset(void) {
    for (int i = 0; i < BLETERM_MAX_LINKS; ++i) {
        bleterm_g.links[i].handle = BLETERM_CON_NONE;
    }
    bleterm_g.ide = -1;
}

static mp_obj_t bleterm_start(void) {
    if (bleterm_g.started) {
        return mp_const_none;
    }
    int ret = mp_bluetooth_btstack_stack_up();
    if (ret != 0) {
        mp_raise_OSError(ret);
    }
    bleterm_on_stack_init(); // idempotent
    links_reset();
    bleterm_nus_reset();
    bleterm_g.user_adv = false;
    bleterm_g.started = true;
    name_update();
    l2cap_set_max_le_mtu(247);
    // BTstack allows one peripheral link by default: the IDE link plus user links.
    gap_set_max_number_peripheral_connections(MAX_NR_HCI_CONNECTIONS);
    // Any user services registered so far are dropped (start() is meant to
    // run from _bleboot, before user code).
    mp_bluetooth_btstack_reset_db();
    if (mp_bluetooth_btstack_user_state_present()) {
        mp_bluetooth_gap_set_device_name((const uint8_t *)bleterm_g.name, bleterm_g.name_len);
    }
    adv_build();
    adv_apply();
    bleterm_adv_update();
    tick_start();
    bleterm_pbl_on_start();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(bleterm_start_obj, bleterm_start);

static mp_obj_t bleterm_stop(void) {
    if (!bleterm_g.started) {
        return mp_const_none;
    }
    for (int i = 0; i < BLETERM_MAX_LINKS; ++i) {
        if (bleterm_g.links[i].handle != BLETERM_CON_NONE && (i == bleterm_g.ide || !bleterm_g.links[i].auth)) {
            gap_disconnect(bleterm_g.links[i].handle);
        }
    }
    if (!bleterm_g.user_adv) {
        gap_advertisements_enable(false);
    }
    bleterm_g.started = false;
    bleterm_g.ide = -1;
    bleterm_g.pairing_until_ms = 0;
    bleterm_session_word = 0;
    bleterm_nus_reset();
    if (!mp_bluetooth_btstack_user_state_present()) {
        mp_bluetooth_btstack_stack_down();
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(bleterm_stop_obj, bleterm_stop);

static mp_obj_t bleterm_configure(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_name, ARG_require_pairing, ARG_led };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_name, MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_require_pairing, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = true} },
        { MP_QSTR_led, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = true} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    if (args[ARG_name].u_obj == mp_const_none) {
        bleterm_g.cfg_name[0] = 0;
    } else {
        size_t len;
        const char *s = mp_obj_str_get_data(args[ARG_name].u_obj, &len);
        if (len == 0 || len > BLETERM_NAME_MAX) {
            mp_raise_ValueError(MP_ERROR_TEXT("name length"));
        }
        memcpy(bleterm_g.cfg_name, s, len);
        bleterm_g.cfg_name[len] = 0;
    }
    bleterm_g.require_pairing = args[ARG_require_pairing].u_bool;
    bleterm_g.led_enabled = args[ARG_led].u_bool;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(bleterm_configure_obj, 0, bleterm_configure);

static mp_obj_t bleterm_pairing(size_t n_args, const mp_obj_t *args) {
    mp_int_t s = n_args > 0 ? mp_obj_get_int(args[0]) : 60;
    if (s <= 0) {
        bleterm_g.pairing_until_ms = 0;
    } else {
        uint32_t t = mp_hal_ticks_ms() + (uint32_t)s * 1000;
        bleterm_g.pairing_until_ms = t ? t : 1;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(bleterm_pairing_obj, 0, 1, bleterm_pairing);

static void dict_set(mp_obj_t d, qstr k, mp_obj_t v) {
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(k), v);
}

static const char *state_name(void) {
    if (!bleterm_g.started) {
        return "off";
    }
    return bleterm_g.ide >= 0 ? "connected" : "advertising";
}

static mp_obj_t bleterm_status(void) {
    bleterm_state_t *g = &bleterm_g;
    mp_obj_t d = mp_obj_new_dict(16);
    dict_set(d, MP_QSTR_state, mp_obj_new_str_from_cstr(state_name()));
    bleterm_link_t *link = ide_link();
    dict_set(d, MP_QSTR_conn_handle, link ? MP_OBJ_NEW_SMALL_INT(link->handle) : mp_const_none);
    dict_set(d, MP_QSTR_mtu, MP_OBJ_NEW_SMALL_INT(link ? bleterm_mtu(link->handle) : ATT_DEFAULT_MTU));
    dict_set(d, MP_QSTR_locked, mp_obj_new_bool(link == NULL || !link->auth));
    dict_set(d, MP_QSTR_pairing, mp_obj_new_bool(g->pairing_until_ms != 0));
    dict_set(d, MP_QSTR_safe_mode, mp_obj_new_bool(g->safe_mode));
    dict_set(d, MP_QSTR_require_pairing, mp_obj_new_bool(g->require_pairing));
    if (g->started) {
        dict_set(d, MP_QSTR_name, mp_obj_new_str(g->name, g->name_len));
        bd_addr_t addr;
        gap_local_bd_addr(addr);
        dict_set(d, MP_QSTR_addr, mp_obj_new_str_from_cstr(bd_addr_to_str(addr)));
    } else {
        dict_set(d, MP_QSTR_name, g->cfg_name[0] ? mp_obj_new_str_from_cstr(g->cfg_name) : mp_const_none);
        dict_set(d, MP_QSTR_addr, mp_const_none);
    }
    dict_set(d, MP_QSTR_links, MP_OBJ_NEW_SMALL_INT(links_count()));
    dict_set(d, MP_QSTR_rx_dropped, mp_obj_new_int_from_uint(g->rx_dropped));
    dict_set(d, MP_QSTR_tx_dropped, mp_obj_new_int_from_uint(g->tx_dropped));
    dict_set(d, MP_QSTR_att_db_size, MP_OBJ_NEW_SMALL_INT(g->att_db_size));
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_0(bleterm_status_obj, bleterm_status);

static mp_obj_t bleterm_safe_mode_fn(void) {
    return mp_obj_new_bool(bleterm_g.safe_mode);
}
static MP_DEFINE_CONST_FUN_OBJ_0(bleterm_safe_mode_obj, bleterm_safe_mode_fn);

// Used by _bleboot after it showed the button feedback on Pin("LED").
static mp_obj_t bleterm_led_reclaim(void) {
    bleterm_g.led_released = false;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(bleterm_led_reclaim_obj, bleterm_led_reclaim);

static mp_obj_t bleterm_set_safe_mode(mp_obj_t v) {
    bleterm_g.safe_mode = mp_obj_is_true(v);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(bleterm_set_safe_mode_obj, bleterm_set_safe_mode);

static mp_obj_t bleterm_session(void) {
    return mp_obj_new_bool(bleterm_g.session_at_boot);
}
static MP_DEFINE_CONST_FUN_OBJ_0(bleterm_session_obj, bleterm_session);

static mp_obj_t bleterm_version(void) {
    mp_obj_t t[3] = {
        MP_OBJ_NEW_SMALL_INT(BLETERM_VERSION_MAJOR),
        MP_OBJ_NEW_SMALL_INT(BLETERM_VERSION_MINOR),
        MP_OBJ_NEW_SMALL_INT(BLETERM_VERSION_PATCH),
    };
    return mp_obj_new_tuple(3, t);
}
static MP_DEFINE_CONST_FUN_OBJ_0(bleterm_version_obj, bleterm_version);

extern const mp_obj_fun_builtin_fixed_t bleterm_keys_obj;
extern const mp_obj_fun_builtin_fixed_t bleterm_add_key_obj;
extern const mp_obj_fun_builtin_fixed_t bleterm_remove_key_obj;
extern const mp_obj_fun_builtin_fixed_t bleterm_boot_done_obj;
#define BLETERM_PBL_GLOBALS \
    { MP_ROM_QSTR(MP_QSTR_keys), MP_ROM_PTR(&bleterm_keys_obj) }, \
    { MP_ROM_QSTR(MP_QSTR_add_key), MP_ROM_PTR(&bleterm_add_key_obj) }, \
    { MP_ROM_QSTR(MP_QSTR_remove_key), MP_ROM_PTR(&bleterm_remove_key_obj) }, \
    { MP_ROM_QSTR(MP_QSTR__boot_done), MP_ROM_PTR(&bleterm_boot_done_obj) },

static const mp_rom_map_elem_t bleterm_module_globals_table[] = {
    BLETERM_PBL_GLOBALS
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_bleterm) },
    { MP_ROM_QSTR(MP_QSTR_version), MP_ROM_PTR(&bleterm_version_obj) },
    { MP_ROM_QSTR(MP_QSTR_start), MP_ROM_PTR(&bleterm_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&bleterm_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_configure), MP_ROM_PTR(&bleterm_configure_obj) },
    { MP_ROM_QSTR(MP_QSTR_pairing), MP_ROM_PTR(&bleterm_pairing_obj) },
    { MP_ROM_QSTR(MP_QSTR_status), MP_ROM_PTR(&bleterm_status_obj) },
    { MP_ROM_QSTR(MP_QSTR_safe_mode), MP_ROM_PTR(&bleterm_safe_mode_obj) },
    { MP_ROM_QSTR(MP_QSTR__set_safe_mode), MP_ROM_PTR(&bleterm_set_safe_mode_obj) },
    { MP_ROM_QSTR(MP_QSTR__led_reclaim), MP_ROM_PTR(&bleterm_led_reclaim_obj) },
    { MP_ROM_QSTR(MP_QSTR_session), MP_ROM_PTR(&bleterm_session_obj) },
};
static MP_DEFINE_CONST_DICT(bleterm_module_globals, bleterm_module_globals_table);

const mp_obj_module_t bleterm_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&bleterm_module_globals,
};
MP_REGISTER_MODULE(MP_QSTR_bleterm, bleterm_user_cmodule);
