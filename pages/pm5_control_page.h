#ifndef PM5_CONTROL_PAGE_H
#define PM5_CONTROL_PAGE_H

#include <gui/view.h>

typedef struct Pm5ControlPage Pm5ControlPage;
typedef void (*Pm5ControlPageBackCallback)(void* context);

Pm5ControlPage*
    pm5_control_page_create(Pm5ControlPageBackCallback back_callback, void* back_callback_context);
void pm5_control_page_free(Pm5ControlPage* page);
View* pm5_control_page_get_view(Pm5ControlPage* page);
void pm5_control_page_start(Pm5ControlPage* page);
void pm5_control_page_stop(Pm5ControlPage* page);

#endif // PM5_CONTROL_PAGE_H
