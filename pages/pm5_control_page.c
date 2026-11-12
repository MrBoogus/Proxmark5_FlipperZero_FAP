#include <furi.h>
#include <gui/canvas.h>
#include <gui/elements.h>
#include <string.h>
#include "pm5_control_page.h"
#include "proxmark5_com.h"
#include "proxmark5_frame.h"
#include "pm3_cmd.h"

typedef enum {
    Pm5CtrlItemLfFreq = 0,
    Pm5CtrlItemQFactor,
    Pm5CtrlItemAntLeds,
    Pm5CtrlItemRgbColor,
    Pm5CtrlItemPowerSave,
    Pm5CtrlItemCount,
} Pm5CtrlItem;

typedef struct {
    const char* name;
    uint8_t r;
    uint8_t g;
    uint8_t b;
} Pm5RgbPreset;

static const Pm5RgbPreset k_rgb_presets[] = {
    {"Auto (Default)", 0, 0, 0},
    {"Cyan", 0, 200, 255},
    {"Green", 0, 255, 40},
    {"Blue", 0, 60, 255},
    {"Purple", 180, 0, 255},
    {"Amber", 255, 140, 0},
    {"Red", 255, 0, 0},
};
#define PM5_RGB_PRESET_COUNT (sizeof(k_rgb_presets) / sizeof(k_rgb_presets[0]))

static const char* const k_lf_freq_names[5] = {
    "125 kHz",
    "134 kHz",
    "250 kHz",
    "375 kHz",
    "500 kHz",
};

// One-hot bit layout of I2C 0x51 reg 0x02:
// Bit 7 (0x80): 125k, Bit 6 (0x40): 134k, Bit 5 (0x20): 250k,
// Bit 4 (0x10): 375k, Bit 3 (0x08): 500k,
// Bit 2 (0x04): HF LED, Bit 1 (0x02): LF LED, Bit 0 (0x01): High Q
static uint8_t pm5_ant_reg_to_freq_idx(uint8_t ant_reg) {
    if(ant_reg & 0x80) return 0; // 125 kHz
    if(ant_reg & 0x40) return 1; // 134 kHz
    if(ant_reg & 0x20) return 2; // 250 kHz
    if(ant_reg & 0x10) return 3; // 375 kHz
    if(ant_reg & 0x08) return 4; // 500 kHz
    return 0;
}

static uint8_t pm5_freq_idx_to_ant_mask(uint8_t freq_idx) {
    if(freq_idx > 4) freq_idx = 0;
    return (uint8_t)(0x80u >> freq_idx);
}

typedef struct {
    bool synced;
    bool busy;
    bool error;
    uint8_t cursor;
    uint8_t ant_reg;     // I2C 0x51 reg 0x02
    uint8_t rgb_idx;     // index into k_rgb_presets
    bool powersave_on;   // CMD_PM5_POWERSAVE
} Pm5ControlModel;

typedef enum {
    Pm5CtrlJobInit = 0,
    Pm5CtrlJobWriteAnt,
    Pm5CtrlJobWriteRgb,
    Pm5CtrlJobWritePowersave,
} Pm5CtrlJobType;

struct Pm5ControlPage {
    View* view;
    FuriThread* worker_thread;
    volatile bool worker_thread_running;
    Pm5CtrlJobType job_type;
    uint8_t pending_ant_reg;
    uint8_t pending_rgb_idx;
    bool pending_powersave;
    Pm5ControlPageBackCallback back_callback;
    void* back_callback_context;
};

static void pm5_control_launch_job(Pm5ControlPage* page, Pm5CtrlJobType job);

