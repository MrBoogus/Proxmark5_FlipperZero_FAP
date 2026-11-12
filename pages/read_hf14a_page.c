#include <furi.h>
#include <gui/canvas.h>
#include <gui/elements.h>
#include <storage/storage.h>
#include <stdio.h>
#include <string.h>
#include "read_hf14a_page.h"
#include "proxmark5_frame.h"
#include "pm3_cmd.h"

typedef enum {
    ReadHf14aPageStateIdle,
    ReadHf14aPageStateRunning,
    ReadHf14aPageStateWritingMagic,
    ReadHf14aPageStateSuccess,
    ReadHf14aPageStateNotFound,
    ReadHf14aPageStateTimeout,
    ReadHf14aPageStateError,
} ReadHf14aPageState;

typedef enum {
    ReadHf14aWorkerTaskScan = 0,
    ReadHf14aWorkerTaskWriteMagic = 1,
} ReadHf14aWorkerTask;

typedef struct {
    ReadHf14aPageState state;
    iso14a_card_select_t card;
    bool has_cached_card;
    uint16_t magic_flags;
    uint8_t nonce_status;
    char action_msg[40];
} ReadHf14aPageModel;

struct ReadHf14aPage {
    View* view;

    FuriThread* worker_thread;
    volatile bool worker_thread_running;
    volatile bool worker_thread_cancel_requested;
    ReadHf14aWorkerTask worker_task;

    ReadHf14aPageBackCallback back_callback;
    void* back_callback_context;
};

static void read_hf14a_page_start_task(ReadHf14aPage* page, ReadHf14aWorkerTask task);

static int read_hf14a_wait_for_response_interruptible(
    uint16_t cmd,
    PacketResponseNG* resp,
    uint32_t timeout_ms,
    volatile bool* cancel_requested) {
    uint32_t start_time = furi_get_tick();

    while((furi_get_tick() - start_time) < timeout_ms) {
        if(cancel_requested && *cancel_requested) {
            return PM3_EOPABORTED;
        }

        if(proxmark5_frame_take_by_cmd(cmd, resp)) {
            return PM3_SUCCESS;
        }

        furi_delay_ms(10);
    }

    if(cancel_requested && *cancel_requested) {
        return PM3_EOPABORTED;
    }

    return proxmark5_frame_take_by_cmd(cmd, resp) ? PM3_SUCCESS : PM3_ETIMEOUT;
}

static int read_hf14a_probe_magic_and_prng(
    volatile bool* cancel_requested,
    bool is_mfc,
    uint16_t* out_magic_flags,
    uint8_t* out_nonce_status) {
    if(out_magic_flags) *out_magic_flags = MAGIC_FLAG_NONE;
    if(out_nonce_status) *out_nonce_status = 0;

    PacketResponseNG* resp = malloc(sizeof(PacketResponseNG));
    if(!resp) {
        return PM3_EMALLOC;
    }

    mf_chinese_ident_t ident_cmd = {
        .is_mfc = is_mfc ? 1 : 0,
        .keytype = 1,
        .key = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
    };

    clearCommandBuffer();
    SendCommandNG(CMD_HF_MIFARE_CIDENT, (uint8_t*)&ident_cmd, sizeof(ident_cmd));
    if(read_hf14a_wait_for_response_interruptible(
           CMD_HF_MIFARE_CIDENT, resp, 2200, cancel_requested) == PM3_SUCCESS) {
        if(resp->status == PM3_SUCCESS && resp->length >= sizeof(uint16_t) && out_magic_flags) {
            uint16_t flags = (uint16_t)resp->data.asBytes[0] |
                             ((uint16_t)resp->data.asBytes[1] << 8);
            *out_magic_flags = flags;
        }
    }

    if(is_mfc && !(cancel_requested && *cancel_requested)) {
        clearCommandBuffer();
        SendCommandNG(CMD_HF_MIFARE_STATIC_NONCE, NULL, 0);
        if(read_hf14a_wait_for_response_interruptible(
               CMD_HF_MIFARE_STATIC_NONCE, resp, 1800, cancel_requested) == PM3_SUCCESS) {
            if(resp->status == PM3_SUCCESS && resp->length >= 1 && out_nonce_status) {
                *out_nonce_status = resp->data.asBytes[0];
            }
        }
    }

    free(resp);
    return PM3_SUCCESS;
}

