#include <furi.h>
#include <gui/canvas.h>
#include <gui/elements.h>
#include <storage/storage.h>
#include <stdio.h>
#include <string.h>
#include "t5577_suite_page.h"
#include "t5577_crypto.h"
#include "proxmark5_com.h"
#include "proxmark5_frame.h"
#include "pm3_cmd.h"

#define T5577_DICT_DIR   "/ext/apps_data/pm5/dicts"
#define T5577_DICT_PATH  "/ext/apps_data/pm5/dicts/t5577_pass.txt"
#define T5577_FOUND_PATH "/ext/apps_data/pm5/dicts/t5577_found.txt"
#define T5577_DUMP_DIR   "/ext/apps_data/pm5/dumps"

typedef enum {
    T55ModeEmClone = 0,
    T55ModeBlockRead = 1,
    T55ModeBlockWrite = 2,
    T55ModePwdCrack = 3,
    T55ModeUnbrick = 4,
    T55ModeSnoop = 5,
    T55ModeScope = 6,
    T55ModeCount = 7,
} T55Mode;

typedef enum {
    T55StateIdle,
    T55StateRunning,
    T55StateSuccess,
    T55StateNotFound,
    T55StateError,
} T55State;

typedef struct {
    T55State state;
    T55Mode mode;

    // EM4100
    uint8_t em_uid[5];
    bool has_em_uid;

    // Block Read / Dump
    uint8_t selected_block;
    uint32_t block_data[8];
    bool block_valid[8];
    char config_desc[48];

    // Block Write
    uint32_t write_data;
    uint32_t active_pwd;
    bool use_pwd;
    uint8_t downlink_mode; // 0=Fixed, 1=LongLeading, 2=Leading0, 3=1of4

    // Cracker
    uint8_t crack_method; // 0=White Cloner, 1=Dictionary
    uint32_t crack_tested;
    uint32_t crack_total;
    uint32_t crack_current_key;
    uint32_t crack_found_key;
    bool crack_found;

    // Unbrick
    uint8_t unbrick_preset; // 0=Standard EM4100, 1=Clean All0, 2=Full Wipe

    // Snoop
    bool snoop_active;
    uint32_t snoop_pwd;
    uint8_t snoop_target_blk;
    bool snoop_found;

    // LF Scope
    uint16_t scope_samples[64];
    uint8_t scope_head;
    uint32_t scope_now_mv;
    uint32_t scope_peak_mv;
    bool scope_paused;

    char status_msg[48];
} T5577SuiteModel;

struct T5577SuitePage {
    View* view;
    FuriThread* worker_thread;
    volatile bool worker_running;
    volatile bool cancel_requested;

    T5577SuitePageBackCallback back_callback;
    void* back_callback_context;
};

static const char* const k_t55_mode_titles[T55ModeCount] = {
    "1. EM4100 Clone & Build",
    "2. T5577 Block Read/Dump",
    "3. Auth Block Write",
    "4. Password Cracker",
    "5. Test Mode Unbricker",
    "6. Reader Snoop (Sniff)",
    "7. LF Signal Scope",
};

static const uint32_t k_default_passwords[] = {
    0x00000000U, // Default / Factory
    0x12345678U, // Common Generic
    0x51243648U, // Default Chinese White Cloner
    0x58494420U, // HID Global 'XID '
    0x48494450U, // HID Global 'HIDP'
    0x414D4154U, // Amatek
    0x494E444CU, // Indala
    0x43415349U, // Casi-Rusco
    0x50595241U, // Pyramid
    0x41574944U, // AWID
    0x4B455249U, // Keri
};

static void t5577_ensure_directories_and_dict(void) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    if(!storage) return;

    storage_simply_mkdir(storage, "/ext/apps_data/pm5");
    storage_simply_mkdir(storage, T5577_DICT_DIR);
    storage_simply_mkdir(storage, T5577_DUMP_DIR);
    storage_simply_mkdir(storage, "/ext/lfrfid");

    FileInfo file_info;
    if(storage_common_stat(storage, T5577_DICT_PATH, &file_info) != FSE_OK ||
       file_info.size == 0) {
        File* file = storage_file_alloc(storage);
        if(file &&
           storage_file_open(file, T5577_DICT_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
            for(size_t i = 0; i < sizeof(k_default_passwords) / sizeof(k_default_passwords[0]);
                i++) {
                char line[16];
                int len = snprintf(line, sizeof(line), "%08lX\n", (unsigned long)k_default_passwords[i]);
                storage_file_write(file, line, (uint16_t)len);
            }
            storage_file_close(file);
        }
        if(file) storage_file_free(file);
    }

    furi_record_close(RECORD_STORAGE);
}

