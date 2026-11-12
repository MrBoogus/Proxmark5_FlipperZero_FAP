#include <furi.h>
#include <furi_hal.h>
#include <fmps_cxt_icons.h>
#include "fmps_cxt.h"
#include "proxmark5_com.h"

static bool fmps_cxt_back_event_callback(void* context) {
    Proxmark5App* app = context;
    view_dispatcher_stop(app->view_dispatcher);
    return true;
}

static void fmps_cxt_open_operate_page(void* context) {
    Proxmark5App* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, Proxmark5CustomEventOpenOperatePage);
}

static void fmps_cxt_open_read_hitag2_page(void* context) {
    Proxmark5App* app = context;
    view_dispatcher_send_custom_event(
        app->view_dispatcher, Proxmark5CustomEventOpenReadHitag2Page);
}

static void fmps_cxt_open_read_hf14a_page(void* context) {
    Proxmark5App* app = context;
    view_dispatcher_send_custom_event(
        app->view_dispatcher, Proxmark5CustomEventOpenReadHf14aPage);
}

static void fmps_cxt_open_hf_mf_audit_page(void* context) {
    Proxmark5App* app = context;
    view_dispatcher_send_custom_event(
        app->view_dispatcher, Proxmark5CustomEventOpenHfMfAuditPage);
}

static void fmps_cxt_open_hf_sniff_page(void* context) {
    Proxmark5App* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, Proxmark5CustomEventOpenHfSniffPage);
}

static void fmps_cxt_open_read_hf_multi_page(void* context) {
    Proxmark5App* app = context;
    view_dispatcher_send_custom_event(
        app->view_dispatcher, Proxmark5CustomEventOpenReadHfMultiPage);
}

static void fmps_cxt_open_lf_lab_page(void* context) {
    Proxmark5App* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, Proxmark5CustomEventOpenLfLabPage);
}

static void fmps_cxt_open_t5577_suite_page(void* context) {
    Proxmark5App* app = context;
    view_dispatcher_send_custom_event(
        app->view_dispatcher, Proxmark5CustomEventOpenT5577SuitePage);
}

static void fmps_cxt_open_rf_scope_page(void* context) {
    Proxmark5App* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, Proxmark5CustomEventOpenRfScopePage);
}

static void fmps_cxt_open_antenna_tune_page(void* context) {
    Proxmark5App* app = context;
    view_dispatcher_send_custom_event(
        app->view_dispatcher, Proxmark5CustomEventOpenAntennaTunePage);
}

static void fmps_cxt_open_pm5_control_page(void* context) {
    Proxmark5App* app = context;
    view_dispatcher_send_custom_event(
        app->view_dispatcher, Proxmark5CustomEventOpenPm5ControlPage);
}

static void fmps_cxt_open_status_page(void* context) {
    Proxmark5App* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, Proxmark5CustomEventOpenStatusPage);
}

static bool fmps_cxt_custom_event_callback(void* context, uint32_t event) {
    Proxmark5App* app = context;

    switch(event) {
    case Proxmark5CustomEventOpenOperatePage:
        view_dispatcher_switch_to_view(app->view_dispatcher, OperatePageViewId);
        return true;
    case Proxmark5CustomEventOpenReadHitag2Page:
        view_dispatcher_switch_to_view(app->view_dispatcher, ReadHitag2PageViewId);
        read_hitag2_page_start(app->read_hitag2_page);
        return true;
    case Proxmark5CustomEventOpenReadHf14aPage:
        view_dispatcher_switch_to_view(app->view_dispatcher, ReadHf14aPageViewId);
        read_hf14a_page_start(app->read_hf14a_page);
        return true;
    case Proxmark5CustomEventOpenHfMfAuditPage:
        view_dispatcher_switch_to_view(app->view_dispatcher, HfMfAuditPageViewId);
        hf_mf_audit_page_start(app->hf_mf_audit_page);
        return true;
    case Proxmark5CustomEventOpenHfSniffPage:
        view_dispatcher_switch_to_view(app->view_dispatcher, HfSniffPageViewId);
        hf_sniff_page_start(app->hf_sniff_page);
        return true;
    case Proxmark5CustomEventOpenReadHfMultiPage:
        view_dispatcher_switch_to_view(app->view_dispatcher, ReadHfMultiPageViewId);
        read_hf_multi_page_start(app->read_hf_multi_page);
        return true;
    case Proxmark5CustomEventOpenLfLabPage:
        view_dispatcher_switch_to_view(app->view_dispatcher, LfLabPageViewId);
        lf_lab_page_start(app->lf_lab_page);
        return true;
    case Proxmark5CustomEventOpenT5577SuitePage:
        view_dispatcher_switch_to_view(app->view_dispatcher, T5577SuitePageViewId);
        return true;
    case Proxmark5CustomEventOpenRfScopePage:
        view_dispatcher_switch_to_view(app->view_dispatcher, RfScopePageViewId);
        rf_scope_page_start(app->rf_scope_page);
        return true;
    case Proxmark5CustomEventOpenAntennaTunePage:
        view_dispatcher_switch_to_view(app->view_dispatcher, AntennaTunePageViewId);
        antenna_tune_page_start(app->antenna_tune_page);
        return true;
    case Proxmark5CustomEventOpenPm5ControlPage:
        view_dispatcher_switch_to_view(app->view_dispatcher, Pm5ControlPageViewId);
        pm5_control_page_start(app->pm5_control_page);
        return true;
    case Proxmark5CustomEventOpenStatusPage:
        view_dispatcher_switch_to_view(app->view_dispatcher, StatusPageViewId);
        status_page_start(app->status_page);
        return true;
    default:
        return false;
    }
}