static int read_hf14a_test(
    volatile bool* cancel_requested,
    iso14a_card_select_t* out_card,
    bool* out_found,
    uint16_t* out_magic_flags,
    uint8_t* out_nonce_status) {
    if(out_found) {
        *out_found = false;
    }
    if(out_card) {
        memset(out_card, 0, sizeof(*out_card));
    }
    if(out_magic_flags) {
        *out_magic_flags = MAGIC_FLAG_NONE;
    }
    if(out_nonce_status) {
        *out_nonce_status = 0;
    }

    iso14a_raw_cmd_t cmd = {
        .flags = ISO14A_CONNECT | ISO14A_CLEARTRACE,
        .timeout = 0,
        .wait_us = 0,
        .len = 0,
        .lenbits = 0,
    };
    PacketResponseNG* resp = malloc(sizeof(PacketResponseNG));
    if(!resp) {
        return PM3_EMALLOC;
    }

    clearCommandBuffer();

    if(cancel_requested && *cancel_requested) {
        free(resp);
        return PM3_EOPABORTED;
    }

    SendCommandNG(CMD_HF_ISO14443A_READER, (uint8_t*)&cmd, sizeof(cmd));

    int wait_result = read_hf14a_wait_for_response_interruptible(
        CMD_HF_ISO14443A_READER, resp, 2000, cancel_requested);
    if(wait_result != PM3_SUCCESS) {
        free(resp);
        return wait_result;
    }

    if(resp->status != PM3_SUCCESS ||
       resp->length < sizeof(iso14a_raw_resp_t) + sizeof(iso14a_card_select_t)) {
        free(resp);
        return PM3_ESOFT;
    }

    const iso14a_raw_resp_t* hdr = (const iso14a_raw_resp_t*)resp->data.asBytes;
    bool found = (hdr->sel != 0);
    if(found) {
        if(out_card) {
            memcpy(out_card, resp->data.asBytes + sizeof(iso14a_raw_resp_t), sizeof(*out_card));
        }
        if(out_found) {
            *out_found = true;
        }
    }

    free(resp);

    if(found && out_card) {
        bool is_mfc =
            (out_card->sak == 0x08 || out_card->sak == 0x09 || out_card->sak == 0x18 ||
             out_card->sak == 0x28 || out_card->sak == 0x38 || out_card->sak == 0x88);
        read_hf14a_probe_magic_and_prng(
            cancel_requested, is_mfc, out_magic_flags, out_nonce_status);
    }

    return PM3_SUCCESS;
}

