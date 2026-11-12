#include <furi.h>
#include <gui/canvas.h>
#include <gui/elements.h>
#include <string.h>
#include "antenna_tune_page.h"
#include "proxmark5_com.h"
#include "proxmark5_frame.h"
#include "pm3_cmd.h"

typedef struct {
    uint32_t v_lf134;
    uint32_t v_lf125;
    uint32_t v_lfconf;
    uint32_t v_hf;
    uint32_t peak_v;
    uint32_t peak_f;
    int32_t divisor;
    uint8_t results[256];
} PACKED pm5_tune_payload_t;

typedef enum {
    AntennaTuneStateIdle,
    AntennaTuneStateRunning,
    AntennaTuneStateReady,
    AntennaTuneStateTimeout,
    AntennaTuneStateError,
} AntennaTuneState;

typedef struct {
    AntennaTuneState state;
    uint8_t tab; // 0 = Summary, 1 = LF Spectrum Graph
    pm5_tune_payload_t data;
} AntennaTuneModel;

struct AntennaTunePage {
    View* view;
    FuriThread* worker_thread;
    volatile bool worker_thread_running;
    volatile bool worker_thread_cancel_requested;
    AntennaTunePageBackCallback back_callback;
    void* back_callback_context;
};

static int antenna_tune_wait_interruptible(
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
        furi_delay_ms(15);
    }
    if(cancel_requested && *cancel_requested) {
        return PM3_EOPABORTED;
    }
    return proxmark5_frame_take_by_cmd(cmd, resp) ? PM3_SUCCESS : PM3_ETIMEOUT;
}

static void antenna_tune_draw_summary(Canvas* canvas, const pm5_tune_payload_t* d) {
    char line[44];

    uint32_t lf125_v = d->v_lf125 / 1000;
    uint32_t lf125_c = (d->v_lf125 % 1000) / 10;
    snprintf(
        line,
        sizeof(line),
        "LF 125.0k: %2lu.%02lu V %s",
        (unsigned long)lf125_v,
        (unsigned long)lf125_c,
        d->v_lf125 >= 10000 ? "[OK]" : "[LOW]");
    canvas_draw_str(canvas, 4, 22, line);

    uint32_t lf134_v = d->v_lf134 / 1000;
    uint32_t lf134_c = (d->v_lf134 % 1000) / 10;
    snprintf(
        line,
        sizeof(line),
        "LF 134.8k: %2lu.%02lu V",
        (unsigned long)lf134_v,
        (unsigned long)lf134_c);
    canvas_draw_str(canvas, 4, 31, line);

    uint32_t opt_freq_x10 = (d->peak_f > 0) ? (120000u / (d->peak_f + 1u)) : 0;
    uint32_t peak_v = d->peak_v / 1000;
    uint32_t peak_c = (d->peak_v % 1000) / 10;
    snprintf(
        line,
        sizeof(line),
        "LF Peak:   %2lu.%02lu V @%lu.%luk",
        (unsigned long)peak_v,
        (unsigned long)peak_c,
        (unsigned long)(opt_freq_x10 / 10),
        (unsigned long)(opt_freq_x10 % 10));
    canvas_draw_str(canvas, 4, 40, line);

    uint32_t hf_v = d->v_hf / 1000;
    uint32_t hf_c = (d->v_hf % 1000) / 10;
    snprintf(
        line,
        sizeof(line),
        "HF 13.56M: %2lu.%02lu V %s",
        (unsigned long)hf_v,
        (unsigned long)hf_c,
        d->v_hf >= 15000 ? "[OK]" : "[LOW]");
    canvas_draw_str(canvas, 4, 49, line);
}

