#ifndef ANTENNA_TUNE_PAGE_H
#define ANTENNA_TUNE_PAGE_H

#include <gui/view.h>

typedef struct AntennaTunePage AntennaTunePage;
typedef void (*AntennaTunePageBackCallback)(void* context);

AntennaTunePage*
    antenna_tune_page_create(AntennaTunePageBackCallback back_callback, void* back_callback_context);
void antenna_tune_page_free(AntennaTunePage* page);
View* antenna_tune_page_get_view(AntennaTunePage* page);
void antenna_tune_page_start(AntennaTunePage* page);
void antenna_tune_page_stop(AntennaTunePage* page);

#endif // ANTENNA_TUNE_PAGE_H