static int read_hf14a_write_magic_uid(
    volatile bool* cancel_requested,
    const iso14a_card_select_t* source_card,
    char* out_msg,
    size_t out_msg_size) {
    if(!source_card || source_card->uidlen == 0) {
        snprintf(out_msg, out_msg_size, "No source UID cached");
        return PM3_EINVARG;
    }

    uint16_t magic_flags = MAGIC_FLAG_NONE;
    read_hf14a_probe_magic_and_prng(cancel_requested, true, &magic_flags, NULL);

    uint8_t block0[16] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x04, 0x00,
        0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69};
    if(source_card->uidlen == 4) {
        memcpy(block0, source_card->uid, 4);
        block0[4] = block0[0] ^ block0[1] ^ block0[2] ^ block0[3];
        block0[5] = source_card->sak ? source_card->sak : 0x08;
        block0[6] = source_card->atqa[0] ? source_card->atqa[0] : 0x04;
        block0[7] = source_card->atqa[1];
    } else if(source_card->uidlen == 7) {
        memcpy(block0, source_card->uid, 7);
        block0[7] = source_card->sak ? source_card->sak : 0x08;
        block0[8] = source_card->atqa[0] ? source_card->atqa[0] : 0x44;
        block0[9] = source_card->atqa[1];
    }

    PacketResponseNG* resp = malloc(sizeof(PacketResponseNG));
    if(!resp) {
        return PM3_EMALLOC;
    }

    int rc = PM3_ESOFT;
    if(magic_flags & (MAGIC_FLAG_GEN_1A | MAGIC_FLAG_GEN_1B)) {
        uint8_t payload[sizeof(mf_chinese_blk_t) + 16];
        mf_chinese_blk_t* cblk = (mf_chinese_blk_t*)payload;
        cblk->params = MAGIC_SINGLE | MAGIC_WUPC;
        cblk->blockno = 0;
        memcpy(cblk->data, block0, 16);

        clearCommandBuffer();
        SendCommandNG(CMD_HF_MIFARE_CSETBL, payload, sizeof(payload));
        if(read_hf14a_wait_for_response_interruptible(
               CMD_HF_MIFARE_CSETBL, resp, 2000, cancel_requested) == PM3_SUCCESS &&
           resp->status == PM3_SUCCESS) {
            snprintf(out_msg, out_msg_size, "Wrote UID to Gen1a Blk0!");
            rc = PM3_SUCCESS;
        } else {
            snprintf(out_msg, out_msg_size, "Gen1a Blk0 write failed");
        }
    } else if(magic_flags & MAGIC_FLAG_GEN_3) {
        uint8_t payload[sizeof(mf_gen3blk_t) + 16];
        mf_gen3blk_t* g3 = (mf_gen3blk_t*)payload;
        g3->blocklen = 16;
        memcpy(g3->block, block0, 16);

        clearCommandBuffer();
        SendCommandNG(CMD_HF_MIFARE_GEN3BLK, payload, sizeof(payload));
        if(read_hf14a_wait_for_response_interruptible(
               CMD_HF_MIFARE_GEN3BLK, resp, 2000, cancel_requested) == PM3_SUCCESS &&
           resp->status == PM3_SUCCESS) {
            snprintf(out_msg, out_msg_size, "Wrote UID to Gen3 Blk0!");
            rc = PM3_SUCCESS;
        } else {
            snprintf(out_msg, out_msg_size, "Gen3 Blk0 write failed");
        }
    } else {
        mf_writeblock_ex_t wcmd = {
            .wakeup = MF_WAKE_WUPA,
            .auth_cmd = MIFARE_AUTH_KEYA,
            .key = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
            .write_cmd = ISO14443A_CMD_WRITEBLOCK,
            .block_no = 0,
        };
        memcpy(wcmd.block_data, block0, 16);

        clearCommandBuffer();
        SendCommandNG(CMD_HF_MIFARE_WRITEBL_EX, (uint8_t*)&wcmd, sizeof(wcmd));
        if(read_hf14a_wait_for_response_interruptible(
               CMD_HF_MIFARE_WRITEBL_EX, resp, 2000, cancel_requested) == PM3_SUCCESS &&
           resp->status == PM3_SUCCESS) {
            snprintf(out_msg, out_msg_size, "Wrote UID to Gen2/CUID!");
            rc = PM3_SUCCESS;
        } else {
            wcmd.auth_cmd = MIFARE_AUTH_KEYB;
            clearCommandBuffer();
            SendCommandNG(CMD_HF_MIFARE_WRITEBL_EX, (uint8_t*)&wcmd, sizeof(wcmd));
            if(read_hf14a_wait_for_response_interruptible(
                   CMD_HF_MIFARE_WRITEBL_EX, resp, 2000, cancel_requested) == PM3_SUCCESS &&
               resp->status == PM3_SUCCESS) {
                snprintf(out_msg, out_msg_size, "Wrote UID to Gen2 (KeyB)!");
                rc = PM3_SUCCESS;
            } else {
                snprintf(out_msg, out_msg_size, "No writable Magic card");
            }
        }
    }

    free(resp);
    return rc;
}

