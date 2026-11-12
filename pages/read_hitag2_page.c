#include <furi.h>
#include <gui/canvas.h>
#include <gui/elements.h>
#include "read_hitag2_page.h"
#include "proxmark5_frame.h"
#include "pm3_cmd.h"
#include "hitag.h"

typedef enum {
    ReadHitag2PageStateIdle,
    ReadHitag2PageStateRunning,
    ReadHitag2PageStateSuccess,
    ReadHitag2PageStateNotFound,
    ReadHitag2PageStateTimeout,
    ReadHitag2PageStateError,
} ReadHitag2PageState;

typedef struct {
    ReadHitag2PageState state;
    int result;
    uint32_t uid;
    bool uid_valid;
} ReadHitag2PageModel;

struct ReadHitag2Page {
    View* view;

    FuriThread* worker_thread;
    volatile bool worker_thread_running;
    volatile bool worker_thread_cancel_requested;

    ReadHitag2PageBackCallback back_callback;
    void* back_callback_context;
};

static int read_hitag2_wait_for_response_interruptible(
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

static const char* getHitagTypeStr(uint32_t uid) {
    //uid s/n        ********
    uint8_t type = (uid >> 4) & 0xF;
    switch(type) {
    case 1:
        return "PCF 7936";
    case 2:
        return "PCF 7946";
    case 3:
        return "PCF 7947";
    case 4:
        return "PCF 7942/44";
    case 5:
        return "PCF 7943";
    case 6:
        return "PCF 7941";
    case 7:
        return "PCF 7952";
    case 8:
        return "PCF 7961";
    case 9:
        return "PCF 7945";
    default:
        return "Hitag2";
    }
}

static int
    read_hitag2_test(volatile bool* cancel_requested, uint32_t* out_uid, bool* out_uid_valid) {
    if(out_uid) {
        *out_uid = 0;
    }
    if(out_uid_valid) {
        *out_uid_valid = false;
    }

    lf_hitag_data_t* packet = malloc(sizeof(lf_hitag_data_t));
    PacketResponseNG* resp = malloc(sizeof(PacketResponseNG));
    if(!packet || !resp) {
        if(packet) {
            free(packet);
        }
        if(resp) {
            free(resp);
        }
        return PM3_EMALLOC;
    }

    memset(packet, 0, sizeof(lf_hitag_data_t));

    // Use HT2F_UID_ONLY so PM5 executes ht2_read_uid(), which works reliably over
    // CEP SPI for both Password and Crypto Hitag2 tags without aborting on RDBF.
    int pm3cmd = CMD_LF_HITAG_READER;
    packet->cmd = HT2F_UID_ONLY;

    int final_res = PM3_EFAILED;
    for(int attempt = 0; attempt < 3; attempt++) {
        if(cancel_requested && *cancel_requested) {
            free(packet);
            free(resp);
            return PM3_EOPABORTED;
        }

        clearCommandBuffer();
        SendCommandNG(pm3cmd, (uint8_t*)packet, sizeof(lf_hitag_data_t));

        int wait_result =
            read_hitag2_wait_for_response_interruptible(pm3cmd, resp, 1500, cancel_requested);
        if(wait_result != PM3_SUCCESS) {
            if(wait_result == PM3_EOPABORTED) {
                free(packet);
                free(resp);
                return PM3_EOPABORTED;
            }
            final_res = wait_result;
            continue;
        }

        if(resp->status == PM3_SUCCESS && resp->length >= HITAG_UID_SIZE) {
            uint32_t uid = ((uint32_t)resp->data.asBytes[0] << 24) |
                           ((uint32_t)resp->data.asBytes[1] << 16) |
                           ((uint32_t)resp->data.asBytes[2] << 8) |
                           (uint32_t)resp->data.asBytes[3];
            if(out_uid) {
                *out_uid = uid;
            }
            if(out_uid_valid) {
                *out_uid_valid = true;
            }
            free(packet);
            free(resp);
            return PM3_SUCCESS;
        }

        final_res = resp->status;
        furi_delay_ms(80);
    }

    free(packet);
    free(resp);
    return final_res;
}

static void read_hitag2_page_draw_callback(Canvas* canvas, void* context) {
    ReadHitag2PageModel* model = context;
    if(!model) {
        return;
    }

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 4, 14, "LF Hitag2 Reader");
    canvas_set_font(canvas, FontSecondary);

    switch(model->state) {
    case ReadHitag2PageStateIdle:
        canvas_draw_str(canvas, 4, 32, "Press Scan to read tag");
        break;
    case ReadHitag2PageStateRunning:
        canvas_draw_str(canvas, 4, 32, "Scanning 125 kHz field...");
        canvas_draw_str(canvas, 4, 44, "Place Hitag2 tag on LF coil");
        break;
    case ReadHitag2PageStateSuccess:
        if(model->uid_valid) {
            const char* type_str = getHitagTypeStr(model->uid);
            char uid_line[32];
            char type_line[40];
            snprintf(uid_line, sizeof(uid_line), "UID:  %08lX", (unsigned long)model->uid);
            snprintf(type_line, sizeof(type_line), "Chip: %s", type_str ? type_str : "Hitag2");
            canvas_draw_str(canvas, 4, 29, uid_line);
            canvas_draw_str(canvas, 4, 41, type_line);
        } else {
            canvas_draw_str(canvas, 4, 34, "UID: n/a");
        }
        break;
    case ReadHitag2PageStateNotFound:
        canvas_draw_str(canvas, 4, 32, "No Hitag2 tag detected");
        canvas_draw_str(canvas, 4, 44, "Place tag on LF coil & Scan");
        break;
    case ReadHitag2PageStateTimeout:
        canvas_draw_str(canvas, 4, 34, "Device not responding");
        break;
    case ReadHitag2PageStateError:
        canvas_draw_str(canvas, 4, 34, "Read error");
        break;
    default:
        break;
    }

    elements_button_left(canvas, "Back");
    if(model->state != ReadHitag2PageStateRunning) {
        elements_button_center(canvas, "Scan");
    }
}

