#ifndef LOGOS_PLAIN_RPC_CONNECTION_BASE_H
#define LOGOS_PLAIN_RPC_CONNECTION_BASE_H

#include "rpc_message.h"

#include <cstdint>
#include <functional>
#include <future>
#include <string>

namespace logos::plain {

// -----------------------------------------------------------------------------
// RpcConnectionBase — type-erased public surface of RpcConnection<Stream>.
//
// Callers (plain_logos_object, plain_transport_host) hold a
// shared_ptr<RpcConnectionBase> so they don't have to know whether the
// underlying socket is plain TCP or TLS-wrapped TCP. All the async machinery
// lives in the templated subclass.
// -----------------------------------------------------------------------------
class RpcConnectionBase {
public:
    using ErrorHandler = std::function<void(const std::string& reason)>;
    // A reply, handed over as it arrives instead of parked in a promise.
    //
    // Invoked AT MOST ONCE per call, from one of three places, and a caller has
    // to answer for all three because they are not on the same thread:
    //   * the connection's strand (io thread) when the peer's Result frame is
    //     decoded — the normal path;
    //   * an arbitrary caller thread inside fail(), which sweeps every pending
    //     call when the connection is torn down (stop(), ~PlainTransport-
    //     Connection, RpcServer::stop());
    //   * INLINE on the calling thread, inside sendCallAsync itself, when the
    //     connection is stopped — either before the call was made, or while it
    //     was registering, which is a race sendCallAsync resolves by reclaiming
    //     its own entry rather than by hoping fail() sees it.
    // It must therefore not block and must not run user code directly — see
    // postDelivery in plain_logos_object.cpp.
    //
    // AT MOST ONCE is a property of the REGISTRATION, and it is weaker than it
    // sounds. Four things contend for a registered handler — dispatchIncoming,
    // fail()'s sweep, cancelPending() and sendCallAsync's own reclaim — and the
    // extract-and-erase under m_mu lets exactly one of them have it, so no
    // handler is ever invoked twice.
    //
    // AT LEAST ONCE is the other half, and it is not free either: it holds only
    // because a registration is made BEFORE m_stopped is re-read, so a teardown
    // and a registration cannot pass each other unseen. Get that order wrong and
    // a handler sits in the map of a dead connection forever — see sendCallAsync.
    //
    // What that does NOT buy is a cancel that arrives in time. dispatchIncoming
    // copies the handler out under m_mu and invokes it with the mutex RELEASED,
    // so a cancelPending() landing in that gap erases nothing and the handler
    // runs to completion AFTER cancelPending() has already returned. A caller
    // that gives up must therefore be able to absorb one more call. Both callers
    // here are:
    //   * PlainLogosObject funnels every outcome into AsyncCall::deliver(),
    //     whose CAS makes the later arrival a no-op — that CAS, and nothing at
    //     this layer, is what makes DELIVERY to the user exactly-once;
    //   * sendCall()'s promise handler cannot be reached twice at all (only one
    //     contender ever gets it) and fulfilling a future its caller has already
    //     walked away from is a no-op.
    // test_plain_cancel_pending_race.cpp builds that interleaving by hand rather
    // than racing for it, and pins both.
    using ResultHandler = std::function<void(ResultMessage)>;

    virtual ~RpcConnectionBase() = default;

    virtual void start() = 0;
    virtual void stop(const std::string& reason = "stopped") = 0;
    virtual bool isOpen() const = 0;

    virtual std::future<ResultMessage>        sendCall(CallMessage msg) = 0;
    // The same send, completion-driven. sendCall() is now a thin wrapper over
    // this one (it fulfils a promise from the handler), so there is exactly one
    // registration path and the two cannot drift.
    virtual void sendCallAsync(CallMessage msg, ResultHandler handler) = 0;
    virtual std::future<MethodsResultMessage> sendMethods(MethodsMessage msg) = 0;