static void pm5_control_draw_callback(Canvas* canvas, void* context) {
    Pm5ControlModel* m = context;
    if(!m) {
        return;
    }

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 4, 10, "PM5 Hardware Ctrl");
    canvas_set_font(canvas, FontSecondary);
    if(m->busy) {
        canvas_draw_str_aligned(canvas, 124, 10, AlignRight, AlignBottom, "Sync...");
    } else if(m->error) {
        canvas_draw_str_aligned(canvas, 124, 10, AlignRight, AlignBottom, "Err");
    }

    uint8_t freq_idx = pm5_ant_reg_to_freq_idx(m->ant_reg);
    bool high_q = (m->ant_reg & 0x01) != 0;
    bool lf_led = (m->ant_reg & 0x02) != 0;
    bool hf_led = (m->ant_reg & 0x04) != 0;

    const char* led_state_str = "Off";
    if(hf_led && lf_led) {
        led_state_str = "HF+LF On";
    } else if(hf_led) {
        led_state_str = "HF On";
    } else if(lf_led) {
        led_state_str = "LF On";
    }

    char rows[Pm5CtrlItemCount][40];
    snprintf(rows[0], sizeof(rows[0]), "LF Resonance: %s", k_lf_freq_names[freq_idx]);
    snprintf(
        rows[1],
        sizeof(rows[1]),
        "Antenna Q:    %s",
        high_q ? "High Q (125/134k)" : "Low Q (Std)");
    snprintf(rows[2], sizeof(rows[2]), "Coil LEDs:    %s", led_state_str);
    snprintf(rows[3], sizeof(rows[3]), "RGB Light:    %s", k_rgb_presets[m->rgb_idx].name);
    snprintf(rows[4], sizeof(rows[4]), "MCU PwrSave:  %s", m->powersave_on ? "On (48MHz)" : "Off");

    // Display 4 visible items with scrolling window
    int first = 0;
    if(m->cursor >= 4) {
        first = m->cursor - 3;
    }
    int y = 20;
    for(int i = 0; i < 4 && (first + i) < Pm5CtrlItemCount; i++) {
        int idx = first + i;
        char line[44];
        snprintf(
            line,
            sizeof(line),
            "%c %s",
            (idx == m->cursor) ? '>' : ' ',
            rows[idx]);
        canvas_draw_str(canvas, 2, y, line);
        y += 9;
    }

    elements_button_left(canvas, "Back");
    if(!m->busy) {
        elements_button_center(canvas, "Change");
    }
}

static void pm5_control_cycle_selected(Pm5ControlPage* page) {
    if(page->worker_thread_running) {
        return;
    }

    Pm5ControlModel* m = view_get_model(page->view);
    uint8_t cursor = m->cursor;
    uint8_t ant_reg = m->ant_reg;
    uint8_t rgb_idx = m->rgb_idx;
    bool powersave = m->powersave_on;
    view_commit_model(page->view, false);

    if(cursor == Pm5CtrlItemLfFreq) {
        uint8_t freq_idx = pm5_ant_reg_to_freq_idx(ant_reg);
        freq_idx = (freq_idx + 1) % 5;
        ant_reg = (ant_reg & 0x07) | pm5_freq_idx_to_ant_mask(freq_idx);
        // Hardware rule: High Q (bit 0) is only allowed for 125 kHz (0) and 134 kHz (1)
        if(freq_idx >= 2) {
            ant_reg &= ~0x01;
        }
        page->pending_ant_reg = ant_reg;
        pm5_control_launch_job(page, Pm5CtrlJobWriteAnt);
    } else if(cursor == Pm5CtrlItemQFactor) {
        uint8_t freq_idx = pm5_ant_reg_to_freq_idx(ant_reg);
        if(freq_idx >= 2) {
            // Reset to 125 kHz (0x80) if user enables High Q while on >=250 kHz
            ant_reg = 0x80 | (ant_reg & 0x06) | 0x01;
        } else {
            if((ant_reg & 0xF8) == 0) {
                ant_reg |= 0x80;
            }
            ant_reg ^= 0x01;
        }
        page->pending_ant_reg = ant_reg;
        pm5_control_launch_job(page, Pm5CtrlJobWriteAnt);
    } else if(cursor == Pm5CtrlItemAntLeds) {
        if((ant_reg & 0xF8) == 0) {
            ant_reg |= 0x80;
        }
        uint8_t leds = (ant_reg >> 1) & 0x03;
        leds = (leds + 1) & 0x03;
        ant_reg = (ant_reg & ~0x06) | (leds << 1);
        page->pending_ant_reg = ant_reg;
        pm5_control_launch_job(page, Pm5CtrlJobWriteAnt);
    } else if(cursor == Pm5CtrlItemRgbColor) {
        rgb_idx = (rgb_idx + 1) % PM5_RGB_PRESET_COUNT;
        page->pending_rgb_idx = rgb_idx;
        pm5_control_launch_job(page, Pm5CtrlJobWriteRgb);
    } else if(cursor == Pm5CtrlItemPowerSave) {
        page->pending_powersave = !powersave;
        pm5_control_launch_job(page, Pm5CtrlJobWritePowersave);
    }
}