static bool read_hf14a_save_nfc_file(
    const iso14a_card_select_t* card,
    char* out_msg,
    size_t out_msg_size) {
    if(!card || card->uidlen == 0) {
        snprintf(out_msg, out_msg_size, "Scan a card first!");
        return false;
    }

    uint8_t uidlen = card->uidlen > sizeof(card->uid) ? sizeof(card->uid) : card->uidlen;
    char uid_compact[24] = {0};
    char uid_spaced[36] = {0};
    for(uint8_t i = 0; i < uidlen; i++) {
        snprintf(&uid_compact[i * 2], sizeof(uid_compact) - (i * 2), "%02X", card->uid[i]);
        snprintf(
            &uid_spaced[i * 3],
            sizeof(uid_spaced) - (i * 3),
            i + 1 < uidlen ? "%02X " : "%02X",
            card->uid[i]);
    }

    char path[64];
    snprintf(path, sizeof(path), "/ext/nfc/PM5_%s.nfc", uid_compact);

    char content[256];
    int len = snprintf(
        content,
        sizeof(content),
        "Filetype: Flipper NFC device\n"
        "Version: 4\n"
        "# Generated by Proxmark5 Companion v2.0\n"
        "Device type: ISO14443-3A\n"
        "UID: %s\n"
        "ATQA: %02X %02X\n"
        "SAK: %02X\n",
        uid_spaced,
        card->atqa[0],
        card->atqa[1],
        card->sak);

    if(len <= 0 || (size_t)len >= sizeof(content)) {
        snprintf(out_msg, out_msg_size, "Format error");
        return false;
    }

    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, "/ext/nfc");
    File* file = storage_file_alloc(storage);
    bool ok = false;
    if(storage_file_open(file, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        uint16_t written = storage_file_write(file, content, (uint16_t)len);
        ok = (written == (uint16_t)len);
        storage_file_close(file);
    }
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);

    if(ok) {
        snprintf(out_msg, out_msg_size, "Saved PM5_%s.nfc", uid_compact);
        FURI_LOG_I("PM5_QA_HF14A", "Saved %s", path);
    } else {
        snprintf(out_msg, out_msg_size, "SD write failed");
    }
    return ok;
}

static const char* hf14a_get_card_type(const iso14a_card_select_t* card) {
    uint16_t atqa = ((uint16_t)card->atqa[1] << 8) | card->atqa[0];
    switch(card->sak) {
    case 0x00:
        return "Ultralight / NTAG";
    case 0x01:
        return "TNP3xxx (Toy Tag)";
    case 0x08:
        return "MIFARE Classic 1K";
    case 0x09:
        return "MIFARE Classic Mini";
    case 0x10:
    case 0x11:
        return "MIFARE Plus (SL2)";
    case 0x18:
        return "MIFARE Classic 4K";
    case 0x20:
        if(atqa == 0x0344) {
            return "MIFARE DESFire";
        }
        return "ISO14443-4 SmartCard";
    case 0x28:
        return "SmartMX / JCOP (1K)";
    case 0x38:
        return "SmartMX / JCOP (4K)";
    case 0x88:
        return "Infineon MIFARE 1K";
    default:
        return "ISO14443-A Tag";
    }
}

static const char* hf14a_get_magic_name(uint16_t flags) {
    if(flags & MAGIC_FLAG_GEN_4GTU) return "Gen4 GTU";
    if(flags & (MAGIC_FLAG_GDM_AUTH | MAGIC_FLAG_GDM_WUP_20 | MAGIC_FLAG_GDM_WUP_40))
        return "Gen4 GDM";
    if(flags & MAGIC_FLAG_GEN_3) return "Gen3 APDU";
    if(flags & MAGIC_FLAG_GEN_2) return "Gen2 CUID";
    if(flags & MAGIC_FLAG_GEN_1A) return "Gen1a";
    if(flags & MAGIC_FLAG_GEN_1B) return "Gen1b";
    if(flags & MAGIC_FLAG_NTAG21X) return "MagicNTAG";
    if(flags & (MAGIC_FLAG_SUPER_GEN1 | MAGIC_FLAG_SUPER_GEN2)) return "SuperCard";
    if(flags & MAGIC_FLAG_GEN_UNFUSED) return "Unfused";
    return "Std";
}

static const char* hf14a_get_prng_name(uint8_t nonce_status) {
    if(nonce_status == NONCE_STATIC) return "Static!";
    if(nonce_status == NONCE_STATIC_ENC) return "StatEnc!";
    if(nonce_status == NONCE_NORMAL) return "Normal";
    return "-";
}

static void format_uid_hex(const iso14a_card_select_t* card, char* out, size_t out_size) {
    uint8_t uidlen = card->uidlen > sizeof(card->uid) ? sizeof(card->uid) : card->uidlen;
    out[0] = '\0';
    for(uint8_t i = 0; i < uidlen && (size_t)(i * 2 + 2) < out_size; i++) {
        snprintf(&out[i * 2], out_size - (i * 2), "%02X", card->uid[i]);
    }
}

