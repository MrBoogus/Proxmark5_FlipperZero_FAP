#include "t5577_crypto.h"
#include <string.h>
#include <stdio.h>

static inline uint32_t rotl32(uint32_t value, unsigned int count) {
    const unsigned int mask = 31;
    count &= mask;
    return (value << count) | (value >> ((-count) & mask));
}

uint32_t t5577_white_pwdgen(uint32_t card_id) {
    uint32_t r1 = rotl32(card_id & 0x000000ECU, 8);
    uint32_t r2 = rotl32(card_id & 0x86000000U, 16);
    uint32_t pwd = 0x00010303U;
    pwd += ((card_id & 0x86EE00ECU) ^ r1 ^ r2);
    return pwd;
}

void t5577_build_em4100_blocks(const uint8_t uid[5], uint32_t* out_hi, uint32_t* out_lo) {
    if(!out_hi || !out_lo) return;

    // 9-bit leading 1s header
    uint64_t frame = 0x1FFULL;
    int col_parity[4] = {0, 0, 0, 0};

    // 10 nibbles from 5 bytes
    for(int i = 0; i < 10; i++) {
        uint8_t byte = uid[i / 2];
        uint8_t nibble = (i % 2 == 0) ? ((byte >> 4) & 0x0F) : (byte & 0x0F);

        int row_parity = 0;
        for(int b = 3; b >= 0; b--) {
            int bit = (nibble >> b) & 1;
            frame = (frame << 1) | bit;
            row_parity ^= bit;
            col_parity[3 - b] ^= bit;
        }
        // 1 even parity bit after each 4-bit nibble
        frame = (frame << 1) | row_parity;
    }

    // 4 column parity bits
    for(int c = 0; c < 4; c++) {
        frame = (frame << 1) | col_parity[c];
    }

    // 1 stop bit (0)
    frame = (frame << 1) | 0;

    *out_hi = (uint32_t)(frame >> 32);
    *out_lo = (uint32_t)(frame & 0xFFFFFFFFULL);
}

void t5577_parse_config_block0(uint32_t block0, T5577ConfigInfo* info) {
    if(!info) return;
    memset(info, 0, sizeof(*info));

    // Modulation: bits 16..12
    uint8_t mod = (block0 >> 12) & 0x1F;
    switch(mod) {
    case 0x00:
        strncpy(info->modulation, "Direct", sizeof(info->modulation));
        break;
    case 0x01:
        strncpy(info->modulation, "PSK1", sizeof(info->modulation));
        break;
    case 0x02:
        strncpy(info->modulation, "PSK2", sizeof(info->modulation));
        break;
    case 0x03:
        strncpy(info->modulation, "PSK3", sizeof(info->modulation));
        break;
    case 0x04:
        strncpy(info->modulation, "FSK1", sizeof(info->modulation));
        break;
    case 0x05:
        strncpy(info->modulation, "FSK2", sizeof(info->modulation));
        break;
    case 0x06:
        strncpy(info->modulation, "FSK1a", sizeof(info->modulation));
        break;
    case 0x07:
        strncpy(info->modulation, "FSK2a", sizeof(info->modulation));
        break;
    case 0x08:
        strncpy(info->modulation, "Manchester", sizeof(info->modulation));
        break;
    case 0x10:
        strncpy(info->modulation, "BiPhase", sizeof(info->modulation));
        break;
    case 0x18:
        strncpy(info->modulation, "DiPhase", sizeof(info->modulation));
        break;
    default:
        strncpy(info->modulation, "Custom", sizeof(info->modulation));
        break;
    }

    // Bitrate: bits 20..18
    uint8_t rate_idx = (block0 >> 18) & 0x07;
    switch(rate_idx) {
    case 0:
        strncpy(info->bitrate, "RF/8", sizeof(info->bitrate));
        break;
    case 1:
        strncpy(info->bitrate, "RF/16", sizeof(info->bitrate));
        break;
    case 2:
        strncpy(info->bitrate, "RF/32", sizeof(info->bitrate));
        break;
    case 3:
        strncpy(info->bitrate, "RF/40", sizeof(info->bitrate));
        break;
    case 4:
        strncpy(info->bitrate, "RF/50", sizeof(info->bitrate));
        break;
    case 5:
        strncpy(info->bitrate, "RF/64", sizeof(info->bitrate));
        break;
    case 6:
        strncpy(info->bitrate, "RF/100", sizeof(info->bitrate));
        break;
    case 7:
        strncpy(info->bitrate, "RF/128", sizeof(info->bitrate));
        break;
    }

    info->max_block = (block0 >> 5) & 0x07;
    info->sequence_term = ((block0 >> 3) & 0x01) != 0;
    info->pwd_enabled = ((block0 >> 4) & 0x01) != 0;
    info->aor = ((block0 >> 9) & 0x01) != 0;
    info->inverted = ((block0 >> 7) & 0x01) != 0;
    info->fast_write = ((block0 >> 17) & 0x01) != 0;
}