    // Forget a pending Call or Methods registration whose caller has given up.
    //
    // THIS IS A RETENTION FIX, and it closes a hole that predates the async
    // rework. m_pendingCalls / m_pendingMethods are emptied by exactly two
    // events: a decoded reply carrying that id, and fail()'s teardown sweep. A
    // call that is resolved by its DEADLINE and never answered is in neither,
    // so its registration — a promise, or now a handler holding the caller's
    // std::function — stayed in the map for the whole life of the connection.
    // Measured against pristine master with a server that never answers: 8.5MB
    // of resident memory over 24,000 orphaned calls, 353 bytes each, growing
    // strictly linearly with the call count. And the connection outlives every
    // handle it hands out, so nothing else was ever going to collect it.
    //
    // Erasing is the right semantic and not merely a cleanup: the caller has
    // already been told the call timed out, so a reply arriving afterwards must
    // be dropped, which is exactly what an absent registration does.
    //
    // Safe to call at any time and from any thread, including for an id that
    // has already been answered (the erase simply finds nothing). Ids come from
    // nextId() and are unique across BOTH maps, so one entry point covers them.
    //
    // BEST EFFORT AGAINST A REPLY ALREADY IN FLIGHT, and deliberately not more.
    // It withdraws a REGISTRATION; it does not stop a handler dispatchIncoming
    // has already taken out of the map. Returning from this is therefore not a
    // guarantee of silence — see ResultHandler for who has to absorb the
    // difference and how.
    virtual void cancelPending(uint64_t id) = 0;

    // Identifies ONE registration, not one (object, event) pair. Local to this
    // process and never on the wire — see sendSubscribe.
    using SubscriptionId = std::uint64_t;

    // Register `callback` for (msg.object, msg.eventName) and put the Subscribe
    // frame on the wire. Returns the token that withdraws THIS registration.
    //
    // MANY REGISTRATIONS PER (object, event) ARE THE POINT. One RpcConnection is
    // shared by every handle a PlainTransportConnection hands out, and
    // requestObject() mints a fresh handle per acquire, so two handles
    // subscribing to the same event on the same module is ordinary — including
    // the deferred-completion channel every handle subscribes to on its first
    // call. This map used to be keyed by (object, event) and ASSIGNED, so the
    // second one silently took the first one's channel away: a lost
    // subscription, never a stale one, whose only symptom was a call that used
    // to answer in single-digit milliseconds waiting out its whole timeout.
    //
    // THE WIRE DOES NOT MOVE, and that is a deliberate choice rather than an
    // omission. Subscribe/Unsubscribe carry (object, event) and nothing else, in
    // both directions, exactly as before — so a new consumer against an old host
    // and an old consumer against a new host both behave exactly as they do
    // today. What changed is WHERE the demultiplexing happens: the host keeps
    // ONE sink per (object, event, connection) — which is the right model,
    // because every sink for one connection is the same "write this frame back
    // down that socket" — and the CONSUMER, which is the only end that knows how
    // many of its own handles want the event, fans the single delivery out. The
    // corollary is the contract sendUnsubscribe implements: an Unsubscribe frame
    // means "this connection wants no more of that event AT ALL", so it may only
    // go out when the last local registration for the pair is gone.
    virtual SubscriptionId sendSubscribe(SubscribeMessage msg,
                                         std::function<void(EventMessage)> callback) = 0;
    // Withdraw one registration. Writes the Unsubscribe frame only when it was
    // the LAST registration for its (object, event) — see sendSubscribe. Safe
    // for an id that has already been withdrawn, or that never existed.
    virtual void sendUnsubscribe(SubscriptionId id) = 0;
    virtual void sendEvent(EventMessage msg) = 0;
    virtual void sendToken(TokenMessage msg) = 0;

    virtual void setErrorHandler(ErrorHandler handler) = 0;
    virtual uint64_t nextId() = 0;
};

} // namespace logos::plain

#endif // LOGOS_PLAIN_RPC_CONNECTION_BASE_H