static uint32_t operate_page_previous_callback(void* context) {
    UNUSED(context);
    return MainPageViewId;
}

static uint32_t read_hitag2_page_previous_callback(void* context) {
    ReadHitag2Page* read_hitag2_page = context;
    read_hitag2_page_stop(read_hitag2_page);
    return OperatePageViewId;
}

static uint32_t read_hf14a_page_previous_callback(void* context) {
    ReadHf14aPage* read_hf14a_page = context;
    read_hf14a_page_stop(read_hf14a_page);
    return OperatePageViewId;
}

static uint32_t hf_mf_audit_page_previous_callback(void* context) {
    HfMfAuditPage* page = context;
    hf_mf_audit_page_stop(page);
    return OperatePageViewId;
}

static uint32_t hf_sniff_page_previous_callback(void* context) {
    HfSniffPage* page = context;
    hf_sniff_page_stop(page);
    return OperatePageViewId;
}

static uint32_t read_hf_multi_page_previous_callback(void* context) {
    ReadHfMultiPage* page = context;
    read_hf_multi_page_stop(page);
    return OperatePageViewId;
}

static uint32_t lf_lab_page_previous_callback(void* context) {
    LfLabPage* page = context;
    lf_lab_page_stop(page);
    return OperatePageViewId;
}

static uint32_t t5577_suite_page_previous_callback(void* context) {
    UNUSED(context);
    return OperatePageViewId;
}

static uint32_t rf_scope_page_previous_callback(void* context) {
    RfScopePage* page = context;
    rf_scope_page_stop(page);
    return OperatePageViewId;
}

static uint32_t antenna_tune_page_previous_callback(void* context) {
    AntennaTunePage* page = context;
    antenna_tune_page_stop(page);
    return OperatePageViewId;
}

static uint32_t pm5_control_page_previous_callback(void* context) {
    Pm5ControlPage* page = context;
    pm5_control_page_stop(page);
    return OperatePageViewId;
}

static uint32_t status_page_previous_callback(void* context) {
    StatusPage* status_page = context;
    status_page_stop(status_page);
    return OperatePageViewId;
}

