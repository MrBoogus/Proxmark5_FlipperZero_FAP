#include <furi.h>
#include <gui/canvas.h>
#include <gui/elements.h>
#include <storage/storage.h>
#include <stdio.h>
#include <string.h>
#include "lf_lab_page.h"
#include "proxmark5_com.h"
#include "proxmark5_frame.h"
#include "pm3_cmd.h"

typedef enum {
    LfLabModeReadEm410x = 0,
    LfLabModeReadHidProx = 1,
    LfLabModeCloneEmToT5577 = 2,
    LfLabModeCloneHidToT5577 = 3,
    LfLabModeResetT5577Blk0 = 4,
    LfLabModeCount = 5,
} LfLabMode;

typedef enum {
    LfLabStateIdle,
    LfLabStateRunning,
    LfLabStateSuccess,
    LfLabStateNotFound,
    LfLabStateError,
} LfLabState;

typedef struct {
    LfLabState state;
    LfLabMode mode;
    bool has_em_id;
    uint8_t em_id[5];
    bool has_hid_id;
    uint32_t hid_hi;
    uint32_t hid_lo;
    char detail_line[44];
    char status_msg[44];
} LfLabModel;

struct LfLabPage {
    View* view;
    FuriThread* worker_thread;
    volatile bool worker_running;
    volatile bool cancel_requested;

    LfLabPageBackCallback back_callback;
    void* back_callback_context;
};

static const char* const k_lf_mode_names[LfLabModeCount] = {
    "1. Read EM410x (125k)",
    "2. Read HID Prox (FSK)",
    "3. Clone EM -> T5577",
    "4. Clone HID -> T5577",
    "5. Init T5577 Block 0",
};