static void antenna_tune_draw_graph(Canvas* canvas, const pm5_tune_payload_t* d) {
    // Plot divisors 39..158 (120 points, ~75 kHz to 300 kHz) across x = 4..123
    const int x_left = 4;
    const int width = 120;
    const int y_top = 14;
    const int y_bottom = 49;
    const int height = y_bottom - y_top;
    const int div_min = 39;

    // Axes
    canvas_draw_line(canvas, x_left, y_bottom, x_left + width - 1, y_bottom);
    canvas_draw_line(canvas, x_left, y_top, x_left, y_bottom);

    uint8_t max_val = 1;
    for(int i = 0; i < width; i++) {
        int div = div_min + i;
        if(div < 256 && d->results[div] > max_val) {
            max_val = d->results[div];
        }
    }

    // Dotted reference markers at 134.8 kHz (div 88) and 125.0 kHz (div 95)
    int x_134 = x_left + (88 - div_min);
    int x_125 = x_left + (95 - div_min);
    for(int y = y_top; y <= y_bottom; y += 3) {
        canvas_draw_dot(canvas, x_134, y);
        canvas_draw_dot(canvas, x_125, y);
    }

    int prev_x = x_left;
    int prev_y = y_bottom;
    for(int i = 0; i < width; i++) {
        int div = div_min + i;
        uint8_t val = (div < 256) ? d->results[div] : 0;
        int y = y_bottom - ((int)val * height) / (int)max_val;
        if(y < y_top) y = y_top;
        if(y > y_bottom) y = y_bottom;
        int x = x_left + i;
        if(i > 0) {
            canvas_draw_line(canvas, prev_x, prev_y, x, y);
        }
        prev_x = x;
        prev_y = y;
    }
}

static void antenna_tune_page_draw_callback(Canvas* canvas, void* context) {
    AntennaTuneModel* model = context;
    if(!model) {
        return;
    }

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    if(model->state == AntennaTuneStateReady && model->tab == 1) {
        canvas_draw_str(canvas, 4, 10, "LF Sweep (134k|125k)");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 124, 10, AlignRight, AlignBottom, "2/2");
    } else {
        canvas_draw_str(canvas, 4, 10, "Antenna Tuner");
        if(model->state == AntennaTuneStateReady) {
            canvas_set_font(canvas, FontSecondary);
            canvas_draw_str_aligned(canvas, 124, 10, AlignRight, AlignBottom, "1/2");
        }
    }

    canvas_set_font(canvas, FontSecondary);
    switch(model->state) {
    case AntennaTuneStateIdle:
        canvas_draw_str(canvas, 4, 30, "Press Tune to sweep LF/HF");
        break;
    case AntennaTuneStateRunning:
        canvas_draw_str(canvas, 4, 28, "Sweeping LF & HF coils...");
        canvas_draw_str(canvas, 4, 40, "Takes ~5s (keep tags away)");
        break;
    case AntennaTuneStateReady:
        if(model->tab == 0) {
            antenna_tune_draw_summary(canvas, &model->data);
        } else {
            antenna_tune_draw_graph(canvas, &model->data);
        }
        break;
    case AntennaTuneStateTimeout:
        canvas_draw_str(canvas, 4, 32, "Measurement timed out");
        break;
    case AntennaTuneStateError:
        canvas_draw_str(canvas, 4, 32, "Measurement failed");
        break;
    }

    elements_button_left(canvas, "Back");
    if(model->state != AntennaTuneStateRunning) {
        elements_button_center(canvas, "Tune");
    }
    if(model->state == AntennaTuneStateReady) {
        elements_button_right(canvas, model->tab == 0 ? "Graph" : "Data");
    }
}

static bool antenna_tune_page_input_callback(InputEvent* event, void* context) {
    AntennaTunePage* page = context;
    if(!page || !event) {
        return false;
    }

    if(event->type == InputTypeShort && event->key == InputKeyLeft) {
        antenna_tune_page_stop(page);
        if(page->back_callback) {
            page->back_callback(page->back_callback_context);
        }
        return true;
    }

    if(event->type == InputTypeShort && event->key == InputKeyRight) {
        AntennaTuneModel* model = view_get_model(page->view);
        bool handled = false;
        if(model->state == AntennaTuneStateReady) {
            model->tab = (model->tab + 1) % 2;
            handled = true;
        }
        view_commit_model(page->view, handled);
        return handled;
    }

    if(event->type == InputTypeShort && event->key == InputKeyOk) {
        if(!page->worker_thread_running) {
            antenna_tune_page_start(page);
            return true;
        }
    }

    return false;
}

static void antenna_tune_cleanup_worker(AntennaTunePage* page) {
    if(!page || !page->worker_thread || page->worker_thread_running) {
        return;
    }
    furi_thread_join(page->worker_thread);
    furi_thread_free(page->worker_thread);
    page->worker_thread = NULL;
}

