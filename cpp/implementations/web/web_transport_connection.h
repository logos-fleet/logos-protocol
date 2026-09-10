#ifndef LOGOS_WEB_TRANSPORT_CONNECTION_H
#define LOGOS_WEB_TRANSPORT_CONNECTION_H

#include "logos_transport.h"

#include "incoming_call_handler.h"
#include "message_channel.h"

#include <memory>

namespace logos::web {

class WebRpcConnection;

// -----------------------------------------------------------------------------
// WebTransportConnection — the consumer side of the web transport.
//
// connectToHost() has nothing to dial: the channel was handed in at
// construction (from a webview bridge, or from the process-wide factory when
// the transport factory resolved `{"protocol":"web"}`), so "connecting" is
// starting the RPC peer on it. A null or closed channel is reported as a
// failure to connect rather than papered over — a process with no bridge in it
// genuinely cannot reach a Web module.
//
// requestObject() hands back a PlainLogosObject over this peer, the same handle
// the TCP transport returns. That is not reuse for its own sake: the handle
// owns the deferred-completion rendezvous, the per-call deadlines and the
// exactly-once delivery, none of which is about sockets, and a second copy of
// it would be a second set of those bugs.
//
// IT ALSO SERVES, when it is given a handler. A page is not only something a
// host calls: it calls back — capability_module for a token, another module's
// method, a subscription to a native module's event — and all of that is
// inbound traffic on the CHANNEL THE HOST ALREADY HOLDS. A second connection
// cannot be laid over that channel to carry it, because each one installs the
// channel's single receiver and the second would silently take every message
// from the first.
//
// So the door is a second constructor, not a second class. The conversation
// underneath was always full duplex (RpcPeer serves whatever handler it was
// given while its own calls are in flight, and the browser SDK's WebPeer is one
// object for both roles); only this entry point refused to say so, hard-wiring
// a null handler. `handler` is borrowed and must outlive the connection.
//
// TWO CONSTRUCTORS RATHER THAN ONE DEFAULTED ARGUMENT, so the one-argument
// symbol survives verbatim: a default argument would re-sign the existing
// constructor, and the archive this class ships in is linked by consumers that
// were compiled against the old one.
// -----------------------------------------------------------------------------
class WebTransportConnection : public LogosTransportConnection {
public:
    explicit WebTransportConnection(MessageChannelPtr channel);
    WebTransportConnection(MessageChannelPtr channel,
                           logos::plain::IncomingCallHandler* handler);
    ~WebTransportConnection() override;

    bool connectToHost() override;
    bool isConnected() const override;
    bool reconnect() override;
    LogosObject* requestObject(const QString& objectName, int timeoutMs) override;
    QString endpointUrl(const QString& instanceId,
                        const QString& moduleName) override;

private:
    MessageChannelPtr                  m_channel;
    logos::plain::IncomingCallHandler* m_handler = nullptr;
    std::shared_ptr<WebRpcConnection>  m_conn;
    bool                               m_connected = false;
};

} // namespace logos::web

#endif // LOGOS_WEB_TRANSPORT_CONNECTION_H