static bool read_hitag2_page_input_callback(InputEvent* event, void* context) {
    ReadHitag2Page* read_hitag2_page = context;
    if(!read_hitag2_page || !event) {
        return false;
    }

    if(event->type == InputTypeShort && event->key == InputKeyLeft) {
        read_hitag2_page_stop(read_hitag2_page);
        if(read_hitag2_page->back_callback) {
            read_hitag2_page->back_callback(read_hitag2_page->back_callback_context);
        }
        return true;
    }

    if(event->type == InputTypeShort && event->key == InputKeyOk) {
        if(!read_hitag2_page->worker_thread_running) {
            read_hitag2_page_start(read_hitag2_page);
            return true;
        }
    }

    return false;
}

static void read_hitag2_page_cleanup_worker(ReadHitag2Page* read_hitag2_page) {
    if(!read_hitag2_page || !read_hitag2_page->worker_thread) {
        return;
    }

    if(read_hitag2_page->worker_thread_running) {
        return;
    }

    furi_thread_join(read_hitag2_page->worker_thread);
    furi_thread_free(read_hitag2_page->worker_thread);
    read_hitag2_page->worker_thread = NULL;
}

static int32_t read_hitag2_page_worker(void* context) {
    ReadHitag2Page* read_hitag2_page = context;
    if(!read_hitag2_page) {
        return 0;
    }

    uint32_t uid = 0;
    bool uid_valid = false;
    int result =
        read_hitag2_test(&read_hitag2_page->worker_thread_cancel_requested, &uid, &uid_valid);

    ReadHitag2PageModel* model = view_get_model(read_hitag2_page->view);
    model->result = result;
    if(result == PM3_SUCCESS) {
        model->state = ReadHitag2PageStateSuccess;
        model->uid = uid;
        model->uid_valid = uid_valid;
        FURI_LOG_I("PM5_QA_Hitag2", "Tag found: UID=%08lX", (unsigned long)uid);
    } else if(result == PM3_EOPABORTED) {
        model->state = ReadHitag2PageStateIdle;
    } else if(result == PM3_EFAILED) {
        model->state = ReadHitag2PageStateNotFound;
        FURI_LOG_I("PM5_QA_Hitag2", "OK (No Hitag2 tag in field)");
    } else if(result == PM3_ETIMEOUT) {
        model->state = ReadHitag2PageStateTimeout;
        FURI_LOG_W("PM5_QA_Hitag2", "TIMEOUT");
    } else {
        model->state = ReadHitag2PageStateError;
        FURI_LOG_W("PM5_QA_Hitag2", "ERROR res=%d", result);
    }
    view_commit_model(read_hitag2_page->view, true);

    read_hitag2_page->worker_thread_running = false;
    return 0;
}

