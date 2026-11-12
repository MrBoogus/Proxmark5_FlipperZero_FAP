#include <furi.h>
#include <gui/canvas.h>
#include <gui/elements.h>
#include <storage/storage.h>
#include <stdio.h>
#include <string.h>
#include "hf_mf_audit_page.h"
#include "proxmark5_frame.h"
#include "pm3_cmd.h"

#define MFC_NUM_SECTORS 16U

typedef enum {
    HfMfAuditStateIdle,
    HfMfAuditStateRunning,
    HfMfAuditStateDumping,
    HfMfAuditStateDone,
    HfMfAuditStateNoCard,
    HfMfAuditStateError,
} HfMfAuditState;

typedef enum {
    HfMfWorkerTaskAudit = 0,
    HfMfWorkerTaskDump = 1,
} HfMfWorkerTask;

typedef struct {
    bool found_a;
    bool found_b;
    uint8_t key_a[6];
    uint8_t key_b[6];
} MfcSectorKeys;

typedef struct {
    HfMfAuditState state;
    iso14a_card_select_t card;
    uint8_t current_sector;
    uint8_t scroll_sector;
    uint8_t total_keys_found;
    MfcSectorKeys sectors[MFC_NUM_SECTORS];
    char status_msg[48];
} HfMfAuditModel;

struct HfMfAuditPage {
    View* view;
    FuriThread* worker_thread;
    volatile bool worker_running;
    volatile bool cancel_requested;
    HfMfWorkerTask worker_task;

    HfMfAuditPageBackCallback back_callback;
    void* back_callback_context;
};

static const uint8_t k_mfc_default_keys[][6] = {
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, // Transport default
    {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5}, // MAD Key A
    {0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5}, // MAD Key B
    {0xD3, 0xF7, 0xD3, 0xF7, 0xD3, 0xF7}, // NDEF Key A
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, // Blank key
    {0x4D, 0x3A, 0x99, 0xC3, 0x51, 0xDD},
    {0x1A, 0x98, 0x2C, 0x7E, 0x45, 0x9A},
    {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF},
    {0x71, 0x4C, 0x5C, 0x88, 0x6E, 0x97},
    {0x58, 0x7E, 0xE5, 0xF9, 0x35, 0x0F},
    {0xA0, 0x47, 0x8C, 0xC3, 0x90, 0x91},
    {0x53, 0x3C, 0xB6, 0xC7, 0x23, 0xF6},
    {0x8F, 0xD0, 0xA4, 0xF2, 0x56, 0xE9},
    {0xA6, 0x45, 0x98, 0xA7, 0x74, 0x78},
    {0x26, 0x94, 0x0B, 0x21, 0xFF, 0x5D},
    {0xFC, 0x00, 0x01, 0x87, 0x78, 0xF7},
    {0x00, 0x00, 0x0F, 0xFE, 0x24, 0x88},
    {0x5C, 0x8F, 0xF9, 0x99, 0x0D, 0xA2},
    {0x75, 0xC2, 0x3C, 0x99, 0xD0, 0xE4},
    {0xE2, 0x16, 0xD6, 0x34, 0x09, 0x5E},
    {0x11, 0x22, 0x33, 0x44, 0x55, 0x66},
    {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC},
    {0xAB, 0xCD, 0xEF, 0x12, 0x34, 0x56},
    {0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6},
};

#define MFC_DICT_COUNT (sizeof(k_mfc_default_keys) / sizeof(k_mfc_default_keys[0]))

static void hf_mf_audit_page_start_task(HfMfAuditPage* page, HfMfWorkerTask task);

static int hf_mf_wait_interruptible(
    uint16_t cmd,
    PacketResponseNG* resp,
    uint32_t timeout_ms,
    volatile bool* cancel_requested) {
    uint32_t start = furi_get_tick();
    while((furi_get_tick() - start) < timeout_ms) {
        if(cancel_requested && *cancel_requested) {
            return PM3_EOPABORTED;
        }
        if(proxmark5_frame_take_by_cmd(cmd, resp)) {
            return PM3_SUCCESS;
        }
        furi_delay_ms(10);
    }
    return proxmark5_frame_take_by_cmd(cmd, resp) ? PM3_SUCCESS : PM3_ETIMEOUT;
}

