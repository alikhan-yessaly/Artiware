// PBL wire codec (docs/protocol.md section 2): fragments, messages, CRC32.
// Pure C, no MicroPython includes; also compiled for the host tests.
#ifndef PBL_CODEC_H
#define PBL_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PBL_MSG_MAX (512)
#define PBL_HDR_FIRST (0x80)
#define PBL_HDR_LAST (0x40)
#define PBL_HDR_INDEX (0x3f)

// zlib-compatible CRC32: crc = pbl_crc32(0, data, n), or chained.
uint32_t pbl_crc32(uint32_t crc, const uint8_t *data, size_t n);

// Reassembly of one message from fragments.
typedef struct {
    uint8_t buf[PBL_MSG_MAX];
    uint16_t len;
    uint8_t next_index;
    bool active;
} pbl_rx_t;

enum {
    PBL_RX_MORE = 0,     // fragment accepted, message not complete
    PBL_RX_DONE = 1,     // message complete in buf[0..len)
    PBL_RX_DROP = -1,    // fragment dropped (and any message in progress)
};

void pbl_rx_reset(pbl_rx_t *rx);
int pbl_rx_fragment(pbl_rx_t *rx, const uint8_t *frag, size_t n);

// Fragmentation of one message.
typedef struct {
    const uint8_t *msg;
    uint16_t len;
    uint16_t off;
    uint8_t index;
    bool started;
} pbl_tx_t;

void pbl_tx_begin(pbl_tx_t *tx, const uint8_t *msg, uint16_t len);
// Writes the next fragment (at most max_frag bytes, >= 2) into out.
// Returns its length, or 0 when the message is complete.
size_t pbl_tx_next(pbl_tx_t *tx, uint8_t *out, size_t max_frag);

// Message helpers: type u8 | seq u8 | payload.
size_t pbl_msg_encode(uint8_t *out, uint8_t type, uint8_t seq, const uint8_t *payload, size_t n);

static inline void pbl_put_u16(uint8_t *p, uint16_t v) {
    p[0] = v & 0xff;
    p[1] = v >> 8;
}
static inline void pbl_put_u32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff;
    p[3] = v >> 24;
}
static inline uint32_t pbl_get_u32(const uint8_t *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

#endif // PBL_CODEC_H
