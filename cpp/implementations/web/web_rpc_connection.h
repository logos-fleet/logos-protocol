#ifndef LOGOS_WEB_RPC_CONNECTION_H
#define LOGOS_WEB_RPC_CONNECTION_H

#include "incoming_call_handler.h"
#include "message_channel.h"
#include "rpc_peer.h"

#include <memory>
#include <string>

namespace logos::web {

// -----------------------------------------------------------------------------
// WebRpcConnection — one full-duplex RPC conversation over an IMessageChannel.
//
// The conversation itself — pending calls, subscription registries, the
// teardown ordering — is RpcPeer's and is shared verbatim with the TCP
// transport. This class is only the two ends RpcPeer leaves open:
//
//   * emitMessage() encodes to one JSON text and hands it to the channel;
//   * an inbound text is decoded and handed straight to dispatchIncoming().
//
// There is no strand and no write queue, and neither is missing: a message
// channel is already message-oriented and already ordered, so the queue the
// Asio transport keeps in order to reassemble a byte stream has nothing to do.
// Sends are serialized only by the channel itself.
//
// Like RpcConnection it works in both directions: a null handler makes it a
// pure consumer (the browser side of a bridge), a non-null one lets it serve
// inbound Call/Methods/Subscribe/Token (the host side).
//
// Lifecycle: heap-allocate via std::make_shared, call start() once, and let
// stop() or destruction tear it down. start() installs the channel receiver;
// teardown removes it and waits for any in-flight delivery, so no callback can
// reach a handler its owner has already destroyed.
// -----------------------------------------------------------------------------
class WebRpcConnection : public logos::plain::RpcPeer {
public:
    WebRpcConnection(MessageChannelPtr channel,
                     logos::plain::IncomingCallHandler* handler = nullptr);
    ~WebRpcConnection() override;

    void start() override;

private:
    // RpcPeer
    void emitMessage(const logos::plain::AnyMessage& msg) override;
    void closeTransport() override;

    void onChannelMessage(const std::string& text);

    MessageChannelPtr m_channel;
};

} // namespace logos::web

#endif // LOGOS_WEB_RPC_CONNECTION_H
