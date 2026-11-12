#include <furi.h>
#include <gui/canvas.h>
#include <gui/elements.h>
#include <storage/storage.h>
#include <stdio.h>
#include <string.h>
#include "hf_sniff_page.h"
#include "proxmark5_com.h"
#include "proxmark5_frame.h"
#include "pm3_cmd.h"

#define MAX_SNIFF_FRAMES 24U
#define MAX_TRACE_BYTES  1024U

typedef enum {
    HfSniffStateIdle,
    HfSniffStateSniffing,
    HfSniffStateDownloading,
    HfSniffStateDone,
    HfSniffStateError,
} HfSniffState;

typedef struct {
    HfSniffState state;
    uint32_t elapsed_ds;
    uint32_t trace_bytes;
    uint8_t frame_count;
    uint8_t scroll_idx;
    char frames[MAX_SNIFF_FRAMES][38];
    char status_msg[40];
} HfSniffModel;

struct HfSniffPage {
    View* view;
    FuriThread* worker_thread;
    volatile bool worker_running;
    volatile bool stop_sniff_requested;
    uint8_t* raw_trace;
    uint32_t raw_trace_len;

    HfSniffPageBackCallback back_callback;
    void* back_callback_context;
};

static const char* hf_sniff_annotate_cmd(const uint8_t* d, uint16_t len, bool is_resp) {
    if(len == 0) return "";
    if(is_resp) {
        if(len == 2) return "ATQA";
        if(len == 1 && d[0] == 0x0A) return "ACK";
        if(len == 5) return "UID+BCC";
        return "";
    }
    switch(d[0]) {
    case 0x26:
        return "REQA";
    case 0x52:
        return "WUPA";
    case 0x93:
        return (len >= 2 && d[1] == 0x20) ? "ANTICOLL1" : "SELECT1";
    case 0x95:
        return (len >= 2 && d[1] == 0x20) ? "ANTICOLL2" : "SELECT2";
    case 0x30:
        return "READ";
    case 0xA0:
        return "WRITE";
    case 0xA2:
        return "UL_WRITE";
    case 0x60:
        return "AUTH_A";
    case 0x61:
        return "AUTH_B";
    case 0x50:
        return "HALT";
    case 0xE0:
        return "RATS";
    case 0x40:
        return "WUPC1";
    case 0x43:
        return "WUPC2";
    default:
        return "";
    }
}

static void hf_sniff_decode_trace(
    const uint8_t* buf,
    uint32_t trace_len,
    HfSniffModel* model) {
    model->frame_count = 0;
    model->scroll_idx = 0;

    uint32_t offset = 0;
    uint32_t limit = trace_len > MAX_TRACE_BYTES ? MAX_TRACE_BYTES : trace_len;

    while(offset + sizeof(tracelog_hdr_t) <= limit && model->frame_count < MAX_SNIFF_FRAMES) {
        const tracelog_hdr_t* hdr = (const tracelog_hdr_t*)(buf + offset);
        uint16_t dlen = hdr->data_len;
        if(dlen == 0 || dlen > 256) {
            break;
        }
        uint16_t plen = (uint16_t)((dlen - 1U) / 8U + 1U);
        uint32_t next_offset = offset + sizeof(tracelog_hdr_t) + dlen + plen;
        if(next_offset > limit) {
            break;
        }

        const uint8_t* frame_bytes = hdr->frame;
        char* line = model->frames[model->frame_count];
        int pos = snprintf(
            line,
            sizeof(model->frames[0]),
            "%02u %s",
            model->frame_count,
            hdr->isResponse ? "T<" : "R>");

        uint16_t show_bytes = dlen > 5 ? 5 : dlen;
        for(uint16_t i = 0; i < show_bytes && pos + 3 < (int)sizeof(model->frames[0]); i++) {
            pos += snprintf(
                line + pos,
                sizeof(model->frames[0]) - pos,
                " %02X",
                frame_bytes[i]);
        }
        if(dlen > 5 && pos + 3 < (int)sizeof(model->frames[0])) {
            pos += snprintf(line + pos, sizeof(model->frames[0]) - pos, "..");
        }

        const char* note = hf_sniff_annotate_cmd(frame_bytes, dlen, hdr->isResponse);
        if(note[0] && pos + 4 < (int)sizeof(model->frames[0])) {
            snprintf(line + pos, sizeof(model->frames[0]) - pos, " %s", note);
        }

        model->frame_count++;
        offset = next_offset;
    }
}

