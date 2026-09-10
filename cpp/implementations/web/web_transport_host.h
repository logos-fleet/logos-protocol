#ifndef LOGOS_WEB_TRANSPORT_HOST_H
#define LOGOS_WEB_TRANSPORT_HOST_H

#include "logos_transport.h"

#include "incoming_call_handler.h"
#include "message_channel.h"

#include <QObject>

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace logos::web {

class WebRpcConnection;

// -----------------------------------------------------------------------------
// WebTransportHost — publishes QObjects to peers reached over message channels.
//
// The same shape as PlainTransportHost with the acceptor removed. A TCP host
// LISTENS and gets its connections from an accept loop; a web host is HANDED
// them — attachChannel() is the whole of "a webview appeared", and one host
// serves as many channels as it is given, each its own peer.
//
// That is what makes a Web module's identity structural (ADR 0005): a message
// is attributed to the channel it arrived on, so the connection a call came in
// over is the module that made it, and a token stolen inside one webview cannot
// be used to impersonate another.
//
// Everything below the message set is shared with the TCP transport: inbound
// requests arrive through IncomingCallHandler and are dispatched to the
// published ModuleProxy with a queued invokeMethod, so a provider still runs on
// its own thread and never on a transport thread.
// -----------------------------------------------------------------------------
class WebTransportHost
    : public LogosTransportHost
    , public logos::plain::IncomingCallHandler
{
public:
    WebTransportHost();
    ~WebTransportHost() override;

    // LogosTransportHost
    bool publishObject(const QString& name, QObject* object) override;
    void unpublishObject(const QString& name) override;
    QString bindUrl(const QString& instanceId,
                    const QString& moduleName) override;

    // Serve one more peer over `channel`. Returns false for a null or closed
    // channel. Idempotent per channel is NOT promised: attaching the same
    // channel twice makes two peers fight over one receiver, which is a caller
    // error.
    bool attachChannel(MessageChannelPtr channel);

    // How many connections are subscribed to (object, event) right now. Exists
    // for tests and diagnostics: a subscription crossing a channel is otherwise
    // unobservable from outside, and "did the Subscribe arrive yet" is exactly
    // the question an event test has to answer before it emits.
    std::size_t subscriberCount(const std::string& object,
                                const std::string& eventName) const;

    // IncomingCallHandler
    void onCall(const logos::plain::CallMessage& req, CallReply reply) override;
    void onMethods(const logos::plain::MethodsMessage& req, MethodsReply reply) override;
    void onSubscribe(const logos::plain::SubscribeMessage& req, EventSink sink,
                     const void* connectionId) override;
    void onUnsubscribe(const logos::plain::UnsubscribeMessage& req,
                       const void* connectionId) override;
    void onConnectionClosed(const void* connectionId) override;
    void onToken(const logos::plain::TokenMessage& req) override;

private:
    // Deliver an event emitted by a published QObject to every subscribed
    // connection — ONE copy per connection, as on the plain transport, because
    // a connection subscribed both by name and by wildcard has one sink that
    // does the same thing twice.
    void fanOutEvent(const std::string& name, logos::plain::EventMessage msg);

    struct Published {
        QObject* object = nullptr;
        QMetaObject::Connection eventConn;
    };

    // object name -> event name ("" = wildcard) -> connection -> sink. Keyed
    // independently of m_published for the reason spelled out on the plain
    // host: a consumer subscribes exactly when the object is most likely to be
    // missing, and a subscription recorded against a Published entry would be
    // dropped there with the consumer told nothing.
    using SinkTable = std::map<std::string, std::map<const void*, EventSink>>;

    mutable std::mutex                             m_mu;
    std::map<std::string, Published>               m_published;
    std::map<std::string, SinkTable>               m_sinks;
    std::vector<std::shared_ptr<WebRpcConnection>> m_connections;
};

} // namespace logos::web

#endif // LOGOS_WEB_TRANSPORT_HOST_H