static bool hf_mf_check_sector_key(
    volatile bool* cancel_requested,
    uint8_t sector,
    uint8_t key_type,
    bool clear_trace,
    PacketResponseNG* resp,
    uint8_t out_key[6]) {
    uint8_t payload[5 + MFC_DICT_COUNT * 6];
    payload[0] = key_type;
    payload[1] = (uint8_t)(sector * 4U + 3U);
    payload[2] = clear_trace ? 1U : 0U;
    payload[3] = (uint8_t)((MFC_DICT_COUNT >> 8) & 0xFFU);
    payload[4] = (uint8_t)(MFC_DICT_COUNT & 0xFFU);
    memcpy(payload + 5, k_mfc_default_keys, sizeof(k_mfc_default_keys));

    clearCommandBuffer();
    SendCommandNG(CMD_HF_MIFARE_CHKKEYS, payload, sizeof(payload));
    if(hf_mf_wait_interruptible(CMD_HF_MIFARE_CHKKEYS, resp, 2500, cancel_requested) !=
       PM3_SUCCESS) {
        return false;
    }
    if(resp->status == PM3_SUCCESS && resp->length >= 7 && resp->data.asBytes[6] != 0) {
        memcpy(out_key, resp->data.asBytes, 6);
        return true;
    }
    return false;
}

static void hf_mf_audit_page_draw_callback(Canvas* canvas, void* context) {
    HfMfAuditModel* model = context;
    if(!model) {
        return;
    }

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 11, "MFC Key Auditor (1K)");
    canvas_set_font(canvas, FontSecondary);

    switch(model->state) {
    case HfMfAuditStateIdle:
        canvas_draw_str(canvas, 4, 26, "Hardware dictionary check");
        canvas_draw_str(canvas, 4, 36, "across Sectors 0..15 (A & B)");
        canvas_draw_str(canvas, 4, 46, "Press Audit to scan card.");
        break;
    case HfMfAuditStateRunning: {
        char line[44];
        snprintf(
            line,
            sizeof(line),
            "Auditing Sector %u / 15... (%u keys)",
            model->current_sector,
            model->total_keys_found);
        canvas_draw_str(canvas, 4, 23, line);

        // Draw 16-sector visual matrix (16 boxes across X=4..116)
        for(uint8_t s = 0; s < MFC_NUM_SECTORS; s++) {
            uint8_t x = (uint8_t)(4 + s * 7);
            canvas_draw_frame(canvas, x, 27, 6, 12);
            if(model->sectors[s].found_a) {
                canvas_draw_box(canvas, x + 1, 28, 4, 4);
            }
            if(model->sectors[s].found_b) {
                canvas_draw_box(canvas, x + 1, 34, 4, 4);
            }
        }
        canvas_draw_str(canvas, 4, 49, "Top=KeyA  Bot=KeyB");
        break;
    }
    case HfMfAuditStateDumping:
        canvas_draw_str(canvas, 4, 30, "Reading sectors & saving .nfc...");
        if(model->status_msg[0]) {
            canvas_draw_str(canvas, 4, 42, model->status_msg);
        }
        break;
    case HfMfAuditStateDone: {
        // Draw compact 16-sector bar at Y=14..23
        for(uint8_t s = 0; s < MFC_NUM_SECTORS; s++) {
            uint8_t x = (uint8_t)(4 + s * 7);
            canvas_draw_frame(canvas, x, 14, 6, 9);
            if(model->sectors[s].found_a) {
                canvas_draw_box(canvas, x + 1, 15, 4, 3);
            }
            if(model->sectors[s].found_b) {
                canvas_draw_box(canvas, x + 1, 19, 4, 3);
            }
        }

        uint8_t s = model->scroll_sector < MFC_NUM_SECTORS ? model->scroll_sector : 0;
        const MfcSectorKeys* sec = &model->sectors[s];
        char line[44];
        char ka[16] = "------------";
        char kb[16] = "------------";
        if(sec->found_a) {
            snprintf(
                ka,
                sizeof(ka),
                "%02X%02X%02X%02X%02X%02X",
                sec->key_a[0],
                sec->key_a[1],
                sec->key_a[2],
                sec->key_a[3],
                sec->key_a[4],
                sec->key_a[5]);
        }
        if(sec->found_b) {
            snprintf(
                kb,
                sizeof(kb),
                "%02X%02X%02X%02X%02X%02X",
                sec->key_b[0],
                sec->key_b[1],
                sec->key_b[2],
                sec->key_b[3],
                sec->key_b[4],
                sec->key_b[5]);
        }
        snprintf(line, sizeof(line), "S%02u A:%s", s, ka);
        canvas_draw_str(canvas, 4, 32, line);
        snprintf(line, sizeof(line), "S%02u B:%s", s, kb);
        canvas_draw_str(canvas, 4, 41, line);
        if(model->status_msg[0]) {
            canvas_draw_str(canvas, 4, 50, model->status_msg);
        } else {
            snprintf(
                line,
                sizeof(line),
                "Found %u/32 keys (Up/Dn scroll)",
                model->total_keys_found);
            canvas_draw_str(canvas, 4, 50, line);
        }
        break;
    }
    case HfMfAuditStateNoCard:
        canvas_draw_str(canvas, 4, 28, "No MIFARE Classic card found");
        canvas_draw_str(canvas, 4, 40, "Place card on HF coil & Audit");
        break;
    case HfMfAuditStateError:
        canvas_draw_str(canvas, 4, 32, "Audit error / timeout");
        break;
    }

    elements_button_left(canvas, "Back");
    if(model->state != HfMfAuditStateRunning && model->state != HfMfAuditStateDumping) {
        elements_button_center(canvas, "Audit");
        if(model->state == HfMfAuditStateDone && model->total_keys_found > 0) {
            elements_button_right(canvas, "Dump");
        }
    }
}