ReadHitag2Page*
    read_hitag2_page_create(ReadHitag2PageBackCallback back_callback, void* back_callback_context) {
    ReadHitag2Page* read_hitag2_page = calloc(1, sizeof(ReadHitag2Page));
    if(!read_hitag2_page) {
        return NULL;
    }

    read_hitag2_page->view = view_alloc();
    if(!read_hitag2_page->view) {
        free(read_hitag2_page);
        return NULL;
    }

    read_hitag2_page->back_callback = back_callback;
    read_hitag2_page->back_callback_context = back_callback_context;

    view_set_context(read_hitag2_page->view, read_hitag2_page);
    // Locking, not LockFree: the worker thread mutates this model while the GUI
    // thread draws from it.
    view_allocate_model(read_hitag2_page->view, ViewModelTypeLocking, sizeof(ReadHitag2PageModel));
    view_set_draw_callback(read_hitag2_page->view, read_hitag2_page_draw_callback);
    view_set_input_callback(read_hitag2_page->view, read_hitag2_page_input_callback);

    ReadHitag2PageModel* model = view_get_model(read_hitag2_page->view);
    model->state = ReadHitag2PageStateIdle;
    model->result = PM3_SUCCESS;
    model->uid = 0;
    model->uid_valid = false;
    // Required: with a locking model, view_get_model() holds the mutex until commit.
    view_commit_model(read_hitag2_page->view, false);

    return read_hitag2_page;
}

void read_hitag2_page_free(ReadHitag2Page* read_hitag2_page) {
    if(!read_hitag2_page) {
        return;
    }

    // Runs on the app thread after the view dispatcher has stopped, so blocking here
    // is safe. The worker notices the cancel flag within ~10ms and issues its own
    // CMD_BREAK_LOOP, which keeps SPI traffic on the worker thread where it belongs.
    read_hitag2_page->worker_thread_cancel_requested = true;

    if(read_hitag2_page->worker_thread) {
        furi_thread_join(read_hitag2_page->worker_thread);
        furi_thread_free(read_hitag2_page->worker_thread);
        read_hitag2_page->worker_thread = NULL;
        read_hitag2_page->worker_thread_running = false;
    }

    if(read_hitag2_page->view) {
        view_set_context(read_hitag2_page->view, NULL);
        view_free_model(read_hitag2_page->view);
        view_free(read_hitag2_page->view);
        read_hitag2_page->view = NULL;
    }

    free(read_hitag2_page);
}

View* read_hitag2_page_get_view(ReadHitag2Page* read_hitag2_page) {
    if(!read_hitag2_page) {
        return NULL;
    }

    return read_hitag2_page->view;
}

void read_hitag2_page_start(ReadHitag2Page* read_hitag2_page) {
    if(!read_hitag2_page || !read_hitag2_page->view) {
        return;
    }

    read_hitag2_page_cleanup_worker(read_hitag2_page);

    // Check cleanup_worker()'s pointer, not worker_thread_running - that flag
    // can flip false between the two checks and leak the old FuriThread.
    if(read_hitag2_page->worker_thread) {
        return;
    }

    ReadHitag2PageModel* model = view_get_model(read_hitag2_page->view);
    model->state = ReadHitag2PageStateRunning;
    model->result = PM3_SUCCESS;
    model->uid = 0;
    model->uid_valid = false;
    view_commit_model(read_hitag2_page->view, true);

    read_hitag2_page->worker_thread_cancel_requested = false;

    // Upstream noted this size was provisional ("此处的栈大小仅供测试"). The frame ring no
    // longer puts ~9KB of PacketResponseNG on the stack, but PacketResponseNG is still
    // ~560 bytes and several live in this call path, so keep real headroom.
    read_hitag2_page->worker_thread =
        furi_thread_alloc_ex("ReadHitag2Task", 8192, read_hitag2_page_worker, read_hitag2_page);
    if(!read_hitag2_page->worker_thread) {
        model = view_get_model(read_hitag2_page->view);
        model->state = ReadHitag2PageStateError;
        model->result = PM3_EMALLOC;
        view_commit_model(read_hitag2_page->view, true);
        return;
    }

    read_hitag2_page->worker_thread_running = true;
    furi_thread_start(read_hitag2_page->worker_thread);
}

// Called from the GUI thread (view previous-callback / left button), so it must never
// block. It only raises the cancel flag; the worker sends CMD_BREAK_LOOP itself and the
// thread is reaped later by read_hitag2_page_start() or read_hitag2_page_free().
//
// The original version called furi_thread_join() here via read_hitag2_page_cleanup_worker(),
// which blocked the ViewDispatcher thread and froze the whole UI after a failed read.
void read_hitag2_page_stop(ReadHitag2Page* read_hitag2_page) {
    if(!read_hitag2_page || !read_hitag2_page->worker_thread) {
        return;
    }

    read_hitag2_page->worker_thread_cancel_requested = true;
}
