#ifndef HF_MF_AUDIT_PAGE_H
#define HF_MF_AUDIT_PAGE_H

#include <gui/view.h>

typedef struct HfMfAuditPage HfMfAuditPage;
typedef void (*HfMfAuditPageBackCallback)(void* context);

HfMfAuditPage*
    hf_mf_audit_page_create(HfMfAuditPageBackCallback back_callback, void* back_callback_context);
void hf_mf_audit_page_free(HfMfAuditPage* page);
View* hf_mf_audit_page_get_view(HfMfAuditPage* page);
void hf_mf_audit_page_start(HfMfAuditPage* page);
void hf_mf_audit_page_stop(HfMfAuditPage* page);

#endif // HF_MF_AUDIT_PAGE_H