static int32_t antenna_tune_worker(void* context) {
    AntennaTunePage* page = context;
    if(!page) {
        return 0;
    }

    PacketResponseNG* resp = malloc(sizeof(PacketResponseNG));
    if(!resp) {
        AntennaTuneModel* model = view_get_model(page->view);
        model->state = AntennaTuneStateError;
        view_commit_model(page->view, true);
        page->worker_thread_running = false;
        return 0;
    }

    clearCommandBuffer();
    SendCommandNG(CMD_MEASURE_ANTENNA_TUNING, NULL, 0);

    // MeasureAntennaTuning sweeps 237 LF divisors * 20ms + 100ms HF (~4.9s total)
    int wait_res = antenna_tune_wait_interruptible(
        CMD_MEASURE_ANTENNA_TUNING, resp, 8000, &page->worker_thread_cancel_requested);

    // MeasureAntennaTuning blocks PM5's main loop for ~4.9s (> CEP_ACTIVITY_TIMEOUT_MS = 3s),
    // which causes PM5's next cep_attach_poll() to drop s_cep_active to false.
    // Re-handshake immediately so subsequent commands work without delay.
    if(!page->worker_thread_cancel_requested) {
        proxmark5_com_handshake();
    }

    AntennaTuneModel* model = view_get_model(page->view);
    if(wait_res == PM3_SUCCESS && resp->status == PM3_SUCCESS &&
       resp->length >= sizeof(pm5_tune_payload_t)) {
        memcpy(&model->data, resp->data.asBytes, sizeof(pm5_tune_payload_t));
        model->state = AntennaTuneStateReady;
        FURI_LOG_I(
            "PM5_QA_Tune",
            "OK: LF125=%lumV LF134=%lumV HF=%lumV Peak=%lumV@div%u",
            (unsigned long)model->data.v_lf125,
            (unsigned long)model->data.v_lf134,
            (unsigned long)model->data.v_hf,
            (unsigned long)model->data.peak_v,
            (unsigned)model->data.peak_f);
    } else if(wait_res == PM3_EOPABORTED) {
        model->state = AntennaTuneStateIdle;
    } else if(wait_res == PM3_ETIMEOUT) {
        model->state = AntennaTuneStateTimeout;
        FURI_LOG_W("PM5_QA_Tune", "TIMEOUT");
    } else {
        model->state = AntennaTuneStateError;
        FURI_LOG_W("PM5_QA_Tune", "ERROR res=%d", wait_res);
    }
    view_commit_model(page->view, true);

    free(resp);
    page->worker_thread_running = false;
    return 0;
}

AntennaTunePage* antenna_tune_page_create(
    AntennaTunePageBackCallback back_callback,
    void* back_callback_context) {
    AntennaTunePage* page = calloc(1, sizeof(AntennaTunePage));
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
    view_allocate_model(page->view, ViewModelTypeLocking, sizeof(AntennaTuneModel));
    view_set_draw_callback(page->view, antenna_tune_page_draw_callback);
    view_set_input_callback(page->view, antenna_tune_page_input_callback);

    AntennaTuneModel* model = view_get_model(page->view);
    model->state = AntennaTuneStateIdle;
    model->tab = 0;
    memset(&model->data, 0, sizeof(model->data));
    view_commit_model(page->view, false);

    return page;
}

void antenna_tune_page_free(AntennaTunePage* page) {
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

View* antenna_tune_page_get_view(AntennaTunePage* page) {
    return page ? page->view : NULL;
}

void antenna_tune_page_start(AntennaTunePage* page) {
    if(!page || !page->view) {
        return;
    }
    antenna_tune_cleanup_worker(page);
    if(page->worker_thread) {
        return;
    }

    AntennaTuneModel* model = view_get_model(page->view);
    model->state = AntennaTuneStateRunning;
    view_commit_model(page->view, true);

    page->worker_thread_cancel_requested = false;
    page->worker_thread =
        furi_thread_alloc_ex("AntennaTuneTask", 4096, antenna_tune_worker, page);
    if(!page->worker_thread) {
        model = view_get_model(page->view);
        model->state = AntennaTuneStateError;
        view_commit_model(page->view, true);
        return;
    }

    page->worker_thread_running = true;
    furi_thread_start(page->worker_thread);
}

void antenna_tune_page_stop(AntennaTunePage* page) {
    if(!page || !page->worker_thread) {
        return;
    }
    page->worker_thread_cancel_requested = true;
}