static bool hf_mf_audit_page_input_callback(InputEvent* event, void* context) {
    HfMfAuditPage* page = context;
    if(!page || !event) {
        return false;
    }

    if(event->type == InputTypeShort && event->key == InputKeyLeft) {
        hf_mf_audit_page_stop(page);
        if(page->back_callback) {
            page->back_callback(page->back_callback_context);
        }
        return true;
    }

    if(event->type == InputTypeShort && event->key == InputKeyOk) {
        if(!page->worker_running) {
            hf_mf_audit_page_start_task(page, HfMfWorkerTaskAudit);
            return true;
        }
    }

    if(event->type == InputTypeShort && event->key == InputKeyRight) {
        if(!page->worker_running) {
            HfMfAuditModel* model = view_get_model(page->view);
            bool can_dump =
                (model && model->state == HfMfAuditStateDone && model->total_keys_found > 0);
            view_commit_model(page->view, false);
            if(can_dump) {
                hf_mf_audit_page_start_task(page, HfMfWorkerTaskDump);
            }
            return true;
        }
    }

    if((event->type == InputTypeShort || event->type == InputTypeRepeat) &&
       (event->key == InputKeyUp || event->key == InputKeyDown)) {
        HfMfAuditModel* model = view_get_model(page->view);
        if(model && model->state == HfMfAuditStateDone) {
            if(event->key == InputKeyUp) {
                model->scroll_sector = (model->scroll_sector == 0) ?
                                           (uint8_t)(MFC_NUM_SECTORS - 1U) :
                                           (uint8_t)(model->scroll_sector - 1U);
            } else {
                model->scroll_sector = (uint8_t)((model->scroll_sector + 1U) % MFC_NUM_SECTORS);
            }
        }
        view_commit_model(page->view, true);
        return true;
    }

    return false;
}

static void hf_mf_audit_cleanup_worker(HfMfAuditPage* page) {
    if(!page || !page->worker_thread || page->worker_running) {
        return;
    }
    furi_thread_join(page->worker_thread);
    furi_thread_free(page->worker_thread);
    page->worker_thread = NULL;
}