static void read_hf14a_page_draw_callback(Canvas* canvas, void* context) {
    ReadHf14aPageModel* model = context;
    if(!model) {
        return;
    }

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 4, 11, "HF 14a & Magic Lab");
    canvas_set_font(canvas, FontSecondary);

    switch(model->state) {
    case ReadHf14aPageStateIdle:
        canvas_draw_str(canvas, 4, 28, "Press Scan to read 14A tag,");
        canvas_draw_str(canvas, 4, 38, "detect Magic Gen & PRNG.");
        if(model->action_msg[0]) {
            canvas_draw_str(canvas, 4, 49, model->action_msg);
        }
        break;
    case ReadHf14aPageStateRunning:
        canvas_draw_str(canvas, 4, 30, "Scanning 14A + Magic + PRNG...");
        break;
    case ReadHf14aPageStateWritingMagic:
        canvas_draw_str(canvas, 4, 30, "Writing Block 0 to Magic tag...");
        break;
    case ReadHf14aPageStateSuccess: {
        char uid_hex[2 * sizeof(model->card.uid) + 1];
        format_uid_hex(&model->card, uid_hex, sizeof(uid_hex));
        char line[44];
        snprintf(
            line,
            sizeof(line),
            "%s (SAK:%02X)",
            hf14a_get_card_type(&model->card),
            model->card.sak);
        canvas_draw_str(canvas, 4, 21, line);
        snprintf(line, sizeof(line), "UID: %s", uid_hex);
        canvas_draw_str(canvas, 4, 30, line);
        snprintf(
            line,
            sizeof(line),
            "Magic: %s  PRNG: %s",
            hf14a_get_magic_name(model->magic_flags),
            hf14a_get_prng_name(model->nonce_status));
        canvas_draw_str(canvas, 4, 39, line);
        if(model->action_msg[0]) {
            canvas_draw_str(canvas, 4, 49, model->action_msg);
        } else {
            canvas_draw_str(canvas, 4, 49, "[Up] Clone UID  [Right] .nfc");
        }
        break;
    }
    case ReadHf14aPageStateNotFound:
        canvas_draw_str(canvas, 4, 26, "No 14A card in field");
        if(model->has_cached_card) {
            char uid_hex[2 * sizeof(model->card.uid) + 1];
            format_uid_hex(&model->card, uid_hex, sizeof(uid_hex));
            char line[44];
            snprintf(line, sizeof(line), "Cached UID: %s", uid_hex);
            canvas_draw_str(canvas, 4, 37, line);
        } else {
            canvas_draw_str(canvas, 4, 37, "Place card on HF coil & Scan");
        }
        if(model->action_msg[0]) {
            canvas_draw_str(canvas, 4, 48, model->action_msg);
        }
        break;
    case ReadHf14aPageStateTimeout:
        canvas_draw_str(canvas, 4, 34, "Device not responding");
        break;
    case ReadHf14aPageStateError:
        canvas_draw_str(canvas, 4, 34, "Scan failed");
        break;
    default:
        break;
    }

    elements_button_left(canvas, "Back");
    if(model->state != ReadHf14aPageStateRunning &&
       model->state != ReadHf14aPageStateWritingMagic) {
        elements_button_center(canvas, "Scan");
        if(model->has_cached_card) {
            elements_button_right(canvas, ".nfc");
        }
    }
}

