#include <furi.h>
#include <gui/canvas.h>
#include <gui/elements.h>
#include <stdio.h>
#include <string.h>
#include "rf_scope_page.h"
#include "proxmark5_com.h"
#include "proxmark5_frame.h"
#include "pm3_cmd.h"

#define SCOPE_SAMPLES 116U

typedef enum {
    RfScopeBandHf1356 = 0,
    RfScopeBandLf125 = 1,
    RfScopeBandLf134 = 2,
    RfScopeBandCount = 3,
} RfScopeBand;

typedef struct {
    bool running;
    RfScopeBand band;
    uint32_t current_mv;
    uint32_t peak_mv;
    uint16_t history_mv[SCOPE_SAMPLES];
    uint8_t sample_count;
} RfScopeModel;

struct RfScopePage {
    View* view;
    FuriThread* worker_thread;
    volatile bool worker_running;
    volatile bool cancel_requested;
    volatile bool reset_peak_requested;
    volatile RfScopeBand requested_band;

    RfScopePageBackCallback back_callback;
    void* back_callback_context;
};

static const char* const k_band_names[RfScopeBandCount] = {
    "HF 13.56M",
    "LF 125.0k",
    "LF 134.8k",
};

static void rf_scope_format_volts(uint32_t mv, char* out, size_t out_size) {
    uint32_t v_int = mv / 1000U;
    uint32_t v_frac = (mv % 1000U) / 10U;
    snprintf(out, out_size, "%lu.%02luV", (unsigned long)v_int, (unsigned long)v_frac);
}

static void rf_scope_set_smart_antenna_lf(RfScopeBand band, PacketResponseNG* resp) {
    if(band == RfScopeBandHf1356) return;
    struct {
        uint8_t data;
        uint8_t reg_type;
    } PACKED ant_cmd = {
        .data = (band == RfScopeBandLf125) ? 0x81 : 0x41,
        .reg_type = 1,
    };
    proxmark5_com_request_ng(
        CMD_ANT_CONTROL_WRITE,
        (uint8_t*)&ant_cmd,
        sizeof(ant_cmd),
        CMD_ANT_CONTROL_WRITE,
        resp,
        300,
        1);
}

static bool rf_scope_band_start(RfScopeBand band, PacketResponseNG* resp) {
    if(band == RfScopeBandHf1356) {
        uint8_t act = 1;
        return proxmark5_com_request_ng(
            CMD_MEASURE_ANTENNA_TUNING_HF,
            &act,
            1,
            CMD_MEASURE_ANTENNA_TUNING_HF,
            resp,
            1200,
            2);
    } else {
        rf_scope_set_smart_antenna_lf(band, resp);
        uint8_t divisor = (band == RfScopeBandLf125) ? 95 : 88;
        uint8_t req[2] = {1, divisor};
        return proxmark5_com_request_ng(
            CMD_MEASURE_ANTENNA_TUNING_LF,
            req,
            sizeof(req),
            CMD_MEASURE_ANTENNA_TUNING_LF,
            resp,
            1200,
            2);
    }
}

static void rf_scope_band_stop(RfScopeBand band, PacketResponseNG* resp) {
    if(band == RfScopeBandHf1356) {
        uint8_t act = 3;
        proxmark5_com_request_ng(
            CMD_MEASURE_ANTENNA_TUNING_HF,
            &act,
            1,
            CMD_MEASURE_ANTENNA_TUNING_HF,
            resp,
            600,
            1);
    } else {
        uint8_t divisor = (band == RfScopeBandLf125) ? 95 : 88;
        uint8_t req[2] = {3, divisor};
        proxmark5_com_request_ng(
            CMD_MEASURE_ANTENNA_TUNING_LF,
            req,
            sizeof(req),
            CMD_MEASURE_ANTENNA_TUNING_LF,
            resp,
            600,
            1);
    }
}

static bool rf_scope_band_sample(RfScopeBand band, PacketResponseNG* resp, uint32_t* out_mv) {
    if(band == RfScopeBandHf1356) {
        uint8_t act = 2;
        clearCommandBuffer();
        SendCommandNG(CMD_MEASURE_ANTENNA_TUNING_HF, &act, 1);
        if(WaitForResponseTimeout(CMD_MEASURE_ANTENNA_TUNING_HF, resp, 350) &&
           resp->status == PM3_SUCCESS && resp->length >= sizeof(uint32_t)) {
            uint32_t mv = (uint32_t)resp->data.asBytes[0] |
                          ((uint32_t)resp->data.asBytes[1] << 8) |
                          ((uint32_t)resp->data.asBytes[2] << 16) |
                          ((uint32_t)resp->data.asBytes[3] << 24);
            *out_mv = mv;
            return true;
        }
    } else {
        uint8_t divisor = (band == RfScopeBandLf125) ? 95 : 88;
        uint8_t req[2] = {2, divisor};
        clearCommandBuffer();
        SendCommandNG(CMD_MEASURE_ANTENNA_TUNING_LF, req, sizeof(req));
        if(WaitForResponseTimeout(CMD_MEASURE_ANTENNA_TUNING_LF, resp, 350) &&
           resp->status == PM3_SUCCESS && resp->length >= sizeof(uint32_t)) {
            uint32_t mv = (uint32_t)resp->data.asBytes[0] |
                          ((uint32_t)resp->data.asBytes[1] << 8) |
                          ((uint32_t)resp->data.asBytes[2] << 16) |
                          ((uint32_t)resp->data.asBytes[3] << 24);
            *out_mv = mv;
            return true;
        }
    }
    return false;
}