static int lf_lab_wait_interruptible(
    uint16_t cmd,
    PacketResponseNG* resp,
    uint32_t timeout_ms,
    volatile bool* cancel_requested) {
    uint32_t start = furi_get_tick();
    while((furi_get_tick() - start) < timeout_ms) {
        proxmark5_com_keep_fast_poll();
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

static uint8_t hex_nibble(char c) {
    if(c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if(c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
    if(c >= 'A' && c <= 'F') return (uint8_t)(c - 'A' + 10);
    return 0xFF;
}

static bool parse_em410x_log(const char* log_str, uint8_t out_id[5]) {
    const char* p = strstr(log_str, "EM TAG ID:");
    if(!p) return false;
    p += strlen("EM TAG ID:");
    while(*p == ' ') p++;

    for(int i = 0; i < 5; i++) {
        uint8_t hi = hex_nibble(p[i * 2]);
        uint8_t lo = hex_nibble(p[i * 2 + 1]);
        if(hi == 0xFF || lo == 0xFF) return false;
        out_id[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static bool parse_hid_log(const char* log_str, uint32_t* out_hi, uint32_t* out_lo) {
    const char* p = strstr(log_str, "TAG ID:");
    if(!p) return false;
    p += strlen("TAG ID:");
    while(*p == ' ') p++;

    uint64_t val = 0;
    int digits = 0;
    while(hex_nibble(*p) != 0xFF && digits < 16) {
        val = (val << 4) | hex_nibble(*p);
        p++;
        digits++;
    }
    if(digits < 8) return false;
    *out_hi = (uint32_t)(val >> 32);
    *out_lo = (uint32_t)(val & 0xFFFFFFFFULL);
    return true;
}

static bool lf_lab_save_rfid_file(const uint8_t em_id[5], char* out_msg, size_t out_msg_size) {
    char id_hex[16];
    snprintf(
        id_hex,
        sizeof(id_hex),
        "%02X%02X%02X%02X%02X",
        em_id[0],
        em_id[1],
        em_id[2],
        em_id[3],
        em_id[4]);

    char path[64];
    snprintf(path, sizeof(path), "/ext/lfrfid/PM5_EM_%s.rfid", id_hex);

    char content[160];
    int len = snprintf(
        content,
        sizeof(content),
        "Filetype: Flipper RFID key\n"
        "Version: 1\n"
        "Key type: EM4100\n"
        "Data: %02X %02X %02X %02X %02X\n",
        em_id[0],
        em_id[1],
        em_id[2],
        em_id[3],
        em_id[4]);

    if(len <= 0 || (size_t)len >= sizeof(content)) {
        snprintf(out_msg, out_msg_size, "Format error");
        return false;
    }

    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, "/ext/lfrfid");
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
        snprintf(out_msg, out_msg_size, "Saved PM5_EM_%s.rfid", id_hex);
        FURI_LOG_I("PM5_QA_LFLAB", "Saved %s", path);
    } else {
        snprintf(out_msg, out_msg_size, "SD write failed");
    }
    return ok;
}

static void lf_lab_page_draw_callback(Canvas* canvas, void* context) {
    LfLabModel* model = context;
    if(!model) {
        return;
    }

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 11, "LF Multi-Cloner Lab");
    canvas_set_font(canvas, FontSecondary);

    char line[48];
    snprintf(line, sizeof(line), "> %s", k_lf_mode_names[model->mode]);
    canvas_draw_str(canvas, 2, 22, line);

    if(model->state == LfLabStateRunning) {
        canvas_draw_str(canvas, 4, 34, "Running LF operation...");
        canvas_draw_str(canvas, 4, 44, "Place LF tag on PM5 coil");
    } else {
        if(model->has_em_id) {
            snprintf(
                line,
                sizeof(line),
                "EM410x: %02X%02X%02X%02X%02X",
                model->em_id[0],
                model->em_id[1],
                model->em_id[2],
                model->em_id[3],
                model->em_id[4]);
            canvas_draw_str(canvas, 4, 32, line);
        } else {
            canvas_draw_str(canvas, 4, 32, "EM410x: (none cached)");
        }

        if(model->has_hid_id) {
            snprintf(
                line,
                sizeof(line),
                "HID: %02lX%08lX",
                (unsigned long)model->hid_hi,
                (unsigned long)model->hid_lo);
            canvas_draw_str(canvas, 4, 41, line);
        } else if(model->detail_line[0]) {
            canvas_draw_str(canvas, 4, 41, model->detail_line);
        } else {
            canvas_draw_str(canvas, 4, 41, "Up/Dn: Mode   OK: Run");
        }

        if(model->status_msg[0]) {
            canvas_draw_str(canvas, 4, 50, model->status_msg);
        }
    }

    elements_button_left(canvas, "Back");
    if(model->state != LfLabStateRunning) {
        elements_button_center(canvas, "Run");
        if(model->has_em_id) {
            elements_button_right(canvas, ".rfid");
        }
    }
}

static bool lf_lab_page_input_callback(InputEvent* event, void* context) {
    LfLabPage* page = context;
    if(!page || !event) {
        return false;
    }

    if(event->type == InputTypeShort && event->key == InputKeyLeft) {
        lf_lab_page_stop(page);
        if(page->back_callback) {
            page->back_callback(page->back_callback_context);
        }
        return true;
    }

    if((event->type == InputTypeShort || event->type == InputTypeRepeat) &&
       (event->key == InputKeyUp || event->key == InputKeyDown)) {
        if(!page->worker_running) {
            LfLabModel* model = view_get_model(page->view);
            if(event->key == InputKeyUp) {
                model->mode =
                    (model->mode == 0) ? (LfLabMode)(LfLabModeCount - 1) : (LfLabMode)(model->mode - 1);
            } else {
                model->mode = (LfLabMode)((model->mode + 1) % LfLabModeCount);
            }
            view_commit_model(page->view, true);
            return true;
        }
    }

    if(event->type == InputTypeShort && event->key == InputKeyOk) {
        if(!page->worker_running) {
            lf_lab_page_start(page);
            return true;
        }
    }

    if(event->type == InputTypeShort && event->key == InputKeyRight) {
        if(!page->worker_running) {
            LfLabModel* model = view_get_model(page->view);
            if(model && model->has_em_id) {
                lf_lab_save_rfid_file(
                    model->em_id, model->status_msg, sizeof(model->status_msg));
            }
            view_commit_model(page->view, true);
            return true;
        }
    }

    return false;
}

static void lf_lab_cleanup_worker(LfLabPage* page) {
    if(!page || !page->worker_thread || page->worker_running) {
        return;
    }
    furi_thread_join(page->worker_thread);
    furi_thread_free(page->worker_thread);
    page->worker_thread = NULL;
}

static int32_t lf_lab_worker(void* context) {
    LfLabPage* page = context;
    if(!page) {
        return 0;
    }

    PacketResponseNG* resp = malloc(sizeof(PacketResponseNG));
    if(!resp) {
        page->worker_running = false;
        return 0;
    }

    LfLabModel* model = view_get_model(page->view);
    LfLabMode mode = model->mode;
    uint8_t em_id[5];
    memcpy(em_id, model->em_id, 5);
    bool has_em = model->has_em_id;
    uint32_t hid_hi = model->hid_hi;
    uint32_t hid_lo = model->hid_lo;
    bool has_hid = model->has_hid_id;
    view_commit_model(page->view, false);

    if(mode == LfLabModeReadEm410x || mode == LfLabModeReadHidProx) {
        uint16_t watch_cmd =
            (mode == LfLabModeReadEm410x) ? CMD_LF_EM410X_WATCH : CMD_LF_HID_WATCH;
        clearCommandBuffer();
        SendCommandNG(watch_cmd, NULL, 0);

        bool found = false;
        char log_line[160] = {0};
        uint32_t start = furi_get_tick();
        bool break_armed = false;
        while((furi_get_tick() - start) < 4200 && !page->cancel_requested) {
            proxmark5_com_keep_fast_poll();
            if(!break_armed && (furi_get_tick() - start) >= 250) {
                // Once pass 1 of DoAcquisition_default has started on PM5, arm
                // 0x55 break-poll so lf_em410x_watch/lf_hid_watch exits cleanly
                // immediately after completing pass 1.
                proxmark5_com_set_break_loop(true);
                break_armed = true;
            }
            if(proxmark5_frame_pop_log(log_line, sizeof(log_line))) {
                if(mode == LfLabModeReadEm410x && parse_em410x_log(log_line, em_id)) {
                    found = true;
                    has_em = true;
                } else if(
                    mode == LfLabModeReadHidProx && parse_hid_log(log_line, &hid_hi, &hid_lo)) {
                    found = true;
                    has_hid = true;
                }
            }
            if(proxmark5_frame_take_by_cmd(watch_cmd, resp)) {
                break;
            }
            furi_delay_ms(20);
        }
        proxmark5_com_set_break_loop(false);

        model = view_get_model(page->view);
        if(found) {
            model->state = LfLabStateSuccess;
            if(mode == LfLabModeReadEm410x) {
                memcpy(model->em_id, em_id, 5);
                model->has_em_id = true;
                snprintf(model->status_msg, sizeof(model->status_msg), "EM410x tag captured!");
                FURI_LOG_I(
                    "PM5_QA_LFLAB",
                    "EM410x found: %02X%02X%02X%02X%02X",
                    em_id[0],
                    em_id[1],
                    em_id[2],
                    em_id[3],
                    em_id[4]);
            } else {
                model->hid_hi = hid_hi;
                model->hid_lo = hid_lo;
                model->has_hid_id = true;
                snprintf(model->status_msg, sizeof(model->status_msg), "HID Prox tag captured!");
                FURI_LOG_I(
                    "PM5_QA_LFLAB",
                    "HID found: %02lX%08lX",
                    (unsigned long)hid_hi,
                    (unsigned long)hid_lo);
            }
        } else {
            model->state = LfLabStateNotFound;
            snprintf(
                model->status_msg,
                sizeof(model->status_msg),
                mode == LfLabModeReadEm410x ? "No EM410x tag in field" : "No HID tag in field");
            FURI_LOG_I("PM5_QA_LFLAB", "OK (%s)", model->status_msg);
        }
        view_commit_model(page->view, true);
    } else if(mode == LfLabModeCloneEmToT5577) {
        if(!has_em) {
            // Provide a valid test ID if none scanned yet so user can test T5577 writer
            const uint8_t default_em[5] = {0x0F, 0x03, 0x68, 0x56, 0x8B};
            memcpy(em_id, default_em, 5);
            has_em = true;
        }
        struct {
            bool Q5;
            bool EM;
            bool add_electra;
            uint8_t clock;
            uint32_t high;
            uint32_t low;
        } PACKED payload = {
            .Q5 = false,
            .EM = false,
            .add_electra = false,
            .clock = 64,
            .high = (uint32_t)em_id[0],
            .low = ((uint32_t)em_id[1] << 24) | ((uint32_t)em_id[2] << 16) |
                   ((uint32_t)em_id[3] << 8) | (uint32_t)em_id[4],
        };

        clearCommandBuffer();
        SendCommandNG(CMD_LF_EM410X_CLONE, (uint8_t*)&payload, sizeof(payload));
        int rc = lf_lab_wait_interruptible(
            CMD_LF_EM410X_CLONE, resp, 2500, &page->cancel_requested);

        model = view_get_model(page->view);
        memcpy(model->em_id, em_id, 5);
        model->has_em_id = true;
        if(rc == PM3_SUCCESS && resp->status == PM3_SUCCESS) {
            model->state = LfLabStateSuccess;
            snprintf(model->status_msg, sizeof(model->status_msg), "Cloned EM410x to T5577!");
            FURI_LOG_I("PM5_QA_LFLAB", "Cloned EM410x to T5577 OK");
        } else {
            model->state = LfLabStateError;
            snprintf(model->status_msg, sizeof(model->status_msg), "T5577 EM clone failed");
            FURI_LOG_W("PM5_QA_LFLAB", "Clone EM failed rc=%d status=%d", rc, resp->status);
        }
        view_commit_model(page->view, true);
    } else if(mode == LfLabModeCloneHidToT5577) {
        if(!has_hid) {
            hid_hi = 0x20;
            hid_lo = 0x04263F8C;
            has_hid = true;
        }
        lf_hidsim_t payload = {
            .hi2 = 0,
            .hi = hid_hi,
            .lo = hid_lo,
            .longFMT = 0,
            .Q5 = false,
            .EM = false,
        };
        clearCommandBuffer();
        SendCommandNG(CMD_LF_HID_CLONE, (uint8_t*)&payload, sizeof(payload));
        int rc =
            lf_lab_wait_interruptible(CMD_LF_HID_CLONE, resp, 2500, &page->cancel_requested);

        model = view_get_model(page->view);
        model->hid_hi = hid_hi;
        model->hid_lo = hid_lo;
        model->has_hid_id = true;
        if(rc == PM3_SUCCESS && resp->status == PM3_SUCCESS) {
            model->state = LfLabStateSuccess;
            snprintf(model->status_msg, sizeof(model->status_msg), "Cloned HID to T5577!");
            FURI_LOG_I("PM5_QA_LFLAB", "Cloned HID to T5577 OK");
        } else {
            model->state = LfLabStateError;
            snprintf(model->status_msg, sizeof(model->status_msg), "T5577 HID clone failed");
            FURI_LOG_W("PM5_QA_LFLAB", "Clone HID failed rc=%d status=%d", rc, resp->status);
        }
        view_commit_model(page->view, true);
    } else if(mode == LfLabModeResetT5577Blk0) {
        t55xx_write_block_t wbl = {
            .data = 0x00148040U, // RF/64, Manchester, MaxBlock=2
            .pwd = 0,
            .blockno = 0,
            .flags = 0x20,
        };
        clearCommandBuffer();
        SendCommandNG(CMD_LF_T55XX_WRITEBL, (uint8_t*)&wbl, sizeof(wbl));
        int rc = lf_lab_wait_interruptible(
            CMD_LF_T55XX_WRITEBL, resp, 2500, &page->cancel_requested);

        model = view_get_model(page->view);
        if(rc == PM3_SUCCESS && resp->status == PM3_SUCCESS) {
            model->state = LfLabStateSuccess;
            snprintf(model->status_msg, sizeof(model->status_msg), "T5577 Blk0 = 0x00148040!");
            FURI_LOG_I("PM5_QA_LFLAB", "T5577 Block 0 init OK");
        } else {
            model->state = LfLabStateError;
            snprintf(model->status_msg, sizeof(model->status_msg), "T5577 Blk0 write failed");
            FURI_LOG_W("PM5_QA_LFLAB", "T5577 Blk0 failed rc=%d status=%d", rc, resp->status);
        }
        view_commit_model(page->view, true);
    }

    free(resp);
    page->worker_running = false;
    return 0;
}

LfLabPage* lf_lab_page_create(LfLabPageBackCallback back_callback, void* back_callback_context) {
    LfLabPage* page = calloc(1, sizeof(LfLabPage));
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
    view_allocate_model(page->view, ViewModelTypeLocking, sizeof(LfLabModel));
    view_set_draw_callback(page->view, lf_lab_page_draw_callback);
    view_set_input_callback(page->view, lf_lab_page_input_callback);

    LfLabModel* model = view_get_model(page->view);
    memset(model, 0, sizeof(*model));
    model->state = LfLabStateIdle;
    model->mode = LfLabModeReadEm410x;
    view_commit_model(page->view, false);

    return page;
}

void lf_lab_page_free(LfLabPage* page) {
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

View* lf_lab_page_get_view(LfLabPage* page) {
    return page ? page->view : NULL;
}

void lf_lab_page_start(LfLabPage* page) {
    if(!page) {
        return;
    }

    lf_lab_cleanup_worker(page);
    if(page->worker_running) {
        return;
    }

    LfLabModel* model = view_get_model(page->view);
    model->state = LfLabStateRunning;
    model->status_msg[0] = '\0';
    view_commit_model(page->view, true);

    page->cancel_requested = false;
    page->worker_running = true;
    page->worker_thread = furi_thread_alloc_ex("LfLabWorker", 3072, lf_lab_worker, page);
    furi_thread_start(page->worker_thread);
}

void lf_lab_page_stop(LfLabPage* page) {
    if(!page) {
        return;
    }

    if(page->worker_thread && page->worker_running) {
        page->cancel_requested = true;
        furi_thread_join(page->worker_thread);
        page->worker_running = false;
    }

    lf_lab_cleanup_worker(page);

    LfLabModel* model = view_get_model(page->view);
    model->state = LfLabStateIdle;
    view_commit_model(page->view, false);
}