static bool hf_sniff_save_trace_file(HfSniffPage* page, char* out_msg, size_t out_msg_size) {
    if(!page || !page->raw_trace) {
        snprintf(out_msg, out_msg_size, "No trace buffer");
        return false;
    }

    uint32_t tick_id = (furi_get_tick() / 100U) & 0xFFFFU;
    char path[64];
    snprintf(path, sizeof(path), "/ext/apps_data/pm5/hf14a_%04lX.trace", (unsigned long)tick_id);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, "/ext/apps_data");
    storage_simply_mkdir(storage, "/ext/apps_data/pm5");
    File* file = storage_file_alloc(storage);
    bool ok = false;
    uint32_t save_len =
        page->raw_trace_len > MAX_TRACE_BYTES ? MAX_TRACE_BYTES : page->raw_trace_len;
    if(storage_file_open(file, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        if(save_len > 0) {
            uint16_t written = storage_file_write(file, page->raw_trace, (uint16_t)save_len);
            ok = (written == (uint16_t)save_len);
        } else {
            ok = true;
        }
        storage_file_close(file);
    }
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);

    if(ok) {
        snprintf(out_msg, out_msg_size, "Saved hf14a_%04lX.trace", (unsigned long)tick_id);
        FURI_LOG_I("PM5_QA_SNIFF", "Saved %s (%lu bytes)", path, (unsigned long)save_len);
    } else {
        snprintf(out_msg, out_msg_size, "SD write failed");
    }
    return ok;
}

static void hf_sniff_page_draw_callback(Canvas* canvas, void* context) {
    HfSniffModel* model = context;
    if(!model) {
        return;
    }

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 11, "14a Air Sniffer");
    canvas_set_font(canvas, FontSecondary);

    switch(model->state) {
    case HfSniffStateIdle:
        canvas_draw_str(canvas, 4, 25, "FPGA passive ISO14443A tap");
        canvas_draw_str(canvas, 4, 35, "Place PM5 between Reader &");
        canvas_draw_str(canvas, 4, 45, "Tag, then press Sniff.");
        break;
    case HfSniffStateSniffing: {
        char line[44];
        snprintf(
            line,
            sizeof(line),
            "Sniffing 13.56MHz... %lu.%lus",
            (unsigned long)(model->elapsed_ds / 10U),
            (unsigned long)(model->elapsed_ds % 10U));
        canvas_draw_str(canvas, 4, 26, line);
        canvas_draw_str(canvas, 4, 37, "Capturing R> & T< frames");
        canvas_draw_str(canvas, 4, 47, "Press Stop to decode BigBuf");
        break;
    }
    case HfSniffStateDownloading:
        canvas_draw_str(canvas, 4, 32, "Stopping & reading BigBuf...");
        break;
    case HfSniffStateDone: {
        char hdr[44];
        if(model->status_msg[0]) {
            snprintf(hdr, sizeof(hdr), "%s", model->status_msg);
        } else {
            snprintf(
                hdr,
                sizeof(hdr),
                "Trace: %luB (%u frames)",
                (unsigned long)model->trace_bytes,
                model->frame_count);
        }
        canvas_draw_str(canvas, 2, 20, hdr);

        if(model->frame_count == 0) {
            canvas_draw_str(canvas, 4, 33, "0 frames captured (quiet air)");
            canvas_draw_str(canvas, 4, 43, "Tap card to reader while");
            canvas_draw_str(canvas, 4, 51, "sniffing to capture frames.");
        } else {
            for(uint8_t row = 0; row < 3; row++) {
                uint8_t idx = (uint8_t)(model->scroll_idx + row);
                if(idx < model->frame_count) {
                    canvas_draw_str(canvas, 2, (uint8_t)(30 + row * 10), model->frames[idx]);
                }
            }
        }
        break;
    }
    case HfSniffStateError:
        canvas_draw_str(canvas, 4, 34, "Sniffer communication error");
        break;
    }

    elements_button_left(canvas, "Back");
    if(model->state == HfSniffStateSniffing) {
        elements_button_center(canvas, "Stop");
    } else if(model->state != HfSniffStateDownloading) {
        elements_button_center(canvas, "Sniff");
        if(model->state == HfSniffStateDone && model->trace_bytes > 0) {
            elements_button_right(canvas, "Save");
        }
    }
}

