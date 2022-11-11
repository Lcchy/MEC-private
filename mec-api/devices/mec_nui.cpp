#include "mec_nui.h"

#include <algorithm>
#include <unistd.h>
#include <unordered_set>

#include <ip/UdpSocket.h>
#include <osc/OscOutboundPacketStream.h>
#include <osc/OscPacketListener.h>
#include <osc/OscReceivedElements.h>
#include <readerwriterqueue.h>

#include <mec_log.h>

#include "nui/nui_basemode.h"
#include "nui/nui_menu.h"
#include "nui/nui_param_1.h"
#include "nui/nui_param_2.h"

namespace mec
{

    const unsigned SCREEN_HEIGHT = 64;
    const unsigned SCREEN_WIDTH = 128;

    static const unsigned NUI_NUM_TEXTLINES = 5;
    // static const unsigned NUI_NUM_TEXTCHARS = (128 / 4); = 32
    static const unsigned NUI_NUM_TEXTCHARS = 30;

    static const unsigned LINE_H = 10;

    Nui::~Nui() { deinit(); }

    bool Nui::init(void *arg)
    {
        Preferences prefs(arg);

        static const auto MENU_TIMEOUT = 2000;
        static const auto POLL_FREQ = 1;
        static const auto POLL_SLEEP = 1000;
        menuTimeout_ = prefs.getInt("menu timeout", MENU_TIMEOUT);
        std::string res =
            prefs.getString("resource path", "/home/we/norns/resources");
        std::string splash = prefs.getString("splash", "./oracsplash4.png");
        device_ = std::make_shared<NuiLite::NuiDevice>(res.c_str());
        pollFreq_ = prefs.getInt("poll freq", POLL_FREQ);
        pollSleep_ = prefs.getInt("poll sleep", POLL_SLEEP);
        pollCount_ = 0;
        if (!device_)
            return false;

        unsigned parammode = prefs.getInt("param display", 0);

        std::shared_ptr<NuiLite::NuiCallback> cb =
            std::make_shared<NuiDeviceCallback>(*this);
        device_->addCallback(cb);
        device_->start();

        if (active_)
        {
            LOG_2("Nui::init - already active deinit");
            deinit();
        }
        active_ = false;
        listenRunning_ = false;
        unsigned listenPort = prefs.getInt("listen port", 6100);

        auxActive_ = false;
        auxLed_ = 0;
        auxLine_ = "";

        active_ = true;
        if (active_)
        {
            if (parammode == 1 || device_->numEncoders() == 3)
            {
                addMode(NM_PARAMETER, std::make_shared<NuiParamMode2>(*this));
            }
            else
            {
                addMode(NM_PARAMETER, std::make_shared<NuiParamMode1>(*this));
            }

            addMode(NM_MAINMENU, std::make_shared<NuiMainMenu>(*this));
            addMode(NM_PRESETMENU, std::make_shared<NuiPresetMenu>(*this));
            addMode(NM_MODULEMENU, std::make_shared<NuiModuleMenu>(*this));
            //        addMode(NM_MODULESELECTMENU,
            //        std::make_shared<NuiModuleSelectMenu>(*this));

            changeMode(NM_PARAMETER);

            listen(listenPort);
        }
        device_->drawPNG(0, 0, splash.c_str());
        device_->displayText(15, 0, 1, "Connecting...");
        return active_;
    }

    void Nui::deinit()
    {
        listenRunning_ = false;

        if (readSocket_)
        {
            readSocket_->AsynchronousBreak();
            receive_thread_.join();
            OscMsg msg;
            while (readMessageQueue_.try_dequeue(msg))
                ;
        }
        listenPort_ = 0;
        readSocket_.reset();

        device_->stop();
        device_ = nullptr;
        active_ = false;
        return;
    }

    bool Nui::isActive() { return active_; }

    void Nui::stop() { deinit(); }

    //--modes and forwarding
    void Nui::addMode(NuiModes mode, std::shared_ptr<NuiMode> m)
    {
        modes_[mode] = m;
    }

    void Nui::changeMode(NuiModes mode)
    {
        currentMode_ = mode;
        auto m = modes_[mode];
        m->activate();
    }

    void Nui::rack(Kontrol::ChangeSource src, const Kontrol::Rack &rack)
    {
        modes_[currentMode_]->rack(src, rack);
    }

    void Nui::module(Kontrol::ChangeSource src, const Kontrol::Rack &rack,
                     const Kontrol::Module &module)
    {
        if (currentModuleId_.empty())
        {
            currentRackId_ = rack.id();
            currentModule(module.id());
        }

        modes_[currentMode_]->module(src, rack, module);
    }

