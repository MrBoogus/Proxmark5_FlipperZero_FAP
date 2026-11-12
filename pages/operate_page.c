#include <furi.h>
#include "operate_page.h"

typedef enum {
    OperateSubmenuIndexHf14a = 0,
    OperateSubmenuIndexHfMfAudit,
    OperateSubmenuIndexHfSniff,
    OperateSubmenuIndexHfMulti,
    OperateSubmenuIndexT5577Suite,
    OperateSubmenuIndexLfLab,
    OperateSubmenuIndexReadHitag2,
    OperateSubmenuIndexRfScope,
    OperateSubmenuIndexAntennaTune,
    OperateSubmenuIndexPm5Control,
    OperateSubmenuIndexStatus,
} OperateSubmenuIndex;

static void operate_page_submenu_callback(void* context, uint32_t index) {
    OperatePage* operate_page = context;
    if(!operate_page) {
        return;
    }

    switch(index) {
    case OperateSubmenuIndexReadHitag2:
        if(operate_page->open_read_hitag2_callback) {
            operate_page->open_read_hitag2_callback(
                operate_page->open_read_hitag2_callback_context);
        }
        break;
    case OperateSubmenuIndexHf14a:
        if(operate_page->open_hf14a_callback) {
            operate_page->open_hf14a_callback(operate_page->open_hf14a_callback_context);
        }
        break;
    case OperateSubmenuIndexHfMfAudit:
        if(operate_page->open_hf_mf_audit_callback) {
            operate_page->open_hf_mf_audit_callback(
                operate_page->open_hf_mf_audit_callback_context);
        }
        break;
    case OperateSubmenuIndexHfSniff:
        if(operate_page->open_hf_sniff_callback) {
            operate_page->open_hf_sniff_callback(operate_page->open_hf_sniff_callback_context);
        }
        break;
    case OperateSubmenuIndexHfMulti:
        if(operate_page->open_hf_multi_callback) {
            operate_page->open_hf_multi_callback(operate_page->open_hf_multi_callback_context);
        }
        break;
    case OperateSubmenuIndexT5577Suite:
        if(operate_page->open_t5577_suite_callback) {
            operate_page->open_t5577_suite_callback(operate_page->open_t5577_suite_callback_context);
        }
        break;
    case OperateSubmenuIndexLfLab:
        if(operate_page->open_lf_lab_callback) {
            operate_page->open_lf_lab_callback(operate_page->open_lf_lab_callback_context);
        }
        break;
    case OperateSubmenuIndexRfScope:
        if(operate_page->open_rf_scope_callback) {
            operate_page->open_rf_scope_callback(operate_page->open_rf_scope_callback_context);
        }
        break;
    case OperateSubmenuIndexAntennaTune:
        if(operate_page->open_antenna_tune_callback) {
            operate_page->open_antenna_tune_callback(
                operate_page->open_antenna_tune_callback_context);
        }
        break;
    case OperateSubmenuIndexPm5Control:
        if(operate_page->open_pm5_control_callback) {
            operate_page->open_pm5_control_callback(
                operate_page->open_pm5_control_callback_context);
        }
        break;
    case OperateSubmenuIndexStatus:
        if(operate_page->open_status_callback) {
            operate_page->open_status_callback(operate_page->open_status_callback_context);
        }
        break;
    default:
        break;
    }
}