static void hf_mf_dump_to_nfc(HfMfAuditPage* page) {
    PacketResponseNG* resp = malloc(sizeof(PacketResponseNG));
    if(!resp) {
        return;
    }

    HfMfAuditModel* model = view_get_model(page->view);
    iso14a_card_select_t card = model->card;
    MfcSectorKeys sectors[MFC_NUM_SECTORS];
    memcpy(sectors, model->sectors, sizeof(sectors));
    view_commit_model(page->view, false);

    uint8_t uidlen = card.uidlen > sizeof(card.uid) ? sizeof(card.uid) : card.uidlen;
    if(uidlen == 0) uidlen = 4;
    char uid_compact[24] = {0};
    char uid_spaced[36] = {0};
    for(uint8_t i = 0; i < uidlen; i++) {
        snprintf(&uid_compact[i * 2], sizeof(uid_compact) - (i * 2), "%02X", card.uid[i]);
        snprintf(
            &uid_spaced[i * 3],
            sizeof(uid_spaced) - (i * 3),
            i + 1 < uidlen ? "%02X " : "%02X",
            card.uid[i]);
    }

    char path[64];
    snprintf(path, sizeof(path), "/ext/nfc/PM5_MFC_%s.nfc", uid_compact);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, "/ext/nfc");
    File* file = storage_file_alloc(storage);
    bool ok = false;

    if(storage_file_open(file, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        char hdr[256];
        int hlen = snprintf(
            hdr,
            sizeof(hdr),
            "Filetype: Flipper NFC device\n"
            "Version: 4\n"
            "# Generated by Proxmark5 Companion v2.0\n"
            "Device type: Mifare Classic\n"
            "UID: %s\n"
            "ATQA: %02X %02X\n"
            "SAK: %02X\n"
            "Mifare Classic type: 1K\n"
            "Data format version: 2\n",
            uid_spaced,
            card.atqa[0],
            card.atqa[1],
            card.sak ? card.sak : 0x08);
        if(hlen > 0) {
            storage_file_write(file, hdr, (uint16_t)hlen);
        }

        ok = true;
        for(uint8_t s = 0; s < MFC_NUM_SECTORS && !page->cancel_requested; s++) {
            uint8_t sector_data[64];
            memset(sector_data, 0, sizeof(sector_data));
            bool read_ok = false;

            if(sectors[s].found_a || sectors[s].found_b) {
                mf_readsector_t rcmd = {
                    .sectorno = s,
                    .keytype = sectors[s].found_a ? 0 : 1,
                };
                memcpy(
                    rcmd.key,
                    sectors[s].found_a ? sectors[s].key_a : sectors[s].key_b,
                    6);

                clearCommandBuffer();
                SendCommandNG(CMD_HF_MIFARE_READSC, (uint8_t*)&rcmd, sizeof(rcmd));
                if(hf_mf_wait_interruptible(
                       CMD_HF_MIFARE_READSC, resp, 2000, &page->cancel_requested) ==
                       PM3_SUCCESS &&
                   resp->status == PM3_SUCCESS && resp->length >= 64) {
                    memcpy(sector_data, resp->data.asBytes, 64);
                    read_ok = true;
                }
            }

            if(sectors[s].found_a) {
                memcpy(&sector_data[48], sectors[s].key_a, 6);
            }
            if(sectors[s].found_b) {
                memcpy(&sector_data[58], sectors[s].key_b, 6);
            }

            for(uint8_t b = 0; b < 4; b++) {
                uint8_t blk_idx = (uint8_t)(s * 4U + b);
                char blk_line[96];
                int pos = snprintf(blk_line, sizeof(blk_line), "Block %u:", blk_idx);
                for(uint8_t i = 0; i < 16; i++) {
                    if(!read_ok && b < 3) {
                        pos += snprintf(blk_line + pos, sizeof(blk_line) - pos, " ??");
                    } else if(b == 3 && i < 6 && !sectors[s].found_a) {
                        pos += snprintf(blk_line + pos, sizeof(blk_line) - pos, " ??");
                    } else if(b == 3 && i >= 10 && !sectors[s].found_b) {
                        pos += snprintf(blk_line + pos, sizeof(blk_line) - pos, " ??");
                    } else {
                        pos += snprintf(
                            blk_line + pos,
                            sizeof(blk_line) - pos,
                            " %02X",
                            sector_data[b * 16 + i]);
                    }
                }
                pos += snprintf(blk_line + pos, sizeof(blk_line) - pos, "\n");
                storage_file_write(file, blk_line, (uint16_t)pos);
            }
        }

        storage_file_close(file);
    }

    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    free(resp);

    model = view_get_model(page->view);
    model->state = HfMfAuditStateDone;
    if(ok) {
        snprintf(model->status_msg, sizeof(model->status_msg), "Saved PM5_MFC_%s.nfc", uid_compact);
        FURI_LOG_I("PM5_QA_MFAUDIT", "Saved %s", path);
    } else {
        snprintf(model->status_msg, sizeof(model->status_msg), "Dump save failed");
    }
    view_commit_model(page->view, true);
}