    void Nui::page(Kontrol::ChangeSource src, const Kontrol::Rack &rack,
                   const Kontrol::Module &module, const Kontrol::Page &page)
    {
        modes_[currentMode_]->page(src, rack, module, page);
    }

    void Nui::param(Kontrol::ChangeSource src, const Kontrol::Rack &rack,
                    const Kontrol::Module &module,
                    const Kontrol::Parameter &param)
    {
        modes_[currentMode_]->param(src, rack, module, param);
    }

    void Nui::changed(Kontrol::ChangeSource src, const Kontrol::Rack &rack,
                      const Kontrol::Module &module,
                      const Kontrol::Parameter &param)
    {
        modes_[currentMode_]->changed(src, rack, module, param);
    }

    void Nui::resource(Kontrol::ChangeSource src, const Kontrol::Rack &rack,
                       const std::string &res, const std::string &value)
    {
        modes_[currentMode_]->resource(src, rack, res, value);

        if (res == "moduleorder")
        {
            moduleOrder_.clear();
            if (value.length() > 0)
            {
                int lidx = 0;
                int idx = 0;
                int len = 0;
                while ((idx = value.find(" ", lidx)) != std::string::npos)
                {
                    len = idx - lidx;
                    std::string mid = value.substr(lidx, len);
                    moduleOrder_.push_back(mid);
                    lidx = idx + 1;
                }
                len = value.length() - lidx;
                if (len > 0)
                {
                    std::string mid = value.substr(lidx, len);
                    moduleOrder_.push_back(mid);
                }
            }
        }
    }

    void Nui::deleteRack(Kontrol::ChangeSource src, const Kontrol::Rack &rack)
    {
        modes_[currentMode_]->deleteRack(src, rack);
    }

    void Nui::activeModule(Kontrol::ChangeSource src, const Kontrol::Rack &rack,
                           const Kontrol::Module &module)
    {
        modes_[currentMode_]->activeModule(src, rack, module);
    }

    void Nui::loadModule(Kontrol::ChangeSource src, const Kontrol::Rack &rack,
                         const Kontrol::EntityId &modId,
                         const std::string &modType)
    {
        modes_[currentMode_]->loadModule(src, rack, modId, modType);
    }

    std::vector<std::shared_ptr<Kontrol::Module>>
    Nui::getModules(const std::shared_ptr<Kontrol::Rack> &pRack)
    {
        std::vector<std::shared_ptr<Kontrol::Module>> ret;
        auto modulelist = model()->getModules(pRack);
        std::unordered_set<std::string> done;

        for (auto mid : moduleOrder_)
        {
            auto pModule = model()->getModule(pRack, mid);
            if (pModule != nullptr)
            {
                ret.push_back(pModule);
                done.insert(mid);
            }
        }
        for (auto pModule : modulelist)
        {
            if (done.find(pModule->id()) == done.end())
            {
                ret.push_back(pModule);
            }
        }

        return ret;
    }
    void Nui::nextPage()
    {
        yieldDisplay_ = false;
        modes_[currentMode_]->nextPage();
    }

    void Nui::prevPage()
    {
        yieldDisplay_ = false;
        modes_[currentMode_]->prevPage();
    }

    void Nui::nextModule()
    {
        yieldDisplay_ = false;
        auto rack = model()->getRack(currentRack());
        auto modules = getModules(rack);
        bool found = false;

        for (auto module : modules)
        {
            if (found)
            {
                currentModule(module->id());
                return;
            }
            if (module->id() == currentModule())
            {
                found = true;
            }
        }
    }

    void Nui::prevModule()
    {
        yieldDisplay_ = false;
        auto rack = model()->getRack(currentRack());
        auto modules = getModules(rack);
        Kontrol::EntityId prevId;
        for (auto module : modules)
        {
            if (module->id() == currentModule())
            {
                if (!prevId.empty())
                {
                    currentModule(prevId);
                    return;
                }
            }
            prevId = module->id();
        }
    }

    void Nui::midiLearn(Kontrol::ChangeSource src, bool b)
    {
        if (b)
            modulationLearnActive_ = false;
        midiLearnActive_ = b;
        modes_[currentMode_]->midiLearn(src, b);
    }

    void Nui::modulationLearn(Kontrol::ChangeSource src, bool b)
    {
        if (b)
            midiLearnActive_ = false;
        modulationLearnActive_ = b;
        modes_[currentMode_]->modulationLearn(src, b);
    }