Proxmark5App* proxmark5_app_alloc() {
    Proxmark5App* app = calloc(1, sizeof(Proxmark5App));
    app->gui = furi_record_open(RECORD_GUI);

    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    app->main_page = main_page_create(fmps_cxt_open_operate_page, app);
    view_dispatcher_add_view(app->view_dispatcher, MainPageViewId, app->main_page->canvas_view);

    app->operate_page = operate_page_create(
        fmps_cxt_open_read_hitag2_page,
        app,
        fmps_cxt_open_read_hf14a_page,
        app,
        fmps_cxt_open_hf_mf_audit_page,
        app,
        fmps_cxt_open_hf_sniff_page,
        app,
        fmps_cxt_open_read_hf_multi_page,
        app,
        fmps_cxt_open_lf_lab_page,
        app,
        fmps_cxt_open_t5577_suite_page,
        app,
        fmps_cxt_open_rf_scope_page,
        app,
        fmps_cxt_open_antenna_tune_page,
        app,
        fmps_cxt_open_pm5_control_page,
        app,
        fmps_cxt_open_status_page,
        app);
    view_set_previous_callback(
        submenu_get_view(app->operate_page->submenu), operate_page_previous_callback);
    view_dispatcher_add_view(
        app->view_dispatcher, OperatePageViewId, submenu_get_view(app->operate_page->submenu));

    app->read_hitag2_page = read_hitag2_page_create(fmps_cxt_open_operate_page, app);
    view_set_previous_callback(
        read_hitag2_page_get_view(app->read_hitag2_page), read_hitag2_page_previous_callback);
    view_dispatcher_add_view(
        app->view_dispatcher,
        ReadHitag2PageViewId,
        read_hitag2_page_get_view(app->read_hitag2_page));

    app->read_hf14a_page = read_hf14a_page_create(fmps_cxt_open_operate_page, app);
    view_set_previous_callback(
        read_hf14a_page_get_view(app->read_hf14a_page), read_hf14a_page_previous_callback);
    view_dispatcher_add_view(
        app->view_dispatcher,
        ReadHf14aPageViewId,
        read_hf14a_page_get_view(app->read_hf14a_page));

    app->hf_mf_audit_page = hf_mf_audit_page_create(fmps_cxt_open_operate_page, app);
    view_set_previous_callback(
        hf_mf_audit_page_get_view(app->hf_mf_audit_page), hf_mf_audit_page_previous_callback);
    view_dispatcher_add_view(
        app->view_dispatcher,
        HfMfAuditPageViewId,
        hf_mf_audit_page_get_view(app->hf_mf_audit_page));

    app->hf_sniff_page = hf_sniff_page_create(fmps_cxt_open_operate_page, app);
    view_set_previous_callback(
        hf_sniff_page_get_view(app->hf_sniff_page), hf_sniff_page_previous_callback);
    view_dispatcher_add_view(
        app->view_dispatcher,
        HfSniffPageViewId,
        hf_sniff_page_get_view(app->hf_sniff_page));

    app->read_hf_multi_page = read_hf_multi_page_create(fmps_cxt_open_operate_page, app);
    view_set_previous_callback(
        read_hf_multi_page_get_view(app->read_hf_multi_page),
        read_hf_multi_page_previous_callback);
    view_dispatcher_add_view(
        app->view_dispatcher,
        ReadHfMultiPageViewId,
        read_hf_multi_page_get_view(app->read_hf_multi_page));

    app->lf_lab_page = lf_lab_page_create(fmps_cxt_open_operate_page, app);
    view_set_previous_callback(
        lf_lab_page_get_view(app->lf_lab_page), lf_lab_page_previous_callback);
    view_dispatcher_add_view(
        app->view_dispatcher,
        LfLabPageViewId,
        lf_lab_page_get_view(app->lf_lab_page));

    app->t5577_suite_page = t5577_suite_page_create(fmps_cxt_open_operate_page, app);
    view_set_previous_callback(
        t5577_suite_page_get_view(app->t5577_suite_page), t5577_suite_page_previous_callback);
    view_dispatcher_add_view(
        app->view_dispatcher,
        T5577SuitePageViewId,
        t5577_suite_page_get_view(app->t5577_suite_page));

    app->rf_scope_page = rf_scope_page_create(fmps_cxt_open_operate_page, app);
    view_set_previous_callback(
        rf_scope_page_get_view(app->rf_scope_page), rf_scope_page_previous_callback);
    view_dispatcher_add_view(
        app->view_dispatcher,
        RfScopePageViewId,
        rf_scope_page_get_view(app->rf_scope_page));

    app->antenna_tune_page = antenna_tune_page_create(fmps_cxt_open_operate_page, app);
    view_set_previous_callback(
        antenna_tune_page_get_view(app->antenna_tune_page), antenna_tune_page_previous_callback);
    view_dispatcher_add_view(
        app->view_dispatcher,
        AntennaTunePageViewId,
        antenna_tune_page_get_view(app->antenna_tune_page));

    app->pm5_control_page = pm5_control_page_create(fmps_cxt_open_operate_page, app);
    view_set_previous_callback(
        pm5_control_page_get_view(app->pm5_control_page), pm5_control_page_previous_callback);
    view_dispatcher_add_view(
        app->view_dispatcher,
        Pm5ControlPageViewId,
        pm5_control_page_get_view(app->pm5_control_page));

    app->status_page = status_page_create();
    view_set_previous_callback(
        status_page_get_view(app->status_page), status_page_previous_callback);
    view_dispatcher_add_view(
        app->view_dispatcher, StatusPageViewId, status_page_get_view(app->status_page));

    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, fmps_cxt_back_event_callback);
    view_dispatcher_set_custom_event_callback(
        app->view_dispatcher, fmps_cxt_custom_event_callback);
    return app;
}

