#ifndef LOGOS_WEB_TRANSPORT_CONNECTION_H
#define LOGOS_WEB_TRANSPORT_CONNECTION_H

#include "logos_transport.h"

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
// -----------------------------------------------------------------------------
class WebTransportConnection : public LogosTransportConnection {
public:
    explicit WebTransportConnection(MessageChannelPtr channel);
    ~WebTransportConnection() override;

    bool connectToHost() override;
    bool isConnected() const override;
    bool reconnect() override;
    LogosObject* requestObject(const QString& objectName, int timeoutMs) override;
    QString endpointUrl(const QString& instanceId,
                        const QString& moduleName) override;

private:
    MessageChannelPtr                 m_channel;
    std::shared_ptr<WebRpcConnection> m_conn;
    bool                              m_connected = false;
};

} // namespace logos::web

#endif // LOGOS_WEB_TRANSPORT_CONNECTION_H
