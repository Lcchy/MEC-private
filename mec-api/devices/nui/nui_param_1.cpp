#include "nui_param_1.h"

//#include <iostream>
namespace mec {

static const unsigned NUI_NUM_PARAMS = 4;


static unsigned param1EncoderMap[] = {0, 2, 3, 1};

void NuiParamMode1::display() {
    if (parent_.getYieldDisplay()) {
        return;
    }

    parent_.clearDisplay();
    auto rack = parent_.model()->getRack(parent_.currentRack());
    auto module = parent_.model()->getModule(rack, parent_.currentModule());
    auto pages = parent_.model()->getPages(module); //TODO safe?

    std::string md = "";
    std::string pd = "";
    if (module) md = module->id() + " : " + module->displayName();
    if (pages.size() == 0) {
        parent_.displayTitle(md, "none");
    } else {
        auto page = parent_.model()->getPage(module, pageId_);
        if (page) pd = page->displayName();
        auto params = parent_.model()->getParams(module, page);
        parent_.displayTitle(md, pd);
        unsigned int j = 0;
        for (auto param : params) {
            if (param != nullptr) {
                displayParamNum(j, *param, true);
            }
            j++;
            if (j == NUI_NUM_PARAMS) break;
        }

        for (; j < NUI_NUM_PARAMS; j++) {
            parent_.clearParamNum(j);
        }
    }

    parent_.displayStatusBar();
}


void NuiParamMode1::activate() {
    NuiBaseMode::activate();
    display();
}

void NuiParamMode1::onButton(unsigned id, unsigned value) {
    NuiBaseMode::onButton(id, value);
    switch (id) {
        case 0 : {
            if (!value) {
                // on release of button
                parent_.changeMode(NM_MAINMENU);
            }
            break;
        }
        case 1 : {
            break;
        }
        case 2 : {
            break;
        }
        default:;
    }
}

void NuiParamMode1::displayParamNum(unsigned num, const Kontrol::Parameter &p, bool local) {
    if (!parent_.getYieldDisplay()) {
        parent_.displayParamNum(num, p, local, false);
    }
}

void NuiParamMode1::changeParam(unsigned idx, int relValue, float steps) {
    try {
        auto pRack = model()->getRack(parent_.currentRack());
        auto pModule = model()->getModule(pRack, parent_.currentModule());
        auto pPage = model()->getPage(pModule, parent_.currentPage());
        auto pParams = model()->getParams(pModule, pPage);


        if (idx >= pParams.size()) return;
        auto &param = pParams[idx];

        if (param != nullptr) {
            float value = float(relValue) / steps;
            Kontrol::ParamValue calc = param->calcRelative(value);
            //std::cerr << "changeParam " << idx << " " << value << " cv " << calc.floatValue() << " pv " << param->current().floatValue() << std::endl;
            model()->changeParam(Kontrol::CS_LOCAL, parent_.currentRack(), pModule->id(), param->id(), calc);
        }
    } catch (std::out_of_range) {
    }
}

void NuiParamMode1::onEncoder(unsigned idx, int v) {
    NuiBaseMode::onEncoder(idx, v);
    if (idx == 2 && buttonState_[2]) {
        // if holding button 3. then turning encoder 3 changed page
        if (v > 0) {
            nextPage();
        } else {
            prevPage();
        }

    } else if (idx == 1 && buttonState_[2]) {
        // if holding button 3. then turning encoder 2 changed module
        if (v > 0) {
            parent_.setYieldDisplay(false); 
            parent_.nextModule();
        } else {
            parent_.setYieldDisplay(false); 
            parent_.prevModule();
        }
    } else {
        if (idx >= sizeof(idx)) return;
        auto paramIdx = param1EncoderMap[idx];
        changeParam(paramIdx, v);
    }
}


void NuiParamMode1::setCurrentPage(unsigned pageIdx, bool UI) {
    try {
        auto rack = parent_.model()->getRack(parent_.currentRack());
        auto module = parent_.model()->getModule(rack, parent_.currentModule());
        //auto page = parent_.model()->getPage(module, pageId_);
        auto pages = parent_.model()->getPages(module);
//        auto params = parent_.model()->getParams(module,page);

        if (pageIdx_ != pageIdx) {
            pageIdx_ = pageIdx;
            if (pageIdx < pages.size()) {
                pageIdx_ = pageIdx;
                try {
                    auto page = pages[pageIdx_];
                    pageId_ = page->id();
                    parent_.currentPage(pageId_);
                    display();
                } catch (std::out_of_range) { ;
                }
            }
        }

    } catch (std::out_of_range) { ;
    }
}


void NuiParamMode1::activeModule(Kontrol::ChangeSource src, const Kontrol::Rack &rack, const Kontrol::Module &module) {
    if (rack.id() == parent_.currentRack()) {
        if (src != Kontrol::CS_LOCAL && module.id() != parent_.currentModule()) {
            parent_.setYieldDisplay(false); 
            parent_.currentModule(module.id());
        }
        pageIdx_ = -1;
        setCurrentPage(0, false);
    }
}


void NuiParamMode1::changed(Kontrol::ChangeSource src, const Kontrol::Rack &rack, const Kontrol::Module &module,
                            const Kontrol::Parameter &param) {
    if (rack.id() != parent_.currentRack() || module.id() != parent_.currentModule()) return;

    auto prack = parent_.model()->getRack(parent_.currentRack());
    auto pmodule = parent_.model()->getModule(prack, parent_.currentModule());
    auto page = parent_.model()->getPage(pmodule, pageId_);
//    auto pages = parent_.model()->getPages(pmodule);
    auto params = parent_.model()->getParams(pmodule, page);


    unsigned sz = params.size();
    sz = sz < NUI_NUM_PARAMS ? sz : NUI_NUM_PARAMS;
    for (unsigned int i = 0; i < sz; i++) {
        try {
            auto &p = params.at(i);
            if (p->id() == param.id()) {
                p->change(param.current(), src == Kontrol::CS_PRESET);
                displayParamNum(i, param, src != Kontrol::CS_LOCAL);
                return;
            }
        } catch (std::out_of_range) {
            return;
        }
    } // for
}

void NuiParamMode1::module(Kontrol::ChangeSource source, const Kontrol::Rack &rack,
                           const Kontrol::Module &module) {
    if (moduleType_ != module.type()) {
        pageIdx_ = -1;
    }
    moduleType_ = module.type();
}

void NuiParamMode1::page(Kontrol::ChangeSource source, const Kontrol::Rack &rack, const Kontrol::Module &module,
                         const Kontrol::Page &page) {
    if (pageIdx_ < 0) setCurrentPage(0, false);
}


void NuiParamMode1::loadModule(Kontrol::ChangeSource source, const Kontrol::Rack &rack,
                               const Kontrol::EntityId &moduleId, const std::string &modType) {
    if (parent_.currentModule() == moduleId) {
        if (moduleType_ != modType) {
            pageIdx_ = -1;
            moduleType_ = modType;
        }
    }
}

void NuiParamMode1::loadPreset(Kontrol::ChangeSource source, const Kontrol::Rack &rack, std::string preset) {
    pageIdx_ = -1;
    setCurrentPage(0, false);
}

void NuiParamMode1::nextPage() {
    if (pageIdx_ < 0) {
        setCurrentPage(0, false);
        return;
    }

    unsigned pagenum = (unsigned) pageIdx_;

    auto rack = parent_.model()->getRack(parent_.currentRack());
    auto module = parent_.model()->getModule(rack, parent_.currentModule());
//        auto page = parent_.model()->getPage(module,pageId_);
    auto pages = parent_.model()->getPages(module);
//        auto params = parent_.model()->getParams(module,page);
    pagenum++;
    pagenum = std::min(pagenum, (unsigned) pages.size() - 1);

    if (pagenum != pageIdx_) {
        setCurrentPage(pagenum, true);
    }
}

void NuiParamMode1::prevPage() {
    if (pageIdx_ < 0) {
        setCurrentPage(0, false);
        return;
    }
    unsigned pagenum = (unsigned) pageIdx_;
    if (pagenum > 0) pagenum--;

    if (pagenum != pageIdx_) {
        setCurrentPage(pagenum, true);
    }
}


} // mec
