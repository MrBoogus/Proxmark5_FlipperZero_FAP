#ifndef RF_SCOPE_PAGE_H
#define RF_SCOPE_PAGE_H

#include <gui/view.h>

typedef struct RfScopePage RfScopePage;
typedef void (*RfScopePageBackCallback)(void* context);

RfScopePage* rf_scope_page_create(RfScopePageBackCallback back_callback, void* back_callback_context);
void rf_scope_page_free(RfScopePage* page);
View* rf_scope_page_get_view(RfScopePage* page);
void rf_scope_page_start(RfScopePage* page);
void rf_scope_page_stop(RfScopePage* page);

#endif // RF_SCOPE_PAGE_H