static void rf_scope_page_draw_callback(Canvas* canvas, void* context) {
    RfScopeModel* model = context;
    if(!model) {
        return;
    }

    canvas_clear(canvas);
    canvas_set_font(canvas, FontSecondary);

    char now_str[16];
    char peak_str[16];
    rf_scope_format_volts(model->current_mv, now_str, sizeof(now_str));
    rf_scope_format_volts(model->peak_mv, peak_str, sizeof(peak_str));

    uint32_t drop_pct = 0;
    if(model->peak_mv > 0 && model->current_mv < model->peak_mv) {
        drop_pct = ((model->peak_mv - model->current_mv) * 100U) / model->peak_mv;
    }

    char line1[44];
    snprintf(
        line1,
        sizeof(line1),
        "[%s] Now:%s",
        k_band_names[model->band],
        now_str);
    canvas_draw_str(canvas, 2, 8, line1);

    char line2[44];
    snprintf(
        line2,
        sizeof(line2),
        "Peak:%s  LoadDrop:%lu%%",
        peak_str,
        (unsigned long)drop_pct);
    canvas_draw_str(canvas, 2, 17, line2);

    // Oscilloscope viewport: X=4..123 (width 120), Y=19..50 (height 32)
    const uint8_t box_x = 4;
    const uint8_t box_y = 19;
    const uint8_t box_w = 120;
    const uint8_t box_h = 32;
    canvas_draw_frame(canvas, box_x, box_y, box_w, box_h);

    // Dotted midline
    for(uint8_t gx = box_x + 2; gx < box_x + box_w - 2; gx += 4) {
        canvas_draw_dot(canvas, gx, box_y + box_h / 2);
    }

    uint32_t scale_max = model->peak_mv > 5000U ? model->peak_mv : 45000U;
    uint8_t prev_x = 0;
    uint8_t prev_y = 0;
    for(uint8_t i = 0; i < SCOPE_SAMPLES; i++) {
        uint32_t mv = model->history_mv[i];
        if(mv > scale_max) mv = scale_max;
        uint8_t plot_h = (uint8_t)((mv * (box_h - 4U)) / scale_max);
        uint8_t x = (uint8_t)(box_x + 2U + i);
        uint8_t y = (uint8_t)(box_y + box_h - 2U - plot_h);
        if(i == 0) {
            canvas_draw_dot(canvas, x, y);
        } else {
            canvas_draw_line(canvas, prev_x, prev_y, x, y);
        }
        prev_x = x;
        prev_y = y;
    }

    elements_button_left(canvas, "Back");
    elements_button_center(canvas, model->running ? "Pause" : "Run");
    elements_button_right(canvas, "Zero");
}

static bool rf_scope_page_input_callback(InputEvent* event, void* context) {
    RfScopePage* page = context;
    if(!page || !event) {
        return false;
    }

    if(event->type == InputTypeShort && event->key == InputKeyLeft) {
        rf_scope_page_stop(page);
        if(page->back_callback) {
            page->back_callback(page->back_callback_context);
        }
        return true;
    }

    if(event->type == InputTypeShort && event->key == InputKeyOk) {
        if(page->worker_running) {
            rf_scope_page_stop(page);
        } else {
            rf_scope_page_start(page);
        }
        return true;
    }

    if(event->type == InputTypeShort && event->key == InputKeyRight) {
        page->reset_peak_requested = true;
        RfScopeModel* model = view_get_model(page->view);
        model->peak_mv = model->current_mv;
        view_commit_model(page->view, true);
        return true;
    }

    if(event->type == InputTypeShort &&
       (event->key == InputKeyUp || event->key == InputKeyDown)) {
        RfScopeModel* model = view_get_model(page->view);
        RfScopeBand next_band = model->band;
        if(event->key == InputKeyUp) {
            next_band = (next_band == 0) ? (RfScopeBand)(RfScopeBandCount - 1) :
                                           (RfScopeBand)(next_band - 1);
        } else {
            next_band = (RfScopeBand)((next_band + 1) % RfScopeBandCount);
        }
        model->band = next_band;
        model->peak_mv = 0;
        memset(model->history_mv, 0, sizeof(model->history_mv));
        page->requested_band = next_band;
        view_commit_model(page->view, true);
        return true;
    }

    return false;
}