void proxmark5_app_free(Proxmark5App* app) {
    furi_assert(app);

    view_dispatcher_remove_view(app->view_dispatcher, MainPageViewId);
    view_dispatcher_remove_view(app->view_dispatcher, OperatePageViewId);
    view_dispatcher_remove_view(app->view_dispatcher, ReadHitag2PageViewId);
    view_dispatcher_remove_view(app->view_dispatcher, ReadHf14aPageViewId);
    view_dispatcher_remove_view(app->view_dispatcher, HfMfAuditPageViewId);
    view_dispatcher_remove_view(app->view_dispatcher, HfSniffPageViewId);
    view_dispatcher_remove_view(app->view_dispatcher, ReadHfMultiPageViewId);
    view_dispatcher_remove_view(app->view_dispatcher, LfLabPageViewId);
    view_dispatcher_remove_view(app->view_dispatcher, T5577SuitePageViewId);
    view_dispatcher_remove_view(app->view_dispatcher, RfScopePageViewId);
    view_dispatcher_remove_view(app->view_dispatcher, AntennaTunePageViewId);
    view_dispatcher_remove_view(app->view_dispatcher, Pm5ControlPageViewId);
    view_dispatcher_remove_view(app->view_dispatcher, StatusPageViewId);

    if(app->main_page) {
        main_page_free(app->main_page);
        app->main_page = NULL;
    }
    if(app->operate_page) {
        operate_page_free(app->operate_page);
        app->operate_page = NULL;
    }
    if(app->read_hitag2_page) {
        read_hitag2_page_free(app->read_hitag2_page);
        app->read_hitag2_page = NULL;
    }
    if(app->read_hf14a_page) {
        read_hf14a_page_free(app->read_hf14a_page);
        app->read_hf14a_page = NULL;
    }
    if(app->hf_mf_audit_page) {
        hf_mf_audit_page_free(app->hf_mf_audit_page);
        app->hf_mf_audit_page = NULL;
    }
    if(app->hf_sniff_page) {
        hf_sniff_page_free(app->hf_sniff_page);
        app->hf_sniff_page = NULL;
    }
    if(app->read_hf_multi_page) {
        read_hf_multi_page_free(app->read_hf_multi_page);
        app->read_hf_multi_page = NULL;
    }
    if(app->lf_lab_page) {
        lf_lab_page_free(app->lf_lab_page);
        app->lf_lab_page = NULL;
    }
    if(app->t5577_suite_page) {
        t5577_suite_page_destroy(app->t5577_suite_page);
        app->t5577_suite_page = NULL;
    }
    if(app->rf_scope_page) {
        rf_scope_page_free(app->rf_scope_page);
        app->rf_scope_page = NULL;
    }
    if(app->antenna_tune_page) {
        antenna_tune_page_free(app->antenna_tune_page);
        app->antenna_tune_page = NULL;
    }
    if(app->pm5_control_page) {
        pm5_control_page_free(app->pm5_control_page);
        app->pm5_control_page = NULL;
    }
    if(app->status_page) {
        status_page_free(app->status_page);
        app->status_page = NULL;
    }

    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);
    free(app);
}

int32_t fmps_cxt_app(void* p) {
    UNUSED(p);

    proxmark5_com_init();

    Proxmark5App* app = proxmark5_app_alloc();
    view_dispatcher_switch_to_view(app->view_dispatcher, MainPageViewId);
    view_dispatcher_run(app->view_dispatcher);
    proxmark5_app_free(app);

    proxmark5_com_deinit();

    return 0;
}