    void Nui::savePreset(Kontrol::ChangeSource source, const Kontrol::Rack &rack,
                         std::string preset)
    {
        modes_[currentMode_]->savePreset(source, rack, preset);
    }

    void Nui::loadPreset(Kontrol::ChangeSource source, const Kontrol::Rack &rack,
                         std::string preset)
    {
        modes_[currentMode_]->loadPreset(source, rack, preset);
    }

    void Nui::onButton(unsigned id, unsigned value)
    {
        modes_[currentMode_]->onButton(id, value);
    }

    void Nui::onEncoder(unsigned id, int value)
    {
        modes_[currentMode_]->onEncoder(id, value);
    }

    void Nui::midiLearn(bool b) { model()->midiLearn(Kontrol::CS_LOCAL, b); }

    void Nui::modulationLearn(bool b)
    {
        model()->modulationLearn(Kontrol::CS_LOCAL, b);
    }

    //--- display functions

    void Nui::displayPopup(const std::string &text, bool)
    {
        // not used currently
    }

    void Nui::clearDisplay()
    {
        if (!device_)
            return;
        device_->displayClear();
    }

    void Nui::clearParamNum(unsigned num)
    {
        if (!device_)
            return;

        unsigned row, col;
        switch (num)
        {
        case 0:
            row = 0, col = 0;
            break;
        case 1:
            row = 0, col = 1;
            break;
        case 2:
            row = 1, col = 0;
            break;
        case 3:
            row = 1, col = 1;
            break;
        default:
            return;
        }
        unsigned x = col * 64;
        unsigned y1 = (row + 1) * 20;
        unsigned y2 = y1 + LINE_H;
        device_->clearRect(0, x, y1, 62 + (col * 2), LINE_H);
        device_->clearRect(0, x, y2, 62 + (col * 2), LINE_H);
    }

    void Nui::displayParamNum(unsigned num, const Kontrol::Parameter &param,
                              bool dispCtrl, bool selected)
    {
        if (!device_)
            return;
        const std::string &dName = param.displayName();
        std::string value = param.displayValue();
        std::string unit = param.displayUnit();

        unsigned row, col;
        switch (num)
        {
        case 0:
            row = 0, col = 0;
            break;
        case 1:
            row = 0, col = 1;
            break;
        case 2:
            row = 1, col = 0;
            break;
        case 3:
            row = 1, col = 1;
            break;
        default:
            return;
        }

        unsigned x = col * 64;
        unsigned y1 = (row + 1) * 20;
        unsigned y2 = y1 + LINE_H;
        unsigned clr = selected ? 15 : 0;
        device_->clearRect(5, x, y1 - LINE_H, 62 + (col * 2), LINE_H);
        device_->drawText(clr, x + 1, y1 - 1, dName.c_str());
        device_->clearRect(0, x, y2 - LINE_H, 62 + (col * 2), LINE_H);
        device_->drawText(15, x + 1, y2 - 1, value);
        device_->drawText(15, x + 1 + 40, y2 - 1, unit);
    }

    void Nui::displayLine(unsigned line, const char *disp)
    {
        if (!device_)
            return;
        device_->clearText(0, line);
        device_->displayText(15, line, 0, disp);
    }

    void Nui::invertLine(unsigned line)
    {
        if (!device_)
            return;
        device_->invertText(line);
    }

    void Nui::displayTitle(const std::string &module, const std::string &page)
    {
        if (!device_)
            return;
        if (module.size() == 0 || page.size() == 0)
            return;
        std::string title = module + " > " + page;

        device_->clearRect(1, 0, 0, 128, LINE_H);
        device_->drawText(15, 0, 8, title.c_str());
    }

    void Nui::displayStatusBar()
    {
        if (!device_)
            return;
        device_->clearRect(0, 1, 60, 128, LINE_H);

        std::string status = "";
        if (auxActive_)
        {
            status += "+";
        }
        else
        {
            status += "-";
        }

        status += " |";
        status += std::to_string(auxLed_);
        status += "| ";
        status += auxLine_;

        device_->drawText(15, 1, 60, status);
    }

    void Nui::currentModule(const Kontrol::EntityId &modId)
    {
        yieldDisplay_ = false;
        currentModuleId_ = modId;
        model()->activeModule(Kontrol::CS_LOCAL, currentRackId_, currentModuleId_);
    }

    void Nui::setAux(bool b) { auxActive_ = b; }

    void Nui::setAuxLed(int i) { auxLed_ = i; }

