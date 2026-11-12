#include <furi.h>
#include <gui/canvas.h>
#include <gui/elements.h>
#include <string.h>
#include "read_hf_multi_page.h"
#include "proxmark5_frame.h"
#include "pm3_cmd.h"

typedef enum {
    HfMultiModeIso15693 = 0,
    HfMultiModeIclass = 1,
    HfMultiModeCount = 2,
} HfMultiMode;

typedef enum {
    HfMultiStateIdle,
    HfMultiStateRunning,
    HfMultiStateSuccess,
    HfMultiStateNotFound,
    HfMultiStateTimeout,
    HfMultiStateError,
} HfMultiState;

typedef struct {
    HfMultiMode mode;
    HfMultiState state;
    uint8_t uid[8];
    uint8_t dsfid;
    uint8_t epurse[8];
    bool has_epurse;
} HfMultiModel;

struct ReadHfMultiPage {
    View* view;
    FuriThread* worker_thread;
    volatile bool worker_thread_running;
    volatile bool worker_thread_cancel_requested;
    ReadHfMultiPageBackCallback back_callback;
    void* back_callback_context;
};

static uint16_t iso15693_crc16(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFF;
    for(size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for(int bit = 0; bit < 8; bit++) {
            if(crc & 1) {
                crc = (crc >> 1) ^ 0x8408;
            } else {
                crc >>= 1;
            }
        }
    }
    return (uint16_t)(~crc & 0xFFFF);
}

static const char* iso15693_get_mfg_name(uint8_t mfg_code) {
    switch(mfg_code) {
    case 0x01:
        return "Motorola";
    case 0x02:
        return "STMicroelectronics";
    case 0x03:
        return "Hitachi";
    case 0x04:
        return "NXP (ICODE/SLIX)";
    case 0x05:
        return "Infineon";
    case 0x07:
        return "Texas Instruments";
    case 0x08:
        return "Fujitsu";
    case 0x16:
        return "EM Microelectronic";
    case 0x2B:
        return "Maxim";
    default:
        return "ISO15693 / NFC-V";
    }
}