static bool read_hf14a_page_input_callback(InputEvent* event, void* context) {
    ReadHf14aPage* read_hf14a_page = context;
    if(!read_hf14a_page || !event) {
        return false;
    }

    if(event->type == InputTypeShort && event->key == InputKeyLeft) {
        read_hf14a_page_stop(read_hf14a_page);
        if(read_hf14a_page->back_callback) {
            read_hf14a_page->back_callback(read_hf14a_page->back_callback_context);
        }
        return true;
    }

    if(event->type == InputTypeShort && event->key == InputKeyOk) {
        if(!read_hf14a_page->worker_thread_running) {
            read_hf14a_page_start_task(read_hf14a_page, ReadHf14aWorkerTaskScan);
            return true;
        }
    }

    if(event->type == InputTypeShort && event->key == InputKeyRight) {
        if(!read_hf14a_page->worker_thread_running) {
            ReadHf14aPageModel* model = view_get_model(read_hf14a_page->view);
            if(model && model->has_cached_card) {
                read_hf14a_save_nfc_file(
                    &model->card, model->action_msg, sizeof(model->action_msg));
            }
            view_commit_model(read_hf14a_page->view, true);
            return true;
        }
    }

    if(event->type == InputTypeShort && event->key == InputKeyUp) {
        if(!read_hf14a_page->worker_thread_running) {
            ReadHf14aPageModel* model = view_get_model(read_hf14a_page->view);
            bool has_card = model && model->has_cached_card;
            view_commit_model(read_hf14a_page->view, false);
            if(has_card) {
                read_hf14a_page_start_task(read_hf14a_page, ReadHf14aWorkerTaskWriteMagic);
            }
            return true;
        }
    }

    return false;
}

static void read_hf14a_page_cleanup_worker(ReadHf14aPage* read_hf14a_page) {
    if(!read_hf14a_page || !read_hf14a_page->worker_thread) {
        return;
    }

    if(read_hf14a_page->worker_thread_running) {
        return;
    }

    furi_thread_join(read_hf14a_page->worker_thread);
    furi_thread_free(read_hf14a_page->worker_thread);
    read_hf14a_page->worker_thread = NULL;
}

static int32_t read_hf14a_page_worker(void* context) {
    ReadHf14aPage* read_hf14a_page = context;
    if(!read_hf14a_page) {
        return 0;
    }

    if(read_hf14a_page->worker_task == ReadHf14aWorkerTaskWriteMagic) {
        iso14a_card_select_t cached_card;
        ReadHf14aPageModel* model = view_get_model(read_hf14a_page->view);
        cached_card = model->card;
        view_commit_model(read_hf14a_page->view, false);

        char msg[40] = {0};
        read_hf14a_write_magic_uid(
            &read_hf14a_page->worker_thread_cancel_requested, &cached_card, msg, sizeof(msg));

        model = view_get_model(read_hf14a_page->view);
        model->state = ReadHf14aPageStateSuccess;
        strncpy(model->action_msg, msg, sizeof(model->action_msg) - 1);
        model->action_msg[sizeof(model->action_msg) - 1] = '\0';
        view_commit_model(read_hf14a_page->view, true);

        read_hf14a_page->worker_thread_running = false;
        return 0;
    }

    iso14a_card_select_t card;
    bool found = false;
    uint16_t magic_flags = MAGIC_FLAG_NONE;
    uint8_t nonce_status = 0;
    int result = read_hf14a_test(
        &read_hf14a_page->worker_thread_cancel_requested,
        &card,
        &found,
        &magic_flags,
        &nonce_status);

    ReadHf14aPageModel* model = view_get_model(read_hf14a_page->view);
    if(result == PM3_SUCCESS) {
        if(found) {
            model->state = ReadHf14aPageStateSuccess;
            model->card = card;
            model->has_cached_card = true;
            model->magic_flags = magic_flags;
            model->nonce_status = nonce_status;
            model->action_msg[0] = '\0';
            FURI_LOG_I(
                "PM5_QA_HF14A",
                "Card found: SAK=0x%02X ATQA=%02X%02X uidlen=%u magic=0x%04X prng=%u",
                card.sak,
                card.atqa[1],
                card.atqa[0],
                card.uidlen,
                magic_flags,
                nonce_status);
        } else {
            model->state = ReadHf14aPageStateNotFound;
            model->action_msg[0] = '\0';
            FURI_LOG_I("PM5_QA_HF14A", "OK (No card in field)");
        }
    } else if(result == PM3_EOPABORTED) {
        model->state = ReadHf14aPageStateIdle;
    } else if(result == PM3_ETIMEOUT) {
        model->state = ReadHf14aPageStateTimeout;
        FURI_LOG_W("PM5_QA_HF14A", "TIMEOUT");
    } else {
        model->state = ReadHf14aPageStateError;
        FURI_LOG_W("PM5_QA_HF14A", "ERROR res=%d", result);
    }
    view_commit_model(read_hf14a_page->view, true);

    read_hf14a_page->worker_thread_running = false;
    return 0;
}