static bool hf_sniff_page_input_callback(InputEvent* event, void* context) {
    HfSniffPage* page = context;
    if(!page || !event) {
        return false;
    }

    if(event->type == InputTypeShort && event->key == InputKeyLeft) {
        hf_sniff_page_stop(page);
        if(page->back_callback) {
            page->back_callback(page->back_callback_context);
        }
        return true;
    }

    if(event->type == InputTypeShort && event->key == InputKeyOk) {
        if(page->worker_running) {
            page->stop_sniff_requested = true;
            HfSniffModel* model = view_get_model(page->view);
            model->state = HfSniffStateDownloading;
            view_commit_model(page->view, true);
        } else {
            hf_sniff_page_start(page);
        }
        return true;
    }

    if(event->type == InputTypeShort && event->key == InputKeyRight) {
        if(!page->worker_running) {
            HfSniffModel* model = view_get_model(page->view);
            if(model && model->state == HfSniffStateDone && model->trace_bytes > 0) {
                hf_sniff_save_trace_file(page, model->status_msg, sizeof(model->status_msg));
            }
            view_commit_model(page->view, true);
            return true;
        }
    }

    if((event->type == InputTypeShort || event->type == InputTypeRepeat) &&
       (event->key == InputKeyUp || event->key == InputKeyDown)) {
        HfSniffModel* model = view_get_model(page->view);
        if(model && model->state == HfSniffStateDone && model->frame_count > 3) {
            if(event->key == InputKeyUp && model->scroll_idx > 0) {
                model->scroll_idx--;
            } else if(
                event->key == InputKeyDown &&
                (uint8_t)(model->scroll_idx + 3U) < model->frame_count) {
                model->scroll_idx++;
            }
        }
        view_commit_model(page->view, true);
        return true;
    }

    return false;
}

static void hf_sniff_cleanup_worker(HfSniffPage* page) {
    if(!page || !page->worker_thread || page->worker_running) {
        return;
    }
    furi_thread_join(page->worker_thread);
    furi_thread_free(page->worker_thread);
    page->worker_thread = NULL;
}

static int32_t hf_sniff_worker(void* context) {
    HfSniffPage* page = context;
    if(!page) {
        return 0;
    }

    PacketResponseNG* resp = malloc(sizeof(PacketResponseNG));
    if(!resp) {
        page->worker_running = false;
        return 0;
    }

    uint8_t param = 0x00; // Trigger immediately on any reader/tag activity
    clearCommandBuffer();
    SendCommandNG(CMD_HF_ISO14443A_SNIFF, &param, sizeof(param));

    uint32_t start_tick = furi_get_tick();
    bool sniff_exited = false;

    while(!page->stop_sniff_requested) {
        proxmark5_com_keep_fast_poll();
        if(proxmark5_frame_take_by_cmd(CMD_HF_ISO14443A_SNIFF, resp)) {
            sniff_exited = true;
            break;
        }
        uint32_t ds = (furi_get_tick() - start_tick) / 100U;
        HfSniffModel* model = view_get_model(page->view);
        model->elapsed_ds = ds;
        view_commit_model(page->view, true);
        furi_delay_ms(50);
    }

    if(!sniff_exited) {
        proxmark5_com_set_break_loop(true);
        WaitForResponseTimeout(CMD_HF_ISO14443A_SNIFF, resp, 1500);
        proxmark5_com_set_break_loop(false);
    }

    // Parse trace length reported by PM5 Dbprintf("trace len = %d")
    uint32_t trace_len = 0;
    char log_line[160] = {0};
    while(proxmark5_frame_pop_log(log_line, sizeof(log_line))) {
        const char* p = strstr(log_line, "trace len =");
        if(p) {
            p += 11;
            while(*p == ' ') p++;
            uint32_t val = 0;
            while(*p >= '0' && *p <= '9') {
                val = val * 10U + (uint32_t)(*p - '0');
                p++;
            }
            trace_len = val;
        }
    }

    // Download captured trace from BigBuf in 256-byte chunks if trace_len > 0
    if(page->raw_trace) {
        memset(page->raw_trace, 0, MAX_TRACE_BYTES);
    }

    uint32_t to_download = trace_len > MAX_TRACE_BYTES ? MAX_TRACE_BYTES : trace_len;
    for(uint32_t offset = 0; offset < to_download; offset += 256U) {
        download_req_t req = {
            .start_index = offset,
            .bytes = 256U,
        };
        clearCommandBuffer();
        SendCommandNG(CMD_DOWNLOAD_BIGBUF, (uint8_t*)&req, sizeof(req));

        bool got_done = WaitForResponseTimeout(CMD_DOWNLOAD_BIGBUF, resp, 1000);
        if(got_done && resp->status == PM3_SUCCESS && resp->length >= sizeof(download_done_t)) {
            const download_done_t* done = (const download_done_t*)resp->data.asBytes;
            trace_len = done->extra;
        }

        PacketResponseNG* chunk_resp = malloc(sizeof(PacketResponseNG));
        if(chunk_resp) {
            if(proxmark5_frame_take_by_cmd(CMD_DOWNLOADED_BIGBUF, chunk_resp) &&
               chunk_resp->length > sizeof(download_chunk_t) && page->raw_trace) {
                uint16_t copy_len = (uint16_t)(chunk_resp->length - sizeof(download_chunk_t));
                if(offset + copy_len <= MAX_TRACE_BYTES) {
                    memcpy(
                        page->raw_trace + offset,
                        chunk_resp->data.asBytes + sizeof(download_chunk_t),
                        copy_len);
                }
            }
            free(chunk_resp);
        }

        if(!got_done || to_download <= offset + 256U) {
            break;
        }
    }

    page->raw_trace_len = trace_len;

    HfSniffModel* model = view_get_model(page->view);
    model->state = HfSniffStateDone;
    model->trace_bytes = trace_len;
    model->status_msg[0] = '\0';
    if(page->raw_trace && trace_len > 0) {
        hf_sniff_decode_trace(page->raw_trace, trace_len, model);
    } else {
        model->frame_count = 0;
        model->scroll_idx = 0;
    }
    FURI_LOG_I(
        "PM5_QA_SNIFF",
        "Sniff stopped: trace_len=%lu frames=%u",
        (unsigned long)trace_len,
        model->frame_count);
    view_commit_model(page->view, true);

    free(resp);
    page->worker_running = false;
    return 0;
}