static void rf_scope_cleanup_worker(RfScopePage* page) {
    if(!page || !page->worker_thread || page->worker_running) {
        return;
    }
    furi_thread_join(page->worker_thread);
    furi_thread_free(page->worker_thread);
    page->worker_thread = NULL;
}

static int32_t rf_scope_worker(void* context) {
    RfScopePage* page = context;
    if(!page) {
        return 0;
    }

    PacketResponseNG* resp = malloc(sizeof(PacketResponseNG));
    if(!resp) {
        page->worker_running = false;
        return 0;
    }

    RfScopeBand active_band = page->requested_band;
    rf_scope_band_start(active_band, resp);
    bool logged_qa = false;

    while(!page->cancel_requested) {
        if(page->requested_band != active_band) {
            rf_scope_band_stop(active_band, resp);
            active_band = page->requested_band;
            rf_scope_band_start(active_band, resp);
        }

        uint32_t mv = 0;
        if(rf_scope_band_sample(active_band, resp, &mv)) {
            RfScopeModel* model = view_get_model(page->view);
            if(page->reset_peak_requested) {
                model->peak_mv = mv;
                page->reset_peak_requested = false;
            }
            model->current_mv = mv;
            if(mv > model->peak_mv) {
                model->peak_mv = mv;
            }
            memmove(
                &model->history_mv[0],
                &model->history_mv[1],
                (SCOPE_SAMPLES - 1U) * sizeof(uint16_t));
            model->history_mv[SCOPE_SAMPLES - 1U] = (uint16_t)(mv > 65535U ? 65535U : mv);
            if(model->sample_count < 250) {
                model->sample_count++;
            }
            if(!logged_qa && model->sample_count >= 3) {
                logged_qa = true;
                FURI_LOG_I(
                    "PM5_QA_RFSCOPE",
                    "Band=%s Now=%lu mV Peak=%lu mV",
                    k_band_names[active_band],
                    (unsigned long)mv,
                    (unsigned long)model->peak_mv);
            }
            view_commit_model(page->view, true);
        }

        furi_delay_ms(35);
    }

    rf_scope_band_stop(active_band, resp);
    free(resp);

    RfScopeModel* model = view_get_model(page->view);
    model->running = false;
    view_commit_model(page->view, true);

    page->worker_running = false;
    return 0;
}

RfScopePage*
    rf_scope_page_create(RfScopePageBackCallback back_callback, void* back_callback_context) {
    RfScopePage* page = calloc(1, sizeof(RfScopePage));
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
    page->requested_band = RfScopeBandHf1356;

    view_set_context(page->view, page);
    view_allocate_model(page->view, ViewModelTypeLocking, sizeof(RfScopeModel));
    view_set_draw_callback(page->view, rf_scope_page_draw_callback);
    view_set_input_callback(page->view, rf_scope_page_input_callback);

    RfScopeModel* model = view_get_model(page->view);
    memset(model, 0, sizeof(*model));
    model->band = RfScopeBandHf1356;
    view_commit_model(page->view, false);

    return page;
}

void rf_scope_page_free(RfScopePage* page) {
    if(!page) {
        return;
    }

    rf_scope_page_stop(page);

    if(page->view) {
        view_set_context(page->view, NULL);
        view_free_model(page->view);
        view_free(page->view);
        page->view = NULL;
    }

    free(page);
}

View* rf_scope_page_get_view(RfScopePage* page) {
    return page ? page->view : NULL;
}

void rf_scope_page_start(RfScopePage* page) {
    if(!page) {
        return;
    }

    rf_scope_cleanup_worker(page);
    if(page->worker_running) {
        return;
    }

    RfScopeModel* model = view_get_model(page->view);
    model->running = true;
    model->sample_count = 0;
    page->requested_band = model->band;
    view_commit_model(page->view, true);

    page->cancel_requested = false;
    page->worker_running = true;
    page->worker_thread = furi_thread_alloc_ex("RfScopeWorker", 2560, rf_scope_worker, page);
    furi_thread_start(page->worker_thread);
}

void rf_scope_page_stop(RfScopePage* page) {
    if(!page) {
        return;
    }

    if(page->worker_thread && page->worker_running) {
        page->cancel_requested = true;
        furi_thread_join(page->worker_thread);
        page->worker_running = false;
    }

    rf_scope_cleanup_worker(page);

    RfScopeModel* model = view_get_model(page->view);
    model->running = false;
    view_commit_model(page->view, false);
}