static bool pm5_control_input_callback(InputEvent* event, void* context) {
    Pm5ControlPage* page = context;
    if(!page || !event) {
        return false;
    }

    if(event->type == InputTypeShort && event->key == InputKeyLeft) {
        pm5_control_page_stop(page);
        if(page->back_callback) {
            page->back_callback(page->back_callback_context);
        }
        return true;
    }

    if(event->type == InputTypeShort || event->type == InputTypeRepeat) {
        if(event->key == InputKeyUp) {
            Pm5ControlModel* m = view_get_model(page->view);
            m->cursor = (m->cursor + Pm5CtrlItemCount - 1) % Pm5CtrlItemCount;
            view_commit_model(page->view, true);
            return true;
        }
        if(event->key == InputKeyDown) {
            Pm5ControlModel* m = view_get_model(page->view);
            m->cursor = (m->cursor + 1) % Pm5CtrlItemCount;
            view_commit_model(page->view, true);
            return true;
        }
    }

    if(event->type == InputTypeShort &&
       (event->key == InputKeyOk || event->key == InputKeyRight)) {
        pm5_control_cycle_selected(page);
        return true;
    }

    return false;
}

static void pm5_control_cleanup_worker(Pm5ControlPage* page) {
    if(!page || !page->worker_thread || page->worker_thread_running) {
        return;
    }
    furi_thread_join(page->worker_thread);
    furi_thread_free(page->worker_thread);
    page->worker_thread = NULL;
}

