#ifndef READ_HF_MULTI_PAGE_H
#define READ_HF_MULTI_PAGE_H

#include <gui/view.h>

typedef struct ReadHfMultiPage ReadHfMultiPage;
typedef void (*ReadHfMultiPageBackCallback)(void* context);

ReadHfMultiPage*
    read_hf_multi_page_create(ReadHfMultiPageBackCallback back_callback, void* back_callback_context);
void read_hf_multi_page_free(ReadHfMultiPage* page);
View* read_hf_multi_page_get_view(ReadHfMultiPage* page);
void read_hf_multi_page_start(ReadHfMultiPage* page);
void read_hf_multi_page_stop(ReadHfMultiPage* page);

#endif // READ_HF_MULTI_PAGE_H