    void Nui::setAuxLine(std::string s) { auxLine_ = s; }

    void Nui::setYieldDisplay(bool b) { yieldDisplay_ = b; }

    bool Nui::getYieldDisplay() { return yieldDisplay_; }

    /////////////////////////////////////////////////

    // OSC

    void *nui_osc_read_thread_func(void *pReceiver)
    {
        Nui *pThis = static_cast<Nui *>(pReceiver);
        pThis->readSocket()->Run();
        return nullptr;
    }

    bool Nui::listen(unsigned port)
    {

        if (readSocket_)
        {
            listenRunning_ = false;
            readSocket_->AsynchronousBreak();
            receive_thread_.join();
            OscMsg msg;
            while (readMessageQueue_.try_dequeue(msg))
                ;
        }
        listenPort_ = 0;
        readSocket_.reset();

        listenPort_ = port;
        try
        {
            LOG_0("listening for clients on " << port);
            readSocket_ = std::make_shared<UdpListeningReceiveSocket>(
                IpEndpointName(IpEndpointName::ANY_ADDRESS, listenPort_),
                packetListener_.get());

            listenRunning_ = true;
            receive_thread_ = std::thread(nui_osc_read_thread_func, this);
        }
        catch (const std::runtime_error &e)
        {
            listenPort_ = 0;
            return false;
        }
        return true;
    }

    class NuiPacketListener : public PacketListener
    {
    public:
        NuiPacketListener(moodycamel::ReaderWriterQueue<Nui::OscMsg> &queue)
            : queue_(queue) {}

        virtual void ProcessPacket(const char *data, int size,
                                   const IpEndpointName &remoteEndpoint)
        {

            Nui::OscMsg msg;
            //        msg.origin_ = remoteEndpoint;
            msg.size_ = (size > Nui::OscMsg::MAX_OSC_MESSAGE_SIZE
                             ? Nui::OscMsg::MAX_OSC_MESSAGE_SIZE
                             : size);
            memcpy(msg.buffer_, data, (size_t)msg.size_);
            msg.origin_ = remoteEndpoint;
            queue_.enqueue(msg);
        }

    private:
        moodycamel::ReaderWriterQueue<Nui::OscMsg> &queue_;
    };

    // Osc implmentation
    class NuiListener : public osc::OscPacketListener
    {
    public:
        NuiListener(Nui &recv) : receiver_(recv) { ; }

