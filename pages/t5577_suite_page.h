#pragma once

#include <gui/view.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct T5577SuitePage T5577SuitePage;
typedef void (*T5577SuitePageBackCallback)(void* context);

T5577SuitePage* t5577_suite_page_create(
    T5577SuitePageBackCallback back_callback,
    void* back_callback_context);

void t5577_suite_page_destroy(T5577SuitePage* page);

View* t5577_suite_page_get_view(T5577SuitePage* page);

#ifdef __cplusplus
}
#endif