static int32_t hf_mf_audit_worker(void* context) {
    HfMfAuditPage* page = context;
    if(!page) {
        return 0;
    }

    if(page->worker_task == HfMfWorkerTaskDump) {
        hf_mf_dump_to_nfc(page);
        page->worker_running = false;
        return 0;
    }

    PacketResponseNG* resp = malloc(sizeof(PacketResponseNG));
    if(!resp) {
        page->worker_running = false;
        return 0;
    }

    // Step 1: Select 14A card first so we don't loop on an empty coil
    iso14a_raw_cmd_t sel_cmd = {
        .flags = ISO14A_CONNECT | ISO14A_CLEARTRACE,
        .timeout = 0,
        .wait_us = 0,
        .len = 0,
        .lenbits = 0,
    };
    clearCommandBuffer();
    SendCommandNG(CMD_HF_ISO14443A_READER, (uint8_t*)&sel_cmd, sizeof(sel_cmd));
    int rc = hf_mf_wait_interruptible(
        CMD_HF_ISO14443A_READER, resp, 2000, &page->cancel_requested);

    if(rc != PM3_SUCCESS || resp->status != PM3_SUCCESS ||
       resp->length < sizeof(iso14a_raw_resp_t) + sizeof(iso14a_card_select_t)) {
        HfMfAuditModel* model = view_get_model(page->view);
        model->state = (rc == PM3_EOPABORTED) ? HfMfAuditStateIdle : HfMfAuditStateError;
        view_commit_model(page->view, true);
        free(resp);
        page->worker_running = false;
        return 0;
    }

    const iso14a_raw_resp_t* hdr = (const iso14a_raw_resp_t*)resp->data.asBytes;
    if(hdr->sel == 0) {
        HfMfAuditModel* model = view_get_model(page->view);
        model->state = HfMfAuditStateNoCard;
        view_commit_model(page->view, true);
        FURI_LOG_I("PM5_QA_MFAUDIT", "OK (No card in field)");
        free(resp);
        page->worker_running = false;
        return 0;
    }

    iso14a_card_select_t card;
    memcpy(&card, resp->data.asBytes + sizeof(iso14a_raw_resp_t), sizeof(card));

    {
        HfMfAuditModel* model = view_get_model(page->view);
        model->card = card;
        memset(model->sectors, 0, sizeof(model->sectors));
        model->total_keys_found = 0;
        model->scroll_sector = 0;
        model->status_msg[0] = '\0';
        view_commit_model(page->view, true);
    }

    // Step 2: Check Key A and Key B across all 16 sectors
    for(uint8_t s = 0; s < MFC_NUM_SECTORS && !page->cancel_requested; s++) {
        {
            HfMfAuditModel* model = view_get_model(page->view);
            model->current_sector = s;
            view_commit_model(page->view, true);
        }

        uint8_t key_a[6] = {0};
        uint8_t key_b[6] = {0};
        bool found_a = hf_mf_check_sector_key(
            &page->cancel_requested, s, 0, (s == 0), resp, key_a);
        bool found_b = false;
        if(!page->cancel_requested) {
            found_b = hf_mf_check_sector_key(
                &page->cancel_requested, s, 1, false, resp, key_b);
        }

        HfMfAuditModel* model = view_get_model(page->view);
        model->sectors[s].found_a = found_a;
        if(found_a) {
            memcpy(model->sectors[s].key_a, key_a, 6);
            model->total_keys_found++;
        }
        model->sectors[s].found_b = found_b;
        if(found_b) {
            memcpy(model->sectors[s].key_b, key_b, 6);
            model->total_keys_found++;
        }
        view_commit_model(page->view, true);
    }

    free(resp);

    HfMfAuditModel* model = view_get_model(page->view);
    model->state = page->cancel_requested ? HfMfAuditStateIdle : HfMfAuditStateDone;
    FURI_LOG_I("PM5_QA_MFAUDIT", "Audit complete: found %u/32 keys", model->total_keys_found);
    view_commit_model(page->view, true);

    page->worker_running = false;
    return 0;
}

