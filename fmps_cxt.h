#ifndef FMPS_CXT_H
#define FMPS_CXT_H

#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>

// Pages
#include "pages/main_page.h"
#include "pages/operate_page.h"
#include "pages/read_hitag2_page.h"
#include "pages/read_hf14a_page.h"
#include "pages/hf_mf_audit_page.h"
#include "pages/hf_sniff_page.h"
#include "pages/read_hf_multi_page.h"
#include "pages/lf_lab_page.h"
#include "pages/t5577_suite_page.h"
#include "pages/rf_scope_page.h"
#include "pages/antenna_tune_page.h"
#include "pages/pm5_control_page.h"
#include "pages/status_page.h"

typedef enum {
    MainPageViewId,
    OperatePageViewId,
    ReadHitag2PageViewId,
    ReadHf14aPageViewId,
    HfMfAuditPageViewId,
    HfSniffPageViewId,
    ReadHfMultiPageViewId,
    LfLabPageViewId,
    T5577SuitePageViewId,
    RfScopePageViewId,
    AntennaTunePageViewId,
    Pm5ControlPageViewId,
    StatusPageViewId,
} Proxmark5ViewId;

typedef enum {
    Proxmark5CustomEventOpenOperatePage,
    Proxmark5CustomEventOpenReadHitag2Page,
    Proxmark5CustomEventOpenReadHf14aPage,
    Proxmark5CustomEventOpenHfMfAuditPage,
    Proxmark5CustomEventOpenHfSniffPage,
    Proxmark5CustomEventOpenReadHfMultiPage,
    Proxmark5CustomEventOpenLfLabPage,
    Proxmark5CustomEventOpenT5577SuitePage,
    Proxmark5CustomEventOpenRfScopePage,
    Proxmark5CustomEventOpenAntennaTunePage,
    Proxmark5CustomEventOpenPm5ControlPage,
    Proxmark5CustomEventOpenStatusPage,
} Proxmark5CustomEvent;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;

    // Pages
    MainPage* main_page;
    OperatePage* operate_page;
    ReadHitag2Page* read_hitag2_page;
    ReadHf14aPage* read_hf14a_page;
    HfMfAuditPage* hf_mf_audit_page;
    HfSniffPage* hf_sniff_page;
    ReadHfMultiPage* read_hf_multi_page;
    LfLabPage* lf_lab_page;
    T5577SuitePage* t5577_suite_page;
    RfScopePage* rf_scope_page;
    AntennaTunePage* antenna_tune_page;
    Pm5ControlPage* pm5_control_page;
    StatusPage* status_page;
} Proxmark5App;

#endif // FMPS_CXT_H