        virtual void ProcessMessage(const osc::ReceivedMessage &m,
                                    const IpEndpointName &remoteEndpoint)
        {
            try
            {
                char host[IpEndpointName::ADDRESS_STRING_LENGTH];
                remoteEndpoint.AddressAsString(host);
                if (std::strcmp(m.AddressPattern(), "/NextPage") == 0)
                {
                    osc::ReceivedMessage::const_iterator arg = m.ArgumentsBegin();
                    if (isArgFalse(arg))
                        return;
                    receiver_.nextPage();
                }
                else if (std::strcmp(m.AddressPattern(), "/PrevPage") == 0)
                {
                    osc::ReceivedMessage::const_iterator arg = m.ArgumentsBegin();
                    if (isArgFalse(arg))
                        return;
                    receiver_.prevPage();
                }
                else if (std::strcmp(m.AddressPattern(), "/NextModule") == 0)
                {
                    osc::ReceivedMessage::const_iterator arg = m.ArgumentsBegin();
                    if (isArgFalse(arg))
                        return;
                    receiver_.nextModule();
                }
                else if (std::strcmp(m.AddressPattern(), "/PrevModule") == 0)
                {
                    osc::ReceivedMessage::const_iterator arg = m.ArgumentsBegin();
                    if (isArgFalse(arg))
                        return;
                    receiver_.prevModule();
                }
                else if (std::strcmp(m.AddressPattern(), "/Param1") == 0)
                {
                    osc::ReceivedMessage::const_iterator arg = m.ArgumentsBegin();
                    float val = 0;
                    if (arg->IsFloat())
                        val = arg->AsFloat();
                    else if (arg->IsInt32())
                        val = arg->AsInt32();
                    receiver_.modes_[receiver_.currentMode_]->changeParam(0, val, 8000.f);
                }
                else if (std::strcmp(m.AddressPattern(), "/Param2") == 0)
                {
                    osc::ReceivedMessage::const_iterator arg = m.ArgumentsBegin();
                    float val = 0;
                    if (arg->IsFloat())
                        val = arg->AsFloat();
                    else if (arg->IsInt32())
                        val = arg->AsInt32();
                    receiver_.modes_[receiver_.currentMode_]->changeParam(1, val, 8000.f);
                }
                else if (std::strcmp(m.AddressPattern(), "/Param3") == 0)
                {
                    osc::ReceivedMessage::const_iterator arg = m.ArgumentsBegin();
                    float val = 0;
                    if (arg->IsFloat())
                        val = arg->AsFloat();
                    else if (arg->IsInt32())
                        val = arg->AsInt32();
                    receiver_.modes_[receiver_.currentMode_]->changeParam(2, val, 8000.f);
                }
                else if (std::strcmp(m.AddressPattern(), "/Param4") == 0)
                {
                    osc::ReceivedMessage::const_iterator arg = m.ArgumentsBegin();
                    float val = 0;
                    if (arg->IsFloat())
                        val = arg->AsFloat();
                    else if (arg->IsInt32())
                        val = arg->AsInt32();
                    receiver_.modes_[receiver_.currentMode_]->changeParam(3, val, 8000.f);
                }
                else if (std::strcmp(m.AddressPattern(), "/AuxAct") == 0)
                {
                    osc::ReceivedMessage::const_iterator arg = m.ArgumentsBegin();
                    int val = arg->AsInt32();
                    if (val == 1)
                    {
                        receiver_.setAux(true);
                    }
                    else
                    {
                        receiver_.setAux(false);
                    }
                    receiver_.modes_[receiver_.currentMode_]->display();
                }
                else if (std::strcmp(m.AddressPattern(), "/AuxLed") == 0)
                {
                    osc::ReceivedMessage::const_iterator arg = m.ArgumentsBegin();
                    int val = arg->AsInt32();
                    receiver_.setAuxLed(val);
                    receiver_.modes_[receiver_.currentMode_]->display();
                }
                else if (std::strcmp(m.AddressPattern(), "/AuxLine") == 0)
                {
                    osc::ReceivedMessage::const_iterator arg = m.ArgumentsBegin();
                    std::string val = arg->AsString();
                    ++arg;
                    std::string whitespace = " ";
                    while (arg->IsString())
                    {
                        val += whitespace + arg->AsString();
                        ++arg;
                    }
                    receiver_.setAuxLine(val);
                    receiver_.modes_[receiver_.currentMode_]->display();
                }
                else if (std::strcmp(m.AddressPattern(), "/YieldDisplay") == 0)
                {
                    osc::ReceivedMessage::const_iterator arg = m.ArgumentsBegin();
                    receiver_.setYieldDisplay(!isArgFalse(arg));
                }
                else if (std::strcmp(m.AddressPattern(), "/monome/enc/delta") == 0)
                {
                    osc::ReceivedMessage::const_iterator arg = m.ArgumentsBegin();
                    int enc_nb = arg->AsInt32();
                    ++arg;
                    int rot = arg->AsInt32();
                    int rot_sign = (0 < rot) - (rot < 0);
                    float delta = float(rot) / 3.f;

                    receiver_.modes_[receiver_.currentMode_]->changeParam(enc_nb, rot_sign * delta * delta, 2048.f); // Arc res is 1024 per rot
                }
            }
            catch (osc::Exception &e)
            {
                LOG_0("display osc message exception " << m.AddressPattern() << " : "
                                                       << e.what());
            }
        }

    private:
        bool isArgFalse(osc::ReceivedMessage::const_iterator arg)
        {
            if ((arg->IsFloat() && (arg->AsFloat() >= 0.5)) ||
                (arg->IsInt32() && (arg->AsInt32() > 0)))
                return false;
            return true;
        }
        Nui &receiver_;
    };

    Nui::Nui()
        : readMessageQueue_(OscMsg::MAX_N_OSC_MSGS), active_(false),
          listenRunning_(false), modulationLearnActive_(false),
          midiLearnActive_(false)
    {
        packetListener_ = std::make_shared<NuiPacketListener>(readMessageQueue_);
        oscListener_ = std::make_shared<NuiListener>(*this);
    }

    // Kontrol::KontrolCallback
    bool Nui::process()
    {
        OscMsg msg;
        while (readMessageQueue_.try_dequeue(msg))
        {
            oscListener_->ProcessPacket(msg.buffer_, msg.size_, msg.origin_);
        }

        pollCount_++;
        if ((pollCount_ % pollFreq_) == 0)
        {
            modes_[currentMode_]->poll();
            if (device_)
                device_->process();
        }

        if (pollSleep_)
        {
            usleep(pollSleep_);
        }
        return true;
    }

} // namespace mec