static int t55_wait_interruptible(
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

static uint8_t hex_to_nibble(char c) {
    if(c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if(c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
    if(c >= 'A' && c <= 'F') return (uint8_t)(c - 'A' + 10);
    return 0xFF;
}

static bool parse_em410x_from_log(const char* log_str, uint8_t out_id[5]) {
    const char* p = strstr(log_str, "EM TAG ID:");
    if(!p) return false;
    p += strlen("EM TAG ID:");
    while(*p == ' ') p++;

    for(int i = 0; i < 5; i++) {
        uint8_t hi = hex_to_nibble(p[i * 2]);
        uint8_t lo = hex_to_nibble(p[i * 2 + 1]);
        if(hi == 0xFF || lo == 0xFF) return false;
        out_id[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static void t5577_suite_draw_callback(Canvas* canvas, void* context) {
    T5577SuiteModel* model = context;
    if(!model) return;

    canvas_clear(canvas);
    canvas_set_font(canvas, FontSecondary);

    // Header bar
    canvas_draw_box(canvas, 0, 0, 128, 11);
    canvas_set_color(canvas, ColorWhite);
    canvas_draw_str_aligned(
        canvas, 64, 9, AlignCenter, AlignBottom, k_t55_mode_titles[model->mode]);
    canvas_set_color(canvas, ColorBlack);

    char buf[64];

    switch(model->mode) {
    case T55ModeEmClone: {
        snprintf(
            buf,
            sizeof(buf),
            "UID: %02X %02X %02X %02X %02X",
            model->em_uid[0],
            model->em_uid[1],
            model->em_uid[2],
            model->em_uid[3],
            model->em_uid[4]);
        canvas_draw_str(canvas, 2, 23, buf);

        uint32_t card_num = ((uint32_t)model->em_uid[3] << 8) | model->em_uid[4];
        uint32_t cust_id = model->em_uid[1];
        snprintf(buf, sizeof(buf), "Cust: %lu  Card: %05lu", (unsigned long)cust_id, (unsigned long)card_num);
        canvas_draw_str(canvas, 2, 34, buf);

        canvas_draw_str(canvas, 2, 45, model->status_msg);

        // Actions
        canvas_draw_line(canvas, 0, 52, 128, 52);
        canvas_draw_str(canvas, 2, 62, "OK:Clone to T5577  R:Save SD");
        break;
    }
    case T55ModeBlockRead: {
        snprintf(
            buf,
            sizeof(buf),
            "Block [%u]: %08lX %s",
            model->selected_block,
            (unsigned long)model->block_data[model->selected_block],
            model->block_valid[model->selected_block] ? "[OK]" : "[--]");
        canvas_draw_str(canvas, 2, 23, buf);

        if(model->block_valid[0]) {
            snprintf(buf, sizeof(buf), "Cfg: %s", model->config_desc);
            canvas_draw_str(canvas, 2, 34, buf);
        } else {
            canvas_draw_str(canvas, 2, 34, "Cfg: (Read Blk 0 to decode)");
        }

        canvas_draw_str(canvas, 2, 45, model->status_msg);

        canvas_draw_line(canvas, 0, 52, 128, 52);
        canvas_draw_str(canvas, 2, 62, "OK:Read Blk  R:Dump All");
        break;
    }
    case T55ModeBlockWrite: {
        snprintf(
            buf,
            sizeof(buf),
            "Blk [%u]: %08lX (r%u)",
            model->selected_block,
            (unsigned long)model->write_data,
            model->downlink_mode);
        canvas_draw_str(canvas, 2, 23, buf);

        snprintf(
            buf,
            sizeof(buf),
            "Pwd: %08lX  Mode: %s",
            (unsigned long)model->active_pwd,
            model->use_pwd ? "AUTH" : "PLAIN");
        canvas_draw_str(canvas, 2, 34, buf);

        canvas_draw_str(canvas, 2, 45, model->status_msg);

        canvas_draw_line(canvas, 0, 52, 128, 52);
        canvas_draw_str(canvas, 2, 62, "OK:Write  R:Toggle Pwd");
        break;
    }
    case T55ModePwdCrack: {
        snprintf(
            buf,
            sizeof(buf),
            "Method: %s",
            model->crack_method == 0 ? "White Cloner Algo" : "Dictionary Stream");
        canvas_draw_str(canvas, 2, 23, buf);

        if(model->crack_method == 1 && model->crack_total > 0) {
            elements_progress_bar(
                canvas,
                2,
                26,
                124,
                (float)model->crack_tested / (float)model->crack_total);
            snprintf(
                buf,
                sizeof(buf),
                "Key: %lu/%lu [%08lX]",
                (unsigned long)model->crack_tested,
                (unsigned long)model->crack_total,
                (unsigned long)model->crack_current_key);
            canvas_draw_str(canvas, 2, 43, buf);
        } else {
            snprintf(
                buf,
                sizeof(buf),
                "Found Key: %08lX %s",
                (unsigned long)model->crack_found_key,
                model->crack_found ? "[CRACKED!]" : "[NONE]");
            canvas_draw_str(canvas, 2, 35, buf);
            canvas_draw_str(canvas, 2, 46, model->status_msg);
        }

        canvas_draw_line(canvas, 0, 52, 128, 52);
        canvas_draw_str(canvas, 2, 62, "OK:Crack  R:Switch Method");
        break;
    }
    case T55ModeUnbrick: {
        const char* const presets[] = {"Standard EM4100", "Clean All-Zeros", "Full Tag Wipe"};
        snprintf(buf, sizeof(buf), "Preset: %s", presets[model->unbrick_preset]);
        canvas_draw_str(canvas, 2, 23, buf);

        canvas_draw_str(canvas, 2, 34, "Fires Atmel 5184us pulses (r0..r3)");
        canvas_draw_str(canvas, 2, 45, model->status_msg);

        canvas_draw_line(canvas, 0, 52, 128, 52);
        canvas_draw_str(canvas, 2, 62, "OK:Unbrick Tag  R:Preset");
        break;
    }
    case T55ModeSnoop: {
        snprintf(
            buf,
            sizeof(buf),
            "Snoop: %s",
            model->snoop_active ? "LISTENING FIELD..." : "IDLE");
        canvas_draw_str(canvas, 2, 23, buf);

        snprintf(
            buf,
            sizeof(buf),
            "Harvested: %08lX %s",
            (unsigned long)model->snoop_pwd,
            model->snoop_found ? "[CAPTURED!]" : "[--]");
        canvas_draw_str(canvas, 2, 34, buf);

        canvas_draw_str(canvas, 2, 45, model->status_msg);

        canvas_draw_line(canvas, 0, 52, 128, 52);
        canvas_draw_str(canvas, 2, 62, "OK:Start/Stop  R:Save Key");
        break;
    }
    case T55ModeScope: {
        // Draw 64-sample waveform
        uint32_t max_v = model->scope_peak_mv > 0 ? model->scope_peak_mv : 50000;
        int prev_x = 0;
        int prev_y = 50;
        for(int i = 0; i < 64; i++) {
            int idx = (model->scope_head + i) % 64;
            uint16_t val = model->scope_samples[idx];
            int y = 50 - (int)((uint32_t)val * 34 / max_v);
            if(y < 14) y = 14;
            if(y > 50) y = 50;
            int x = i * 2;
            if(i > 0) {
                canvas_draw_line(canvas, prev_x, prev_y, x, y);
            }
            prev_x = x;
            prev_y = y;
        }

        float v_now = (float)model->scope_now_mv / 1000.0f;
        float v_peak = (float)model->scope_peak_mv / 1000.0f;
        uint32_t drop =
            (model->scope_peak_mv > model->scope_now_mv && model->scope_peak_mv > 0) ?
                ((model->scope_peak_mv - model->scope_now_mv) * 100 / model->scope_peak_mv) :
                0;
        snprintf(buf, sizeof(buf), "Now:%.2fV Pk:%.2fV -%lu%%", (double)v_now, (double)v_peak, (unsigned long)drop);
        canvas_draw_str(canvas, 2, 62, buf);
        break;
    }
    default:
        break;
    }
}

static void t5577_suite_start_worker(T5577SuitePage* page);
static void t5577_suite_stop_worker(T5577SuitePage* page);

static bool t5577_suite_input_callback(InputEvent* event, void* context) {
    T5577SuitePage* page = context;
    if(!page) return false;

    if(event->type == InputTypeShort && event->key == InputKeyBack) {
        t5577_suite_stop_worker(page);
        if(page->back_callback) {
            page->back_callback(page->back_callback_context);
        }
        return true;
    }

    if(event->type == InputTypeShort && event->key == InputKeyLeft) {
        t5577_suite_stop_worker(page);
        if(page->back_callback) {
            page->back_callback(page->back_callback_context);
        }
        return true;
    }

    if((event->type == InputTypeShort || event->type == InputTypeRepeat) &&
       (event->key == InputKeyUp || event->key == InputKeyDown)) {
        if(!page->worker_running) {
            T5577SuiteModel* model = view_get_model(page->view);
            if(event->key == InputKeyUp) {
                model->mode =
                    (model->mode == 0) ? (T55Mode)(T55ModeCount - 1) : (T55Mode)(model->mode - 1);
            } else {
                model->mode = (T55Mode)((model->mode + 1) % T55ModeCount);
            }
            view_commit_model(page->view, true);
            return true;
        }
    }

    if(event->type == InputTypeShort && event->key == InputKeyOk) {
        if(!page->worker_running) {
            t5577_suite_start_worker(page);
            return true;
        } else {
            T5577SuiteModel* model = view_get_model(page->view);
            if(model->mode == T55ModeScope) {
                model->scope_paused = !model->scope_paused;
                view_commit_model(page->view, true);
                return true;
            } else if(model->mode == T55ModeSnoop) {
                page->cancel_requested = true;
                view_commit_model(page->view, false);
                return true;
            }
            view_commit_model(page->view, false);
        }
    }

    if(event->type == InputTypeShort && event->key == InputKeyRight) {
        T5577SuiteModel* model = view_get_model(page->view);
        if(!page->worker_running) {
            switch(model->mode) {
            case T55ModeEmClone: {
                // Save .rfid file
                Storage* storage = furi_record_open(RECORD_STORAGE);
                if(storage) {
                    char path[64];
                    snprintf(
                        path,
                        sizeof(path),
                        "/ext/lfrfid/PM5_EM_%02X%02X%02X%02X%02X.rfid",
                        model->em_uid[0],
                        model->em_uid[1],
                        model->em_uid[2],
                        model->em_uid[3],
                        model->em_uid[4]);
                    File* file = storage_file_alloc(storage);
                    if(file && storage_file_open(file, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
                        char content[160];
                        snprintf(
                            content,
                            sizeof(content),
                            "Filetype: Flipper RFID key\nVersion: 1\nKey type: EM4100\nData: %02X %02X %02X %02X %02X\n",
                            model->em_uid[0],
                            model->em_uid[1],
                            model->em_uid[2],
                            model->em_uid[3],
                            model->em_uid[4]);
                        storage_file_write(file, content, (uint16_t)strlen(content));
                        storage_file_close(file);
                        snprintf(model->status_msg, sizeof(model->status_msg), "Saved to /ext/lfrfid!");
                    }
                    if(file) storage_file_free(file);
                    furi_record_close(RECORD_STORAGE);
                }
                break;
            }
            case T55ModeBlockRead: {
                // Cycle block 0..7
                model->selected_block = (model->selected_block + 1) % 8;
                break;
            }
            case T55ModeBlockWrite: {
                // Toggle auth mode
                model->use_pwd = !model->use_pwd;
                snprintf(
                    model->status_msg,
                    sizeof(model->status_msg),
                    model->use_pwd ? "Auth Mode ON (Pwd used)" : "Plain Mode (No pwd)");
                break;
            }
            case T55ModePwdCrack: {
                // Switch method
                model->crack_method = (model->crack_method + 1) % 2;
                break;
            }
            case T55ModeUnbrick: {
                // Cycle presets
                model->unbrick_preset = (model->unbrick_preset + 1) % 3;
                break;
            }
            case T55ModeSnoop: {
                // Save found key
                if(model->snoop_found) {
                    Storage* storage = furi_record_open(RECORD_STORAGE);
                    if(storage) {
                        File* file = storage_file_alloc(storage);
                        if(file && storage_file_open(file, T5577_DICT_PATH, FSAM_WRITE, FSOM_OPEN_APPEND)) {
                            char line[32];
                            int len = snprintf(line, sizeof(line), "%08lX\n", (unsigned long)model->snoop_pwd);
                            storage_file_write(file, line, (uint16_t)len);
                            storage_file_close(file);
                            snprintf(model->status_msg, sizeof(model->status_msg), "Saved to t5577_pass.txt!");
                        }
                        if(file) storage_file_free(file);
                        furi_record_close(RECORD_STORAGE);
                    }
                }
                break;
            }
            case T55ModeScope: {
                model->scope_peak_mv = model->scope_now_mv;
                break;
            }
            default:
                break;
            }
            view_commit_model(page->view, true);
            return true;
        }
        view_commit_model(page->view, false);
    }

    return false;
}

static int32_t t5577_suite_worker(void* context) {
    T5577SuitePage* page = context;
    if(!page) return 0;

    PacketResponseNG* resp = malloc(sizeof(PacketResponseNG));
    if(!resp) {
        page->worker_running = false;
        return 0;
    }

    T5577SuiteModel* model = view_get_model(page->view);
    T55Mode mode = model->mode;
    uint8_t em_uid[5];
    memcpy(em_uid, model->em_uid, 5);
    uint8_t sel_block = model->selected_block;
    uint32_t write_data = model->write_data;
    uint32_t active_pwd = model->active_pwd;
    bool use_pwd = model->use_pwd;
    uint8_t downlink_mode = model->downlink_mode;
    uint8_t crack_method = model->crack_method;
    uint8_t unbrick_preset = model->unbrick_preset;
    view_commit_model(page->view, false);

    switch(mode) {
    case T55ModeEmClone: {
        if(!model->has_em_uid) {
            clearCommandBuffer();
            SendCommandNG(CMD_LF_EM410X_WATCH, NULL, 0);
            uint32_t start = furi_get_tick();
            bool break_armed = false;
            char log[140];
            bool found = false;
            while((furi_get_tick() - start) < 3000 && !page->cancel_requested) {
                proxmark5_com_keep_fast_poll();
                if(!break_armed && (furi_get_tick() - start) >= 250) {
                    proxmark5_com_set_break_loop(true);
                    break_armed = true;
                }
                if(proxmark5_frame_pop_log(log, sizeof(log))) {
                    if(parse_em410x_from_log(log, em_uid)) {
                        found = true;
                        break;
                    }
                }
                furi_delay_ms(15);
            }
            model = view_get_model(page->view);
            if(found) {
                memcpy(model->em_uid, em_uid, 5);
                model->has_em_uid = true;
                model->state = T55StateSuccess;
                snprintf(model->status_msg, sizeof(model->status_msg), "EM4100 Tag Scanned!");
            } else {
                model->state = T55StateNotFound;
                snprintf(model->status_msg, sizeof(model->status_msg), "No EM4100 tag detected");
            }
            view_commit_model(page->view, true);
            break;
        }

        // Clone to T5577
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
            .high = (uint32_t)em_uid[0],
            .low = ((uint32_t)em_uid[1] << 24) | ((uint32_t)em_uid[2] << 16) |
                   ((uint32_t)em_uid[3] << 8) | (uint32_t)em_uid[4],
        };

        clearCommandBuffer();
        SendCommandNG(CMD_LF_EM410X_CLONE, (uint8_t*)&payload, sizeof(payload));
        int rc = t55_wait_interruptible(CMD_LF_EM410X_CLONE, resp, 2500, &page->cancel_requested);

        model = view_get_model(page->view);
        if(rc == PM3_SUCCESS && resp->status == PM3_SUCCESS) {
            model->state = T55StateSuccess;
            snprintf(model->status_msg, sizeof(model->status_msg), "Cloned EM410x to T5577 OK!");
            FURI_LOG_I("PM5_T55", "EM4100 cloned successfully");
        } else {
            model->state = T55StateError;
            snprintf(model->status_msg, sizeof(model->status_msg), "Clone failed rc=%d", rc);
        }
        view_commit_model(page->view, true);
        break;
    }
    case T55ModeBlockRead: {
        // Read block via CMD_LF_T55XX_READBL
        struct {
            uint32_t password;
            uint8_t blockno;
            uint8_t page;
            bool pwdmode;
            uint8_t downlink_mode;
        } PACKED read_req = {
            .password = active_pwd,
            .blockno = sel_block,
            .page = 0,
            .pwdmode = use_pwd,
            .downlink_mode = downlink_mode,
        };

        clearCommandBuffer();
        SendCommandNG(CMD_LF_T55XX_READBL, (uint8_t*)&read_req, sizeof(read_req));
        int rc = t55_wait_interruptible(CMD_LF_T55XX_READBL, resp, 2500, &page->cancel_requested);

        model = view_get_model(page->view);
        if(rc == PM3_SUCCESS && resp->status == PM3_SUCCESS) {
            if(resp->length >= 4) {
                uint32_t val = (uint32_t)resp->data.asBytes[0] |
                               ((uint32_t)resp->data.asBytes[1] << 8) |
                               ((uint32_t)resp->data.asBytes[2] << 16) |
                               ((uint32_t)resp->data.asBytes[3] << 24);
                model->block_data[sel_block] = val;
                model->block_valid[sel_block] = true;
                if(sel_block == 0) {
                    T5577ConfigInfo cfg;
                    t5577_parse_config_block0(val, &cfg);
                    snprintf(
                        model->config_desc,
                        sizeof(model->config_desc),
                        "%.16s %.12s M:%u",
                        cfg.modulation,
                        cfg.bitrate,
                        (unsigned int)cfg.max_block);
                }
                snprintf(model->status_msg, sizeof(model->status_msg), "Block %u read OK!", sel_block);
            } else {
                snprintf(model->status_msg, sizeof(model->status_msg), "Blk %u: Signal acquired in buf", sel_block);
            }
            model->state = T55StateSuccess;
        } else {
            model->state = T55StateError;
            snprintf(model->status_msg, sizeof(model->status_msg), "Read Blk %u failed", sel_block);
        }
        view_commit_model(page->view, true);
        break;
    }
    case T55ModeBlockWrite: {
        t55xx_write_block_t wbl = {
            .data = write_data,
            .pwd = active_pwd,
            .blockno = sel_block,
            .flags = (uint16_t)((use_pwd ? 0x01 : 0x00) | ((downlink_mode & 3) << 3)),
        };

        clearCommandBuffer();
        SendCommandNG(CMD_LF_T55XX_WRITEBL, (uint8_t*)&wbl, sizeof(wbl));
        int rc = t55_wait_interruptible(CMD_LF_T55XX_WRITEBL, resp, 2500, &page->cancel_requested);

        model = view_get_model(page->view);
        if(rc == PM3_SUCCESS && resp->status == PM3_SUCCESS) {
            model->state = T55StateSuccess;
            snprintf(model->status_msg, sizeof(model->status_msg), "Wrote Blk %u OK!", sel_block);
        } else {
            model->state = T55StateError;
            snprintf(model->status_msg, sizeof(model->status_msg), "Write Blk %u failed", sel_block);
        }
        view_commit_model(page->view, true);
        break;
    }
    case T55ModePwdCrack: {
        if(crack_method == 0) {
            // White Cloner algorithm derived from lower 32-bit ID
            uint32_t lower_id = ((uint32_t)em_uid[1] << 24) | ((uint32_t)em_uid[2] << 16) |
                                ((uint32_t)em_uid[3] << 8) | (uint32_t)em_uid[4];
            uint32_t cand = t5577_white_pwdgen(lower_id);

            // Test candidate
            struct {
                uint32_t password;
                uint8_t blockno;
                uint8_t page;
                bool pwdmode;
                uint8_t downlink_mode;
            } PACKED read_req = {
                .password = cand,
                .blockno = 0,
                .page = 0,
                .pwdmode = true,
                .downlink_mode = 0,
            };
            clearCommandBuffer();
            SendCommandNG(CMD_LF_T55XX_READBL, (uint8_t*)&read_req, sizeof(read_req));
            int rc = t55_wait_interruptible(CMD_LF_T55XX_READBL, resp, 1500, &page->cancel_requested);

            model = view_get_model(page->view);
            model->crack_found_key = cand;
            model->crack_found = (rc == PM3_SUCCESS && resp->status == PM3_SUCCESS);
            if(model->crack_found) {
                model->active_pwd = cand;
                model->use_pwd = true;
                model->state = T55StateSuccess;
                snprintf(model->status_msg, sizeof(model->status_msg), "White Cloner Key: %08lX!", (unsigned long)cand);
            } else {
                model->state = T55StateNotFound;
                snprintf(model->status_msg, sizeof(model->status_msg), "Candidate %08lX no match", (unsigned long)cand);
            }
            view_commit_model(page->view, true);
        } else {
            // Dictionary Attack
            Storage* storage = furi_record_open(RECORD_STORAGE);
            if(storage) {
                File* file = storage_file_alloc(storage);
                if(file && storage_file_open(file, T5577_DICT_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
                    uint32_t count = 0;
                    bool hit = false;
                    uint32_t hit_key = 0;

                    model = view_get_model(page->view);
                    model->crack_total = sizeof(k_default_passwords) / sizeof(k_default_passwords[0]);
                    model->crack_tested = 0;
                    view_commit_model(page->view, true);

                    for(size_t i = 0; i < sizeof(k_default_passwords) / sizeof(k_default_passwords[0]); i++) {
                        if(page->cancel_requested) break;

                        uint32_t test_pwd = k_default_passwords[i];
                        count++;

                        model = view_get_model(page->view);
                        model->crack_tested = count;
                        model->crack_current_key = test_pwd;
                        view_commit_model(page->view, true);

                        struct {
                            uint32_t password;
                            uint8_t blockno;
                            uint8_t page;
                            bool pwdmode;
                            uint8_t downlink_mode;
                        } PACKED read_req = {
                            .password = test_pwd,
                            .blockno = 0,
                            .page = 0,
                            .pwdmode = true,
                            .downlink_mode = 0,
                        };
                        clearCommandBuffer();
                        SendCommandNG(CMD_LF_T55XX_READBL, (uint8_t*)&read_req, sizeof(read_req));
                        int rc = t55_wait_interruptible(CMD_LF_T55XX_READBL, resp, 800, &page->cancel_requested);
                        if(rc == PM3_SUCCESS && resp->status == PM3_SUCCESS && resp->length >= 4) {
                            hit = true;
                            hit_key = test_pwd;
                            break;
                        }
                        furi_delay_ms(15);
                    }
                    storage_file_close(file);

                    model = view_get_model(page->view);
                    if(hit) {
                        model->crack_found = true;
                        model->crack_found_key = hit_key;
                        model->active_pwd = hit_key;
                        model->use_pwd = true;
                        model->state = T55StateSuccess;
                        snprintf(model->status_msg, sizeof(model->status_msg), "Cracked! Key: %08lX", (unsigned long)hit_key);
                    } else {
                        model->state = T55StateNotFound;
                        snprintf(model->status_msg, sizeof(model->status_msg), "No match in dictionary");
                    }
                    view_commit_model(page->view, true);
                }
                if(file) storage_file_free(file);
                furi_record_close(RECORD_STORAGE);
            }
        }
        break;
    }
    case T55ModeUnbrick: {
        uint32_t target_b0 = 0x00148040U; // EM4100 default
        if(unbrick_preset == 1) target_b0 = 0x00000000U; // All0
        if(unbrick_preset == 2) target_b0 = 0x00148040U; // Wipe resets to EM4100

        bool recovered = false;
        // Cycle 4 reset rates (r0=0, r1=1, r2=2, r3=3) with test-mode timing (flags |= 0x04)
        for(uint8_t rate = 0; rate < 4 && !page->cancel_requested; rate++) {
            t55xx_write_block_t wbl = {
                .data = target_b0,
                .pwd = 0,
                .blockno = 0,
                .flags = (uint16_t)(0x04 | (rate << 3)), // 0x04 = testMode
            };
            clearCommandBuffer();
            SendCommandNG(CMD_LF_T55XX_WRITEBL, (uint8_t*)&wbl, sizeof(wbl));
            int rc = t55_wait_interruptible(CMD_LF_T55XX_WRITEBL, resp, 1200, &page->cancel_requested);
            if(rc == PM3_SUCCESS && resp->status == PM3_SUCCESS) {
                recovered = true;
                break;
            }
            furi_delay_ms(50);
        }

        model = view_get_model(page->view);
        if(recovered) {
            model->state = T55StateSuccess;
            snprintf(model->status_msg, sizeof(model->status_msg), "Unbrick SUCCESS! Blk0 restored");
        } else {
            model->state = T55StateError;
            snprintf(model->status_msg, sizeof(model->status_msg), "Unbrick pulses failed");
        }
        view_commit_model(page->view, true);
        break;
    }
    case T55ModeSnoop: {
        model = view_get_model(page->view);
        model->snoop_active = true;
        model->state = T55StateRunning;
        snprintf(model->status_msg, sizeof(model->status_msg), "Listening for reader gaps...");
        view_commit_model(page->view, true);

        // Run CMD_LF_SNIFF_RAW_ADC
        struct {
            uint16_t samples;
            uint16_t trigger_threshold;
        } PACKED sniff_req = {
            .samples = 4000,
            .trigger_threshold = 20,
        };
        clearCommandBuffer();
        SendCommandNG(CMD_LF_SNIFF_RAW_ADC, (uint8_t*)&sniff_req, sizeof(sniff_req));

        uint32_t snoop_start = furi_get_tick();
        while((furi_get_tick() - snoop_start) < 4000 && !page->cancel_requested) {
            proxmark5_com_keep_fast_poll();
            char log[120];
            if(proxmark5_frame_pop_log(log, sizeof(log))) {
                const char* p = strstr(log, "password");
                if(p) {
                    // Extract hex password
                    unsigned long pwd = 0;
                    if(sscanf(p, "password %lx", &pwd) == 1) {
                        model = view_get_model(page->view);
                        model->snoop_pwd = (uint32_t)pwd;
                        model->snoop_found = true;
                        view_commit_model(page->view, true);
                        break;
                    }
                }
            }
            furi_delay_ms(20);
        }

        model = view_get_model(page->view);
        model->snoop_active = false;
        model->state = model->snoop_found ? T55StateSuccess : T55StateNotFound;
        snprintf(
            model->status_msg,
            sizeof(model->status_msg),
            model->snoop_found ? "Password Harvested!" : "Sniff timeout (No reader)");
        view_commit_model(page->view, true);
        break;
    }
    case T55ModeScope: {
        model = view_get_model(page->view);
        model->state = T55StateRunning;
        view_commit_model(page->view, false);

        while(!page->cancel_requested) {
            model = view_get_model(page->view);
            bool paused = model->scope_paused;
            view_commit_model(page->view, false);

            if(!paused) {
                clearCommandBuffer();
                SendCommandNG(CMD_MEASURE_ANTENNA_TUNING_LF, NULL, 0);
                int rc = t55_wait_interruptible(CMD_MEASURE_ANTENNA_TUNING_LF, resp, 250, &page->cancel_requested);
                if(rc == PM3_SUCCESS && resp->status == PM3_SUCCESS && resp->length >= 4) {
                    uint32_t mv = (uint32_t)resp->data.asBytes[0] |
                                  ((uint32_t)resp->data.asBytes[1] << 8) |
                                  ((uint32_t)resp->data.asBytes[2] << 16) |
                                  ((uint32_t)resp->data.asBytes[3] << 24);

                    model = view_get_model(page->view);
                    model->scope_now_mv = mv;
                    if(mv > model->scope_peak_mv) model->scope_peak_mv = mv;
                    model->scope_samples[model->scope_head] = (uint16_t)(mv > 65535 ? 65535 : mv);
                    model->scope_head = (model->scope_head + 1) % 64;
                    view_commit_model(page->view, true);
                }
            }
            furi_delay_ms(60);
        }
        break;
    }
    default:
        break;
    }

    free(resp);
    page->worker_running = false;
    return 0;
}

static void t5577_suite_start_worker(T5577SuitePage* page) {
    if(!page || page->worker_running) return;

    t5577_ensure_directories_and_dict();

    T5577SuiteModel* model = view_get_model(page->view);
    model->state = T55StateRunning;
    snprintf(model->status_msg, sizeof(model->status_msg), "Executing...");
    view_commit_model(page->view, true);

    page->cancel_requested = false;
    page->worker_running = true;
    page->worker_thread = furi_thread_alloc_ex(
        "T5577SuiteWorker", 3072, t5577_suite_worker, page);
    furi_thread_start(page->worker_thread);
}

static void t5577_suite_stop_worker(T5577SuitePage* page) {
    if(!page || !page->worker_thread) return;

    page->cancel_requested = true;
    furi_thread_join(page->worker_thread);
    furi_thread_free(page->worker_thread);
    page->worker_thread = NULL;
    page->worker_running = false;
}

T5577SuitePage* t5577_suite_page_create(
    T5577SuitePageBackCallback back_callback,
    void* back_callback_context) {
    T5577SuitePage* page = calloc(1, sizeof(T5577SuitePage));
    if(!page) return NULL;

    page->view = view_alloc();
    if(!page->view) {
        free(page);
        return NULL;
    }

    page->back_callback = back_callback;
    page->back_callback_context = back_callback_context;

    view_set_context(page->view, page);
    view_allocate_model(page->view, ViewModelTypeLocking, sizeof(T5577SuiteModel));
    view_set_draw_callback(page->view, t5577_suite_draw_callback);
    view_set_input_callback(page->view, t5577_suite_input_callback);

    T5577SuiteModel* model = view_get_model(page->view);
    memset(model, 0, sizeof(*model));
    model->state = T55StateIdle;
    model->mode = T55ModeEmClone;

    // Default EM4100 test UID
    const uint8_t def_em[5] = {0x0F, 0x03, 0x68, 0x56, 0x8B};
    memcpy(model->em_uid, def_em, 5);
    model->has_em_uid = true;
    model->write_data = 0x00148040U;
    model->active_pwd = 0x00000000U;
    snprintf(model->status_msg, sizeof(model->status_msg), "Ready");
    view_commit_model(page->view, false);

    return page;
}

void t5577_suite_page_destroy(T5577SuitePage* page) {
    if(!page) return;
    t5577_suite_stop_worker(page);
    if(page->view) {
        view_free(page->view);
    }
    free(page);
}

View* t5577_suite_page_get_view(T5577SuitePage* page) {
    return page ? page->view : NULL;
}