static int32_t pm5_control_worker(void* context) {
    Pm5ControlPage* page = context;
    if(!page) {
        return 0;
    }

    PacketResponseNG* resp = malloc(sizeof(PacketResponseNG));
    if(!resp) {
        Pm5ControlModel* m = view_get_model(page->view);
        m->busy = false;
        m->error = true;
        view_commit_model(page->view, true);
        page->worker_thread_running = false;
        return 0;
    }

    bool ok = false;
    uint8_t read_ant = 0;

    if(page->job_type == Pm5CtrlJobInit) {
        uint8_t reg_type = 1; // map reg (0x02)
        if(proxmark5_com_request_ng(
               CMD_ANT_CONTROL_READ, &reg_type, 1, CMD_ANT_CONTROL_READ, resp, 1000, 2) &&
           resp->status == PM3_SUCCESS && resp->length >= 1) {
            read_ant = resp->data.asBytes[0];
            ok = true;
        }
    } else if(page->job_type == Pm5CtrlJobWriteAnt) {
        struct {
            uint8_t data;
            uint8_t reg_type;
        } PACKED payload = {
            .data = page->pending_ant_reg,
            .reg_type = 1,
        };
        if(proxmark5_com_request_ng(
               CMD_ANT_CONTROL_WRITE,
               (uint8_t*)&payload,
               sizeof(payload),
               CMD_ANT_CONTROL_WRITE,
               resp,
               1000,
               2) &&
           resp->status == PM3_SUCCESS) {
            read_ant = page->pending_ant_reg;
            ok = true;
        }
    } else if(page->job_type == Pm5CtrlJobWriteRgb) {
        const Pm5RgbPreset* p = &k_rgb_presets[page->pending_rgb_idx];
        struct {
            uint8_t r;
            uint8_t g;
            uint8_t b;
        } PACKED rgb = {
            .r = p->r,
            .g = p->g,
            .b = p->b,
        };
        if(proxmark5_com_request_ng(
               CMD_PM5_RGB_SET, (uint8_t*)&rgb, sizeof(rgb), CMD_PM5_RGB_SET, resp, 1000, 2) &&
           resp->status == PM3_SUCCESS) {
            ok = true;
        }
    } else if(page->job_type == Pm5CtrlJobWritePowersave) {
        uint8_t en = page->pending_powersave ? 1 : 0;
        if(proxmark5_com_request_ng(
               CMD_PM5_POWERSAVE, &en, 1, CMD_PM5_POWERSAVE, resp, 1000, 2) &&
           resp->status == PM3_SUCCESS) {
            ok = true;
        }
    }

    Pm5ControlModel* m = view_get_model(page->view);
    m->busy = false;
    m->error = !ok;
    if(ok) {
        if(page->job_type == Pm5CtrlJobInit || page->job_type == Pm5CtrlJobWriteAnt) {
            m->ant_reg = read_ant;
            m->synced = true;
            FURI_LOG_I("PM5_QA_Ctrl", "ANT reg=0x%02X job=%d OK", read_ant, page->job_type);
        } else if(page->job_type == Pm5CtrlJobWriteRgb) {
            m->rgb_idx = page->pending_rgb_idx;
            FURI_LOG_I(
                "PM5_QA_Ctrl",
                "RGB preset=%s OK",
                k_rgb_presets[m->rgb_idx].name);
        } else if(page->job_type == Pm5CtrlJobWritePowersave) {
            m->powersave_on = page->pending_powersave;
            FURI_LOG_I("PM5_QA_Ctrl", "POWERSAVE=%d OK", m->powersave_on ? 1 : 0);
        }
    } else {
        FURI_LOG_W("PM5_QA_Ctrl", "Job %d FAILED", page->job_type);
    }
    view_commit_model(page->view, true);

    free(resp);
    page->worker_thread_running = false;
    return 0;
}

static void pm5_control_launch_job(Pm5ControlPage* page, Pm5CtrlJobType job) {
    if(!page || !page->view) {
        return;
    }
    pm5_control_cleanup_worker(page);
    if(page->worker_thread) {
        return;
    }

    page->job_type = job;
    Pm5ControlModel* m = view_get_model(page->view);
    m->busy = true;
    m->error = false;
    view_commit_model(page->view, true);

    page->worker_thread =
        furi_thread_alloc_ex("Pm5ControlTask", 4096, pm5_control_worker, page);
    if(!page->worker_thread) {
        m = view_get_model(page->view);
        m->busy = false;
        m->error = true;
        view_commit_model(page->view, true);
        return;
    }

    page->worker_thread_running = true;
    furi_thread_start(page->worker_thread);
}

Pm5ControlPage* pm5_control_page_create(
    Pm5ControlPageBackCallback back_callback,
    void* back_callback_context) {
    Pm5ControlPage* page = calloc(1, sizeof(Pm5ControlPage));
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
    view_allocate_model(page->view, ViewModelTypeLocking, sizeof(Pm5ControlModel));
    view_set_draw_callback(page->view, pm5_control_draw_callback);
    view_set_input_callback(page->view, pm5_control_input_callback);

    Pm5ControlModel* m = view_get_model(page->view);
    m->synced = false;
    m->busy = false;
    m->error = false;
    m->cursor = 0;
    m->ant_reg = 0x80;
    m->rgb_idx = 0;
    m->powersave_on = true;
    view_commit_model(page->view, false);

    return page;
}

void pm5_control_page_free(Pm5ControlPage* page) {
    if(!page) {
        return;
    }
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

View* pm5_control_page_get_view(Pm5ControlPage* page) {
    return page ? page->view : NULL;
}

void pm5_control_page_start(Pm5ControlPage* page) {
    pm5_control_launch_job(page, Pm5CtrlJobInit);
}

void pm5_control_page_stop(Pm5ControlPage* page) {
    UNUSED(page);
}
