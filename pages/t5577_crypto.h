#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t t5577_white_pwdgen(uint32_t card_id);

void t5577_build_em4100_blocks(const uint8_t uid[5], uint32_t* out_hi, uint32_t* out_lo);

typedef struct {
    char modulation[16];
    char bitrate[12];
    uint8_t max_block;
    bool pwd_enabled;
    bool inverted;
    bool sequence_term;
    bool fast_write;
    bool aor;
} T5577ConfigInfo;

void t5577_parse_config_block0(uint32_t block0, T5577ConfigInfo* info);

#ifdef __cplusplus
}
#endif
