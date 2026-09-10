#ifndef LOGOS_WEB_MESSAGE_CHANNEL_H
#define LOGOS_WEB_MESSAGE_CHANNEL_H

#include <functional>
#include <memory>
#include <string>

namespace logos::web {

// -----------------------------------------------------------------------------
// IMessageChannel — a bidirectional pipe that moves whole MESSAGES.
//
// This is the entire dependency the web transport has on its environment, and
// it is deliberately smaller than a socket: no length prefix, no partial reads,
// no addressing. Every host the web transport is meant for already delivers
// discrete messages — a webview's postMessage, a custom-URL-scheme fetch pump,
// a WebAssembly host's port — so a byte stream would only mean re-framing
// something that was already framed.
//
// The channel is INJECTED rather than dialed. A test binds an in-memory pair
// (in_memory_channel.h); a host binds one endpoint to the webview it owns,
// which is also what makes a Web module's identity structural — the channel a
// message arrived on IS the module that sent it, so a stolen token cannot
// impersonate another module (see ADR 0005).
//
// CONTRACT, and both halves matter to the peer above:
//
//   * DELIVERY IS ASYNCHRONOUS. send() may not invoke the far end's receiver on
//     the calling thread. RpcPeer writes messages with its registry mutex held
//     (see RpcPeer::sendSubscribe), so a channel that delivered inline would
//     re-enter that peer under its own lock and deadlock. Enqueue and return.
//   * THE RECEIVER MAY RUN ON ANY THREAD, and never concurrently with itself
//     for one channel: messages are delivered one at a time, in send order.
//     setReceiver(nullptr) must not return while a delivery is still running,
//     so a peer can detach and then be destroyed.
// -----------------------------------------------------------------------------
class IMessageChannel {
public:
    using Receiver = std::function<void(const std::string& message)>;

    virtual ~IMessageChannel() = default;

    // Install (or, with nullptr, remove) the sink for messages from the far
    // end. Removing waits for an in-flight delivery to finish — see above.
    virtual void setReceiver(Receiver receiver) = 0;

    // Hand one message to the far end. Returns false when the channel is
    // closed. Never blocks on the far end and never delivers inline.
    virtual bool send(const std::string& message) = 0;

    // Close this endpoint. Idempotent. After it returns, send() fails and no
    // further message is delivered to this endpoint's receiver.
    virtual void close() = 0;

    virtual bool isOpen() const = 0;
};

using MessageChannelPtr = std::shared_ptr<IMessageChannel>;

// -----------------------------------------------------------------------------
// The process-wide channel source, for the consumer side.
//
// LogosTransportFactory::createConnection() is handed a LogosTransportConfig and
// nothing else — there is no field in it that could carry a webview handle, and
// there should not be. So a host that can produce channels installs a factory
// once at startup (a webview host binds its bridge; a test binds one end of an
// in-memory pair) and every `{"protocol":"web"}` connection resolved after that
// gets one.
//
// Unset by default: createConnection() still returns a WebTransportConnection,
// and connectToHost() reports failure rather than pretending. That is the
// honest answer for a process that has no webview in it.
// -----------------------------------------------------------------------------
using MessageChannelFactory = std::function<MessageChannelPtr()>;

void setMessageChannelFactory(MessageChannelFactory factory);

// A new channel from the installed factory, or nullptr when none is installed
// (or the factory itself declined).
MessageChannelPtr makeMessageChannel();

} // namespace logos::web

#endif // LOGOS_WEB_MESSAGE_CHANNEL_H