HfMfAuditPage*
    hf_mf_audit_page_create(HfMfAuditPageBackCallback back_callback, void* back_callback_context) {
    HfMfAuditPage* page = calloc(1, sizeof(HfMfAuditPage));
    if(!page) {
        return NULL;
    }

    page->view = view_alloc();
    if(!page->view) {
        free(page);
        return NULL;
    }

    page->back_callback = back_callback;
    page->back_callback_context = back_callback_context;

    view_set_context(page->view, page);
    view_allocate_model(page->view, ViewModelTypeLocking, sizeof(HfMfAuditModel));
    view_set_draw_callback(page->view, hf_mf_audit_page_draw_callback);
    view_set_input_callback(page->view, hf_mf_audit_page_input_callback);

    HfMfAuditModel* model = view_get_model(page->view);
    memset(model, 0, sizeof(*model));
    model->state = HfMfAuditStateIdle;
    view_commit_model(page->view, false);

    return page;
}

void hf_mf_audit_page_free(HfMfAuditPage* page) {
    if(!page) {
        return;
    }

    page->cancel_requested = true;
    if(page->worker_thread) {
        furi_thread_join(page->worker_thread);
        furi_thread_free(page->worker_thread);
        page->worker_thread = NULL;
        page->worker_running = false;
    }

    if(page->view) {
        view_set_context(page->view, NULL);
        view_free_model(page->view);
        view_free(page->view);
        page->view = NULL;
    }

    free(page);
}

View* hf_mf_audit_page_get_view(HfMfAuditPage* page) {
    return page ? page->view : NULL;
}

static void hf_mf_audit_page_start_task(HfMfAuditPage* page, HfMfWorkerTask task) {
    if(!page) {
        return;
    }

    hf_mf_audit_cleanup_worker(page);
    if(page->worker_running) {
        return;
    }

    page->worker_task = task;
    HfMfAuditModel* model = view_get_model(page->view);
    model->state = (task == HfMfWorkerTaskDump) ? HfMfAuditStateDumping : HfMfAuditStateRunning;
    model->current_sector = 0;
    view_commit_model(page->view, true);

    page->cancel_requested = false;
    page->worker_running = true;
    page->worker_thread =
        furi_thread_alloc_ex("HfMfAuditWorker", 3072, hf_mf_audit_worker, page);
    furi_thread_start(page->worker_thread);
}

void hf_mf_audit_page_start(HfMfAuditPage* page) {
    hf_mf_audit_page_start_task(page, HfMfWorkerTaskAudit);
}

void hf_mf_audit_page_stop(HfMfAuditPage* page) {
    if(!page) {
        return;
    }

    if(page->worker_thread && page->worker_running) {
        page->cancel_requested = true;
        furi_thread_join(page->worker_thread);
        page->worker_running = false;
    }

    hf_mf_audit_cleanup_worker(page);

    HfMfAuditModel* model = view_get_model(page->view);
    model->state = HfMfAuditStateIdle;
    view_commit_model(page->view, false);
}
