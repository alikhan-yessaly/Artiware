// PBL wire codec. See pbl_codec.h and docs/protocol.md.
#include <string.h>

#include "pbl_codec.h"

uint32_t pbl_crc32(uint32_t crc, const uint8_t *data, size_t n) {
    crc = ~crc;
    while (n--) {
        crc ^= *data++;
        for (int k = 0; k < 8; ++k) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

void pbl_rx_reset(pbl_rx_t *rx) {
    rx->len = 0;
    rx->next_index = 0;
    rx->active = false;
}

int pbl_rx_fragment(pbl_rx_t *rx, const uint8_t *frag, size_t n) {
    if (n < 1) {
        return PBL_RX_DROP;
    }
    uint8_t hdr = frag[0];
    uint8_t index = hdr & PBL_HDR_INDEX;
    if (hdr & PBL_HDR_FIRST) {
        // A FIRST bit drops the message in progress.
        pbl_rx_reset(rx);
        if (index != 0) {
            return PBL_RX_DROP;
        }
        rx->active = true;
    } else if (!rx->active || index != rx->next_index) {
        pbl_rx_reset(rx);
        return PBL_RX_DROP;
    }
    size_t data_len = n - 1;
    if (rx->len + data_len > PBL_MSG_MAX) {
        pbl_rx_reset(rx);
        return PBL_RX_DROP;
    }
    memcpy(rx->buf + rx->len, frag + 1, data_len);
    rx->len += data_len;
    rx->next_index = (index + 1) & PBL_HDR_INDEX;
    if (hdr & PBL_HDR_LAST) {
        rx->active = false;
        return PBL_RX_DONE;
    }
    return PBL_RX_MORE;
}

void pbl_tx_begin(pbl_tx_t *tx, const uint8_t *msg, uint16_t len) {
    tx->msg = msg;
    tx->len = len;
    tx->off = 0;
    tx->index = 0;
    tx->started = false;
}

size_t pbl_tx_next(pbl_tx_t *tx, uint8_t *out, size_t max_frag) {
    if (max_frag < 2 || (tx->started && tx->off >= tx->len)) {
        return 0;
    }
    size_t n = tx->len - tx->off;
    if (n > max_frag - 1) {
        n = max_frag - 1;
    }
    uint8_t hdr = tx->index & PBL_HDR_INDEX;
    if (!tx->started) {
        hdr |= PBL_HDR_FIRST;
    }
    if (tx->off + n >= tx->len) {
        hdr |= PBL_HDR_LAST;
    }
    out[0] = hdr;
    memcpy(out + 1, tx->msg + tx->off, n);
    tx->off += n;
    tx->index = (tx->index + 1) & PBL_HDR_INDEX;
    tx->started = true;
    return n + 1;
}

size_t pbl_msg_encode(uint8_t *out, uint8_t type, uint8_t seq, const uint8_t *payload, size_t n) {
    if (n > PBL_MSG_MAX - 2) {
        return 0;
    }
    out[0] = type;
    out[1] = seq;
    if (n) {
        memmove(out + 2, payload, n);
    }
    return n + 2;
}
