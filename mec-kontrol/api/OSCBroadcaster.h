#pragma once

#include "KontrolModel.h"


#include <memory>
#include <ip/UdpSocket.h>

namespace Kontrol {

class OSCBroadcaster : public KontrolCallback {
public:
    static const unsigned int OUTPUT_BUFFER_SIZE = 1024;
    static const std::string ADDRESS;

    OSCBroadcaster();
    ~OSCBroadcaster();
    bool connect(const std::string& host = ADDRESS, unsigned port = 9001);
    void stop();

    void requestMetaData();
    void requestConnect(unsigned port);

    // KontrolCallback
    virtual void rack(ParameterSource, const Rack&);
    virtual void module(ParameterSource, const Rack& rack, const Module&);
    virtual void page(ParameterSource src, const Rack& rack, const Module& module, const Page& p);
    virtual void param(ParameterSource src, const Rack& rack, const Module& module, const Parameter&);
    virtual void changed(ParameterSource src, const Rack& rack, const Module& module, const Parameter& p);
private:
    unsigned int port_;
    std::shared_ptr<UdpTransmitSocket> socket_;
    char buffer_[OUTPUT_BUFFER_SIZE];
};

} //namespace