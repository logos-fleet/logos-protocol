#ifndef LOGOS_PLAIN_RPC_PEER_H
#define LOGOS_PLAIN_RPC_PEER_H

#include "incoming_call_handler.h"
#include "rpc_connection_base.h"
#include "rpc_message.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace logos::plain {

// -----------------------------------------------------------------------------
// RpcPeer — everything an RPC conversation is once you take the socket out.
//
// The registration bookkeeping is the hard part of this protocol and none of it
// is about bytes: which pending call a Result belongs to, which of several local
// handles a single Event copy fans out to, when an Unsubscribe may go on the
// wire, and the register-then-re-check ordering that keeps a teardown racing a
// send from stranding a caller on its deadline. Every comment those rules earned
// lives on the members and methods below.
//
// A transport supplies two things and inherits the rest:
//
//   * emitMessage() — serialize one message and hand it to the wire. Called
//     with m_mu HELD from sendSubscribe()/sendUnsubscribe() (see sendSubscribe
//     for why the lock spans the write), so an implementation must not take
//     m_mu, must not run user code, and must not deliver the message
//     synchronously back into this peer.
//   * closeTransport() — tear the wire down. Called exactly once, from fail().
//
// RpcConnection<Stream> is the Boost.Asio socket implementation (length-prefixed
// frames); WebRpcConnection is the message-channel one (whole JSON messages, no
// framing). They differ in those two functions and in how bytes arrive; the
// conversation itself is here, once.
// -----------------------------------------------------------------------------
class RpcPeer
    : public RpcConnectionBase
    , public std::enable_shared_from_this<RpcPeer>
{
public:
    explicit RpcPeer(IncomingCallHandler* handler);
    ~RpcPeer() override;

    void stop(const std::string& reason = "stopped") override;
    bool isOpen() const override { return !m_stopped.load(); }

    std::future<ResultMessage>        sendCall(CallMessage msg) override;
    void                              sendCallAsync(CallMessage msg,
                                                    ResultHandler handler) override;
    std::future<MethodsResultMessage> sendMethods(MethodsMessage msg) override;

    void cancelPending(uint64_t id) override;

    SubscriptionId sendSubscribe(SubscribeMessage msg,
                                 std::function<void(EventMessage)> callback) override;
    void sendUnsubscribe(SubscriptionId id) override;
    void sendEvent(EventMessage msg) override;
    void sendToken(TokenMessage msg) override;

    void setErrorHandler(ErrorHandler handler) override {
        std::lock_guard<std::mutex> g(m_mu);
        m_error = std::move(handler);
    }

    uint64_t nextId() override {
        return m_nextId.fetch_add(1, std::memory_order_relaxed);
    }

protected:
    // See the class comment: called under m_mu from the subscribe paths.
    virtual void emitMessage(const AnyMessage& msg) = 0;
    // Called once, from fail(), on whatever thread tore the peer down.
    virtual void closeTransport() = 0;

    // Route one decoded message: a reply to its waiter, an event to every local
    // subscriber, an inbound request to the IncomingCallHandler.
    void dispatchIncoming(AnyMessage msg);

    // Answer every pending caller with a transport error, drop the
    // subscriptions, close the wire and tell the handler this peer is gone.
    // Idempotent — only the first caller does anything.
    void fail(const std::string& reason);

    IncomingCallHandler* m_handler;

    // Outgoing-pending maps. Calls hold a HANDLER rather than a promise: the
    // promise is one possible handler (see sendCall), not the mechanism.
    std::mutex                                                              m_mu;
    std::map<uint64_t, ResultHandler>                                       m_pendingCalls;
    std::map<uint64_t, std::shared_ptr<std::promise<MethodsResultMessage>>> m_pendingMethods;

    using EventKey = std::pair<std::string, std::string>; // object, event
    struct EventSubscription {
        SubscriptionId                    id;
        std::function<void(EventMessage)> callback;
    };
    // A LIST per key, in registration order, because several handles on this one
    // connection legitimately want the same event (see sendSubscribe).
    std::map<EventKey, std::vector<EventSubscription>> m_eventSubs;
    // Reverse index, so withdrawing a registration is a lookup rather than a walk
    // of every key. Kept exactly in step with m_eventSubs under m_mu.
    std::map<SubscriptionId, EventKey>                m_subKeys;
    std::atomic<SubscriptionId>                       m_nextSubId{1};

    ErrorHandler          m_error;
    std::atomic<uint64_t> m_nextId{1};
    std::atomic<bool>     m_stopped{false};
    std::atomic<bool>     m_started{false};
};

} // namespace logos::plain

#endif // LOGOS_PLAIN_RPC_PEER_H