HfSniffPage*
    hf_sniff_page_create(HfSniffPageBackCallback back_callback, void* back_callback_context) {
    HfSniffPage* page = calloc(1, sizeof(HfSniffPage));
    if(!page) {
        return NULL;
    }

    page->view = view_alloc();
    if(!page->view) {
        free(page);
        return NULL;
    }

    page->raw_trace = malloc(MAX_TRACE_BYTES);
    page->back_callback = back_callback;
    page->back_callback_context = back_callback_context;

    view_set_context(page->view, page);
    view_allocate_model(page->view, ViewModelTypeLocking, sizeof(HfSniffModel));
    view_set_draw_callback(page->view, hf_sniff_page_draw_callback);
    view_set_input_callback(page->view, hf_sniff_page_input_callback);

    HfSniffModel* model = view_get_model(page->view);
    memset(model, 0, sizeof(*model));
    model->state = HfSniffStateIdle;
    view_commit_model(page->view, false);

    return page;
}

void hf_sniff_page_free(HfSniffPage* page) {
    if(!page) {
        return;
    }

    hf_sniff_page_stop(page);

    if(page->raw_trace) {
        free(page->raw_trace);
        page->raw_trace = NULL;
    }

    if(page->view) {
        view_set_context(page->view, NULL);
        view_free_model(page->view);
        view_free(page->view);
        page->view = NULL;
    }

    free(page);
}

View* hf_sniff_page_get_view(HfSniffPage* page) {
    return page ? page->view : NULL;
}

void hf_sniff_page_start(HfSniffPage* page) {
    if(!page) {
        return;
    }

    hf_sniff_cleanup_worker(page);
    if(page->worker_running) {
        return;
    }

    HfSniffModel* model = view_get_model(page->view);
    model->state = HfSniffStateSniffing;
    model->elapsed_ds = 0;
    model->status_msg[0] = '\0';
    view_commit_model(page->view, true);

    page->stop_sniff_requested = false;
    page->worker_running = true;
    page->worker_thread = furi_thread_alloc_ex("HfSniffWorker", 3072, hf_sniff_worker, page);
    furi_thread_start(page->worker_thread);
}

void hf_sniff_page_stop(HfSniffPage* page) {
    if(!page) {
        return;
    }

    if(page->worker_thread && page->worker_running) {
        page->stop_sniff_requested = true;
        furi_thread_join(page->worker_thread);
        page->worker_running = false;
    }

    hf_sniff_cleanup_worker(page);

    HfSniffModel* model = view_get_model(page->view);
    if(model->state == HfSniffStateSniffing || model->state == HfSniffStateDownloading) {
        model->state = HfSniffStateIdle;
    }
    view_commit_model(page->view, false);
}
