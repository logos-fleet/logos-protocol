#include "web_rpc_connection.h"

#include "web_message_codec.h"

#include <exception>

namespace logos::web {

using logos::plain::AnyMessage;

WebRpcConnection::WebRpcConnection(MessageChannelPtr channel,
                                   logos::plain::IncomingCallHandler* handler)
    : logos::plain::RpcPeer(handler)
    , m_channel(std::move(channel))
{
}

WebRpcConnection::~WebRpcConnection()
{
    // Detach BEFORE this object's members go away. setReceiver(nullptr) waits
    // for an in-flight delivery (see IMessageChannel), so after it returns no
    // channel thread is still inside dispatchIncoming.
    if (m_channel) m_channel->setReceiver(nullptr);
}

void WebRpcConnection::start()
{
    bool expected = false;
    if (!m_started.compare_exchange_strong(expected, true)) return;
    if (!m_channel) return;
    // weak, so the installed receiver does not keep the connection alive: a
    // channel outliving its peer is ordinary (the host owns the channel, the
    // consumer owns its connection), and a strong capture would make every
    // connection immortal for as long as its channel is.
    std::weak_ptr<logos::plain::RpcPeer> weak = shared_from_this();
    m_channel->setReceiver([weak](const std::string& text) {
        if (auto self = std::static_pointer_cast<WebRpcConnection>(weak.lock()))
            self->onChannelMessage(text);
    });
}

void WebRpcConnection::emitMessage(const AnyMessage& msg)
{
    if (m_stopped.load()) return;
    // encodeWebMessage THROWS on an oversized message and the throw is allowed
    // to propagate, which is not an oversight: the framed transport does
    // exactly this (encodeFrame refuses and the FramingError leaves
    // sendCall/sendSubscribe on the caller's stack), and "rejected identically"
    // is the point of sharing the cap. Catching it here would make the web
    // transport the quiet way past a limit the other one enforces loudly.
    const std::string text = encodeWebMessage(msg);
    // Called with m_mu HELD from the subscribe paths — see RpcPeer. The channel
    // contract forbids delivering inline, so this cannot re-enter us.
    m_channel->send(text);
}

void WebRpcConnection::closeTransport()
{
    if (!m_channel) return;
    // Only the RECEIVER is dropped here, not the channel. A host owns the
    // channel and may attach a new connection to it (a page that reloads
    // inside the same webview), so closing it on this connection's teardown
    // would take the bridge down with the conversation. Dropping the receiver
    // is what "no more messages reach this peer" means, and it waits for an
    // in-flight delivery unless we ARE that delivery.
    m_channel->setReceiver(nullptr);
}

void WebRpcConnection::onChannelMessage(const std::string& text)
{
    if (m_stopped.load()) return;
    AnyMessage msg;
    try {
        msg = decodeWebMessage(text);
    } catch (const std::exception& e) {
        // Same verdict the framed transport reaches for a bad frame: a peer
        // that cannot be parsed cannot be talked to, so the conversation ends
        // and every waiter is answered with a transport error.
        fail(std::string("web decode error: ") + e.what());
        return;
    }
    dispatchIncoming(std::move(msg));
}

} // namespace logos::web