ReadHf14aPage*
    read_hf14a_page_create(ReadHf14aPageBackCallback back_callback, void* back_callback_context) {
    ReadHf14aPage* read_hf14a_page = calloc(1, sizeof(ReadHf14aPage));
    if(!read_hf14a_page) {
        return NULL;
    }

    read_hf14a_page->view = view_alloc();
    if(!read_hf14a_page->view) {
        free(read_hf14a_page);
        return NULL;
    }

    read_hf14a_page->back_callback = back_callback;
    read_hf14a_page->back_callback_context = back_callback_context;

    view_set_context(read_hf14a_page->view, read_hf14a_page);
    view_allocate_model(read_hf14a_page->view, ViewModelTypeLocking, sizeof(ReadHf14aPageModel));
    view_set_draw_callback(read_hf14a_page->view, read_hf14a_page_draw_callback);
    view_set_input_callback(read_hf14a_page->view, read_hf14a_page_input_callback);

    ReadHf14aPageModel* model = view_get_model(read_hf14a_page->view);
    memset(model, 0, sizeof(*model));
    model->state = ReadHf14aPageStateIdle;
    view_commit_model(read_hf14a_page->view, false);

    return read_hf14a_page;
}

void read_hf14a_page_free(ReadHf14aPage* read_hf14a_page) {
    if(!read_hf14a_page) {
        return;
    }

    read_hf14a_page->worker_thread_cancel_requested = true;

    if(read_hf14a_page->worker_thread) {
        furi_thread_join(read_hf14a_page->worker_thread);
        furi_thread_free(read_hf14a_page->worker_thread);
        read_hf14a_page->worker_thread = NULL;
        read_hf14a_page->worker_thread_running = false;
    }

    if(read_hf14a_page->view) {
        view_set_context(read_hf14a_page->view, NULL);
        view_free_model(read_hf14a_page->view);
        view_free(read_hf14a_page->view);
        read_hf14a_page->view = NULL;
    }

    free(read_hf14a_page);
}

View* read_hf14a_page_get_view(ReadHf14aPage* read_hf14a_page) {
    return read_hf14a_page ? read_hf14a_page->view : NULL;
}

static void read_hf14a_page_start_task(ReadHf14aPage* read_hf14a_page, ReadHf14aWorkerTask task) {
    if(!read_hf14a_page) {
        return;
    }

    read_hf14a_page_cleanup_worker(read_hf14a_page);
    if(read_hf14a_page->worker_thread_running) {
        return;
    }

    read_hf14a_page->worker_task = task;
    ReadHf14aPageModel* model = view_get_model(read_hf14a_page->view);
    model->state = (task == ReadHf14aWorkerTaskWriteMagic) ? ReadHf14aPageStateWritingMagic :
                                                             ReadHf14aPageStateRunning;
    view_commit_model(read_hf14a_page->view, true);

    read_hf14a_page->worker_thread_cancel_requested = false;
    read_hf14a_page->worker_thread_running = true;
    read_hf14a_page->worker_thread =
        furi_thread_alloc_ex("ReadHf14aWorker", 3072, read_hf14a_page_worker, read_hf14a_page);
    furi_thread_start(read_hf14a_page->worker_thread);
}

void read_hf14a_page_start(ReadHf14aPage* read_hf14a_page) {
    read_hf14a_page_start_task(read_hf14a_page, ReadHf14aWorkerTaskScan);
}

void read_hf14a_page_stop(ReadHf14aPage* read_hf14a_page) {
    if(!read_hf14a_page) {
        return;
    }

    if(read_hf14a_page->worker_thread && read_hf14a_page->worker_thread_running) {
        read_hf14a_page->worker_thread_cancel_requested = true;
        furi_thread_join(read_hf14a_page->worker_thread);
        read_hf14a_page->worker_thread_running = false;
    }

    read_hf14a_page_cleanup_worker(read_hf14a_page);

    ReadHf14aPageModel* model = view_get_model(read_hf14a_page->view);
    model->state = ReadHf14aPageStateIdle;
    view_commit_model(read_hf14a_page->view, false);
}
