#ifndef HF_SNIFF_PAGE_H
#define HF_SNIFF_PAGE_H

#include <gui/view.h>

typedef struct HfSniffPage HfSniffPage;
typedef void (*HfSniffPageBackCallback)(void* context);

HfSniffPage* hf_sniff_page_create(HfSniffPageBackCallback back_callback, void* back_callback_context);
void hf_sniff_page_free(HfSniffPage* page);
View* hf_sniff_page_get_view(HfSniffPage* page);
void hf_sniff_page_start(HfSniffPage* page);
void hf_sniff_page_stop(HfSniffPage* page);

#endif // HF_SNIFF_PAGE_H