static int hf_multi_wait_interruptible(
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

static int hf_multi_scan_iso15693(
    volatile bool* cancel_requested,
    uint8_t out_uid[8],
    uint8_t* out_dsfid,
    bool* out_found) {
    *out_found = false;

    struct {
        uint8_t flags;
        uint16_t rawlen;
        uint8_t raw[5];
    } PACKED req;

    // ISO15_CONNECT (1<<0) | ISO15_HIGH_SPEED (1<<4) | ISO15_READ_RESPONSE (1<<5)
    req.flags = (1 << 0) | (1 << 4) | (1 << 5);
    req.rawlen = 5;
    req.raw[0] = 0x26; // Single subcarrier, high data rate, inventory, 1 slot
    req.raw[1] = 0x01; // Inventory command
    req.raw[2] = 0x00; // Mask length 0
    uint16_t crc = iso15693_crc16(req.raw, 3);
    req.raw[3] = (uint8_t)(crc & 0xFF);
    req.raw[4] = (uint8_t)((crc >> 8) & 0xFF);

    PacketResponseNG* resp = malloc(sizeof(PacketResponseNG));
    if(!resp) {
        return PM3_EMALLOC;
    }

    clearCommandBuffer();
    SendCommandNG(CMD_HF_ISO15693_COMMAND, (uint8_t*)&req, sizeof(req));

    int wait_res =
        hf_multi_wait_interruptible(CMD_HF_ISO15693_COMMAND, resp, 2500, cancel_requested);
    if(wait_res != PM3_SUCCESS) {
        free(resp);
        return wait_res;
    }

    if(resp->status == PM3_SUCCESS && resp->length >= 12) {
        uint16_t rx_crc = iso15693_crc16(resp->data.asBytes, 10);
        uint16_t pkt_crc =
            (uint16_t)resp->data.asBytes[10] | ((uint16_t)resp->data.asBytes[11] << 8);
        if(rx_crc == pkt_crc) {
            *out_dsfid = resp->data.asBytes[1];
            for(int i = 0; i < 8; i++) {
                out_uid[i] = resp->data.asBytes[9 - i];
            }
            *out_found = true;
        }
    }

    free(resp);
    return PM3_SUCCESS;
}

static int hf_multi_scan_iclass(
    volatile bool* cancel_requested,
    uint8_t out_csn[8],
    uint8_t out_epurse[8],
    bool* out_has_epurse,
    bool* out_found) {
    *out_found = false;
    *out_has_epurse = false;

    struct {
        uint8_t flags;
        uint8_t page;
    } PACKED req = {
        .flags = 0x01 | 0x02, // FLAG_ICLASS_READER_INIT | FLAG_ICLASS_READER_CLEARTRACE
        .page = 0,
    };

    PacketResponseNG* resp = malloc(sizeof(PacketResponseNG));
    if(!resp) {
        return PM3_EMALLOC;
    }

    clearCommandBuffer();
    SendCommandNG(CMD_HF_ICLASS_READER, (uint8_t*)&req, sizeof(req));

    int wait_res =
        hf_multi_wait_interruptible(CMD_HF_ICLASS_READER, resp, 2500, cancel_requested);
    if(wait_res != PM3_SUCCESS) {
        free(resp);
        return wait_res;
    }

    if(resp->status == PM3_SUCCESS && resp->length >= 1 + 8 + 8 + 8) {
        uint8_t status_flags = resp->data.asBytes[0];
        if(status_flags & 0x01) { // FLAG_ICLASS_CSN
            memcpy(out_csn, &resp->data.asBytes[1], 8);
            *out_found = true;
            if(status_flags & 0x02) { // FLAG_ICLASS_CC
                memcpy(out_epurse, &resp->data.asBytes[1 + 8 + 8], 8);
                *out_has_epurse = true;
            }
        }
    }

    free(resp);
    return PM3_SUCCESS;
}

static void read_hf_multi_draw_callback(Canvas* canvas, void* context) {
    HfMultiModel* model = context;
    if(!model) {
        return;
    }

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    if(model->mode == HfMultiModeIso15693) {
        canvas_draw_str(canvas, 4, 12, "HF ISO15693 (NFC-V)");
    } else {
        canvas_draw_str(canvas, 4, 12, "HF iCLASS / PicoPass");
    }

    canvas_set_font(canvas, FontSecondary);
    char line[44];
    switch(model->state) {
    case HfMultiStateIdle:
        canvas_draw_str(canvas, 4, 30, "Press Scan to read tag");
        canvas_draw_str(canvas, 4, 42, "Press Right to switch mode");
        break;
    case HfMultiStateRunning:
        canvas_draw_str(canvas, 4, 32, "Scanning 13.56 MHz field...");
        break;
    case HfMultiStateSuccess:
        if(model->mode == HfMultiModeIso15693) {
            snprintf(line, sizeof(line), "Mfg: %s", iso15693_get_mfg_name(model->uid[1]));
            canvas_draw_str(canvas, 4, 24, line);
            snprintf(
                line,
                sizeof(line),
                "UID: %02X%02X%02X%02X%02X%02X%02X%02X",
                model->uid[0],
                model->uid[1],
                model->uid[2],
                model->uid[3],
                model->uid[4],
                model->uid[5],
                model->uid[6],
                model->uid[7]);
            canvas_draw_str(canvas, 4, 35, line);
            snprintf(
                line,
                sizeof(line),
                "DSFID: 0x%02X  IC Mfg: 0x%02X",
                model->dsfid,
                model->uid[1]);
            canvas_draw_str(canvas, 4, 46, line);
        } else {
            canvas_draw_str(canvas, 4, 24, "Card: HID iCLASS / PicoPass");
            snprintf(
                line,
                sizeof(line),
                "CSN: %02X%02X%02X%02X%02X%02X%02X%02X",
                model->uid[0],
                model->uid[1],
                model->uid[2],
                model->uid[3],
                model->uid[4],
                model->uid[5],
                model->uid[6],
                model->uid[7]);
            canvas_draw_str(canvas, 4, 35, line);
            if(model->has_epurse) {
                snprintf(
                    line,
                    sizeof(line),
                    "CC:  %02X%02X%02X%02X%02X%02X%02X%02X",
                    model->epurse[0],
                    model->epurse[1],
                    model->epurse[2],
                    model->epurse[3],
                    model->epurse[4],
                    model->epurse[5],
                    model->epurse[6],
                    model->epurse[7]);
                canvas_draw_str(canvas, 4, 46, line);
            }
        }
        break;
    case HfMultiStateNotFound:
        if(model->mode == HfMultiModeIso15693) {
            canvas_draw_str(canvas, 4, 32, "No ISO15693 tag detected");
        } else {
            canvas_draw_str(canvas, 4, 32, "No iCLASS card detected");
        }
        canvas_draw_str(canvas, 4, 44, "Place on HF coil & Scan");
        break;
    case HfMultiStateTimeout:
        canvas_draw_str(canvas, 4, 34, "Device not responding");
        break;
    case HfMultiStateError:
        canvas_draw_str(canvas, 4, 34, "Scan failed");
        break;
    }

    elements_button_left(canvas, "Back");
    if(model->state != HfMultiStateRunning) {
        elements_button_center(canvas, "Scan");
        elements_button_right(canvas, model->mode == HfMultiModeIso15693 ? "iCLASS" : "15693");
    }
}

static bool read_hf_multi_input_callback(InputEvent* event, void* context) {
    ReadHfMultiPage* page = context;
    if(!page || !event) {
        return false;
    }

    if(event->type == InputTypeShort && event->key == InputKeyLeft) {
        read_hf_multi_page_stop(page);
        if(page->back_callback) {
            page->back_callback(page->back_callback_context);
        }
        return true;
    }

    if(event->type == InputTypeShort && event->key == InputKeyRight) {
        if(!page->worker_thread_running) {
            HfMultiModel* model = view_get_model(page->view);
            model->mode = (model->mode + 1) % HfMultiModeCount;
            model->state = HfMultiStateIdle;
            view_commit_model(page->view, true);
            read_hf_multi_page_start(page);
            return true;
        }
    }

    if(event->type == InputTypeShort && event->key == InputKeyOk) {
        if(!page->worker_thread_running) {
            read_hf_multi_page_start(page);
            return true;
        }
    }

    return false;
}

static void read_hf_multi_cleanup_worker(ReadHfMultiPage* page) {
    if(!page || !page->worker_thread || page->worker_thread_running) {
        return;
    }
    furi_thread_join(page->worker_thread);
    furi_thread_free(page->worker_thread);
    page->worker_thread = NULL;
}

static int32_t read_hf_multi_worker(void* context) {
    ReadHfMultiPage* page = context;
    if(!page) {
        return 0;
    }

    HfMultiModel* model = view_get_model(page->view);
    HfMultiMode mode = model->mode;
    view_commit_model(page->view, false);

    uint8_t uid[8] = {0};
    uint8_t dsfid = 0;
    uint8_t epurse[8] = {0};
    bool has_epurse = false;
    bool found = false;
    int res;

    if(mode == HfMultiModeIso15693) {
        res = hf_multi_scan_iso15693(
            &page->worker_thread_cancel_requested, uid, &dsfid, &found);
    } else {
        res = hf_multi_scan_iclass(
            &page->worker_thread_cancel_requested, uid, epurse, &has_epurse, &found);
    }

    model = view_get_model(page->view);
    if(res == PM3_SUCCESS) {
        if(found) {
            memcpy(model->uid, uid, 8);
            model->dsfid = dsfid;
            memcpy(model->epurse, epurse, 8);
            model->has_epurse = has_epurse;
            model->state = HfMultiStateSuccess;
            FURI_LOG_I("PM5_QA_HFMulti", "Mode=%d Tag found", mode);
        } else {
            model->state = HfMultiStateNotFound;
            FURI_LOG_I("PM5_QA_HFMulti", "Mode=%d OK (No tag in field)", mode);
        }
    } else if(res == PM3_EOPABORTED) {
        model->state = HfMultiStateIdle;
    } else if(res == PM3_ETIMEOUT) {
        model->state = HfMultiStateTimeout;
        FURI_LOG_W("PM5_QA_HFMulti", "Mode=%d TIMEOUT", mode);
    } else {
        model->state = HfMultiStateError;
        FURI_LOG_W("PM5_QA_HFMulti", "Mode=%d ERROR res=%d", mode, res);
    }
    view_commit_model(page->view, true);

    page->worker_thread_running = false;
    return 0;
}

ReadHfMultiPage* read_hf_multi_page_create(
    ReadHfMultiPageBackCallback back_callback,
    void* back_callback_context) {
    ReadHfMultiPage* page = calloc(1, sizeof(ReadHfMultiPage));
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
    view_allocate_model(page->view, ViewModelTypeLocking, sizeof(HfMultiModel));
    view_set_draw_callback(page->view, read_hf_multi_draw_callback);
    view_set_input_callback(page->view, read_hf_multi_input_callback);

    HfMultiModel* model = view_get_model(page->view);
    model->mode = HfMultiModeIso15693;
    model->state = HfMultiStateIdle;
    view_commit_model(page->view, false);

    return page;
}

void read_hf_multi_page_free(ReadHfMultiPage* page) {
    if(!page) {
        return;
    }
    page->worker_thread_cancel_requested = true;
    if(page->worker_thread) {
        furi_thread_join(page->worker_thread);
        furi_thread_free(page->worker_thread);
        page->worker_thread = NULL;
        page->worker_thread_running = false;
    }
    if(page->view) {
        view_set_context(page->view, NULL);
        view_free_model(page->view);
        view_free(page->view);
        page->view = NULL;
    }
    free(page);
}

View* read_hf_multi_page_get_view(ReadHfMultiPage* page) {
    return page ? page->view : NULL;
}

void read_hf_multi_page_start(ReadHfMultiPage* page) {
    if(!page || !page->view) {
        return;
    }
    read_hf_multi_cleanup_worker(page);
    if(page->worker_thread) {
        return;
    }

    HfMultiModel* model = view_get_model(page->view);
    model->state = HfMultiStateRunning;
    view_commit_model(page->view, true);

    page->worker_thread_cancel_requested = false;
    page->worker_thread =
        furi_thread_alloc_ex("ReadHfMultiTask", 4096, read_hf_multi_worker, page);
    if(!page->worker_thread) {
        model = view_get_model(page->view);
        model->state = HfMultiStateError;
        view_commit_model(page->view, true);
        return;
    }

    page->worker_thread_running = true;
    furi_thread_start(page->worker_thread);
}

void read_hf_multi_page_stop(ReadHfMultiPage* page) {
    if(!page || !page->worker_thread) {
        return;
    }
    page->worker_thread_cancel_requested = true;
}
