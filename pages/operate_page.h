#ifndef OPERATE_PAGE_H
#define OPERATE_PAGE_H

#include <gui/modules/submenu.h>

typedef void (*OperatePageCallback)(void* context);

typedef struct {
    Submenu* submenu;

    OperatePageCallback open_read_hitag2_callback;
    void* open_read_hitag2_callback_context;
    OperatePageCallback open_hf14a_callback;
    void* open_hf14a_callback_context;
    OperatePageCallback open_hf_mf_audit_callback;
    void* open_hf_mf_audit_callback_context;
    OperatePageCallback open_hf_sniff_callback;
    void* open_hf_sniff_callback_context;
    OperatePageCallback open_hf_multi_callback;
    void* open_hf_multi_callback_context;
    OperatePageCallback open_lf_lab_callback;
    void* open_lf_lab_callback_context;
    OperatePageCallback open_t5577_suite_callback;
    void* open_t5577_suite_callback_context;
    OperatePageCallback open_rf_scope_callback;
    void* open_rf_scope_callback_context;
    OperatePageCallback open_antenna_tune_callback;
    void* open_antenna_tune_callback_context;
    OperatePageCallback open_pm5_control_callback;
    void* open_pm5_control_callback_context;
    OperatePageCallback open_status_callback;
    void* open_status_callback_context;
} OperatePage;

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
    void* open_status_callback_context);
void operate_page_free(OperatePage* operate_page);

#endif // OPERATE_PAGE_H
