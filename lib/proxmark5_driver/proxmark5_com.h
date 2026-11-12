#ifndef proxmark5_COM_H
#define proxmark5_COM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "pm3_cmd.h"

void proxmark5_com_init(void);
void proxmark5_com_deinit(void);
bool proxmark5_com_handshake(void);
void proxmark5_com_keep_fast_poll(void);
void proxmark5_com_set_break_loop(bool active);
bool proxmark5_com_send_spi(uint8_t* data, size_t length);
bool proxmark5_com_request_ng(
    uint16_t req_cmd,
    uint8_t* data,
    size_t len,
    uint16_t resp_cmd,
    PacketResponseNG* resp,
    uint32_t timeout_ms,
    uint8_t max_attempts);

#endif // proxmark5_COM_H