OperatePage* operate_page_create(
    OperatePageCallback open_read_hitag2_callback,
    void* open_read_hitag2_callback_context,
    OperatePageCallback open_hf14a_callback,
    void* open_hf14a_callback_context,
    OperatePageCallback open_hf_mf_audit_callback,
    void* open_hf_mf_audit_callback_context,
    OperatePageCallback open_hf_sniff_callback,
    void* open_hf_sniff_callback_context,
    OperatePageCallback open_hf_multi_callback,
    void* open_hf_multi_callback_context,
    OperatePageCallback open_lf_lab_callback,
    void* open_lf_lab_callback_context,
    OperatePageCallback open_t5577_suite_callback,
    void* open_t5577_suite_callback_context,
    OperatePageCallback open_rf_scope_callback,
    void* open_rf_scope_callback_context,
    OperatePageCallback open_antenna_tune_callback,
    void* open_antenna_tune_callback_context,
    OperatePageCallback open_pm5_control_callback,
    void* open_pm5_control_callback_context,
    OperatePageCallback open_status_callback,
    void* open_status_callback_context) {
    OperatePage* operate_page = calloc(1, sizeof(OperatePage));
    if(!operate_page) {
        return NULL;
    }

    operate_page->open_read_hitag2_callback = open_read_hitag2_callback;
    operate_page->open_read_hitag2_callback_context = open_read_hitag2_callback_context;
    operate_page->open_hf14a_callback = open_hf14a_callback;
    operate_page->open_hf14a_callback_context = open_hf14a_callback_context;
    operate_page->open_hf_mf_audit_callback = open_hf_mf_audit_callback;
    operate_page->open_hf_mf_audit_callback_context = open_hf_mf_audit_callback_context;
    operate_page->open_hf_sniff_callback = open_hf_sniff_callback;
    operate_page->open_hf_sniff_callback_context = open_hf_sniff_callback_context;
    operate_page->open_hf_multi_callback = open_hf_multi_callback;
    operate_page->open_hf_multi_callback_context = open_hf_multi_callback_context;
    operate_page->open_lf_lab_callback = open_lf_lab_callback;
    operate_page->open_lf_lab_callback_context = open_lf_lab_callback_context;
    operate_page->open_t5577_suite_callback = open_t5577_suite_callback;
    operate_page->open_t5577_suite_callback_context = open_t5577_suite_callback_context;
    operate_page->open_rf_scope_callback = open_rf_scope_callback;
    operate_page->open_rf_scope_callback_context = open_rf_scope_callback_context;
    operate_page->open_antenna_tune_callback = open_antenna_tune_callback;
    operate_page->open_antenna_tune_callback_context = open_antenna_tune_callback_context;
    operate_page->open_pm5_control_callback = open_pm5_control_callback;
    operate_page->open_pm5_control_callback_context = open_pm5_control_callback_context;
    operate_page->open_status_callback = open_status_callback;
    operate_page->open_status_callback_context = open_status_callback_context;

    operate_page->submenu = submenu_alloc();
    submenu_set_header(operate_page->submenu, "PM5 Companion v2.0");
    submenu_add_item(
        operate_page->submenu,
        "HF 14a & Magic Lab",
        OperateSubmenuIndexHf14a,
        operate_page_submenu_callback,
        operate_page);
    submenu_add_item(
        operate_page->submenu,
        "MFC Key Auditor (1K)",
        OperateSubmenuIndexHfMfAudit,
        operate_page_submenu_callback,
        operate_page);
    submenu_add_item(
        operate_page->submenu,
        "14a Air Sniffer",
        OperateSubmenuIndexHfSniff,
        operate_page_submenu_callback,
        operate_page);
    submenu_add_item(
        operate_page->submenu,
        "HF 15693 / iCLASS",
        OperateSubmenuIndexHfMulti,
        operate_page_submenu_callback,
        operate_page);
    submenu_add_item(
        operate_page->submenu,
        "T5577 & EM4100 Master",
        OperateSubmenuIndexT5577Suite,
        operate_page_submenu_callback,
        operate_page);
    submenu_add_item(
        operate_page->submenu,
        "LF Multi-Cloner Lab",
        OperateSubmenuIndexLfLab,
        operate_page_submenu_callback,
        operate_page);
    submenu_add_item(
        operate_page->submenu,
        "LF Hitag2 Reader",
        OperateSubmenuIndexReadHitag2,
        operate_page_submenu_callback,
        operate_page);
    submenu_add_item(
        operate_page->submenu,
        "Live RF Field Scope",
        OperateSubmenuIndexRfScope,
        operate_page_submenu_callback,
        operate_page);
    submenu_add_item(
        operate_page->submenu,
        "Antenna Tuner & Graph",
        OperateSubmenuIndexAntennaTune,
        operate_page_submenu_callback,
        operate_page);
    submenu_add_item(
        operate_page->submenu,
        "PM5 Hardware Control",
        OperateSubmenuIndexPm5Control,
        operate_page_submenu_callback,
        operate_page);
    submenu_add_item(
        operate_page->submenu,
        "Hardware Status",
        OperateSubmenuIndexStatus,
        operate_page_submenu_callback,
        operate_page);

    return operate_page;
}

void operate_page_free(OperatePage* operate_page) {
    if(!operate_page) {
        return;
    }

    if(operate_page->submenu) {
        submenu_free(operate_page->submenu);
        operate_page->submenu = NULL;
    }

    free(operate_page);
}
