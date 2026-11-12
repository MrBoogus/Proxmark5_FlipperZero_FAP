#ifndef LF_LAB_PAGE_H
#define LF_LAB_PAGE_H

#include <gui/view.h>

typedef struct LfLabPage LfLabPage;
typedef void (*LfLabPageBackCallback)(void* context);

LfLabPage* lf_lab_page_create(LfLabPageBackCallback back_callback, void* back_callback_context);
void lf_lab_page_free(LfLabPage* page);
View* lf_lab_page_get_view(LfLabPage* page);
void lf_lab_page_start(LfLabPage* page);
void lf_lab_page_stop(LfLabPage* page);

#endif // LF_LAB_PAGE_H
