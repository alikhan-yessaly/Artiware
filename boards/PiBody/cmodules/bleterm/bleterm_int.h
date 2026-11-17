// Internal state shared by the bleterm source files. Static memory only.
#ifndef BLETERM_INT_H
#define BLETERM_INT_H

#include "bleterm.h"

#define BLETERM_MAX_LINKS (4)
#define BLETERM_NAME_MAX (29) // fits the scan response (31 - 2)
#define BLETERM_CON_NONE (0xffff)

enum {
    BLETERM_OFF = 0,
    BLETERM_ADVERTISING,
    BLETERM_CONNECTED,
};

typedef struct {
    uint16_t handle;        // BLETERM_CON_NONE when the slot is free
    bool peripheral;        // we are the peripheral on this link
    bool auth;              // authenticated (or open device)
    uint16_t cccd_sc;       // Service Changed CCCD
    uint16_t cccd_nus;      // NUS TX CCCD
    uint16_t cccd_pbl;      // PBL TX CCCD
    uint32_t t_connect_ms;
    uint8_t nonce[8];       // PBL HELLO nonce, new per connection
    bool nonce_valid;
} bleterm_link_t;

typedef struct {
    bool started;
    volatile bool vm_down;
    bool user_adv;          // user code owns the advertising payload
    bool require_pairing;
    bool led_enabled;
    bool led_released;
    bool safe_mode;
    bool session_at_boot;
    bool keep_session;      // planned reset: keep the session flag on disconnect
    char cfg_name[BLETERM_NAME_MAX + 1]; // configured name ("" = default)
    char name[BLETERM_NAME_MAX + 1];     // name in use
    uint8_t name_len;
    bleterm_link_t links[BLETERM_MAX_LINKS];
    int8_t ide;             // index into links of the IDE link, or -1
    uint8_t adv_data[31];
    uint8_t adv_len;
    uint8_t sr_data[31];
    uint8_t sr_len;
    // NUS input waiting for room in stdin_ringbuf (write with response).
    bool rx_pending;
    uint16_t rx_pending_con;
    uint16_t rx_pending_len;
    uint32_t rx_dropped;
    uint32_t tx_dropped;
    // Pairing window end (ticks_ms), 0 = closed.
    uint32_t pairing_until_ms;
    // System block handles (filled by bleterm_att_add_system_services).
    uint16_t h_gap_name, h_sc, h_sc_cccd;
    uint16_t h_nus_rx, h_nus_tx, h_nus_cccd;
    uint16_t h_pbl_rx, h_pbl_tx, h_pbl_cccd;
    uint16_t h_first, h_last;
    uint16_t att_db_size;
} bleterm_state_t;

extern bleterm_state_t bleterm_g;

bleterm_link_t *bleterm_link_find(uint16_t con_handle);
// The IDE link if it exists, is subscribed to NUS and is authenticated.
bleterm_link_t *bleterm_nus_target(void);
void bleterm_become_ide(bleterm_link_t *link);
void bleterm_schedule_worker(void);
void bleterm_adv_update(void);
uint16_t bleterm_mtu(uint16_t con_handle);

// bleterm_pbl.c
int bleterm_pbl_rx(bleterm_link_t *link, const uint8_t *data, uint16_t len, bool is_request);
void bleterm_pbl_link_event(bleterm_link_t *link, bool connected);
void bleterm_pbl_worker(void);
void bleterm_pbl_soft_reset(void);
void bleterm_pbl_vm_ready(void);
void bleterm_pbl_on_start(void);
uint8_t bleterm_pbl_flags(const bleterm_link_t *link);

// bleterm_led.c
void bleterm_led_worker(void);

// bleterm_nus.c
int bleterm_nus_rx(bleterm_link_t *link, uint8_t *buffer, uint16_t size, bool is_request);
void bleterm_nus_kick(void);
void bleterm_nus_reset(void);
void bleterm_nus_worker(void);
size_t bleterm_nus_tx_pending(void);
void bleterm_stdin_inject(const uint8_t *data, size_t len);

#endif // BLETERM_INT_H
