#include "rpc_peer.h"

#include <algorithm>

namespace logos::plain {

RpcPeer::RpcPeer(IncomingCallHandler* handler)
    : m_handler(handler)
{
}

RpcPeer::~RpcPeer() = default;

void RpcPeer::stop(const std::string& reason)
{
    fail(reason);
}

void RpcPeer::cancelPending(uint64_t id)
{
    std::lock_guard<std::mutex> g(m_mu);
    m_pendingCalls.erase(id);
    m_pendingMethods.erase(id);
}

void RpcPeer::dispatchIncoming(AnyMessage msg)
{
    std::visit([this](auto&& m) {
        using T = std::decay_t<decltype(m)>;

        if constexpr (std::is_same_v<T, ResultMessage>) {
            ResultHandler h;
            {
                std::lock_guard<std::mutex> g(m_mu);
                auto it = m_pendingCalls.find(m.id);
                if (it != m_pendingCalls.end()) {
                    h = std::move(it->second);
                    m_pendingCalls.erase(it);
                }
            }
            // Erased under the lock BEFORE the call, so this, fail()'s sweep and
            // cancelPending() cannot all get the same handler — that is what
            // makes INVOCATION at-most-once at this layer, and it is the whole
            // of what this layer promises. It is NOT a cancellation barrier: the
            // call below runs with m_mu released, so a cancelPending() racing it
            // finds the entry already gone, erases nothing, and returns while
            // this handler is still running. Exactly-once DELIVERY belongs to the
            // handler — see ResultHandler.
            if (h) h(std::forward<decltype(m)>(m));

        } else if constexpr (std::is_same_v<T, MethodsResultMessage>) {
            std::shared_ptr<std::promise<MethodsResultMessage>> p;
            {
                std::lock_guard<std::mutex> g(m_mu);
                auto it = m_pendingMethods.find(m.id);
                if (it != m_pendingMethods.end()) {
                    p = std::move(it->second);
                    m_pendingMethods.erase(it);
                }
            }
            if (p) p->set_value(std::forward<decltype(m)>(m));

        } else if constexpr (std::is_same_v<T, EventMessage>) {
            // ONE frame, EVERY local subscriber. The host sends a connection a
            // single copy of an event (one sink per object/event/connection), so
            // this is the only place that knows how many handles asked for it.
            // Copied out under m_mu and invoked with the mutex RELEASED — the
            // same shape the single-callback version had, and for the same
            // reason: a subscriber may call back into this connection.
            std::vector<std::function<void(EventMessage)>> cbs;
            {
                std::lock_guard<std::mutex> g(m_mu);
                auto collect = [&](const EventKey& key) {
                    auto it = m_eventSubs.find(key);
                    if (it == m_eventSubs.end()) return;
                    for (const auto& sub : it->second) cbs.push_back(sub.callback);
                };
                collect({m.object, m.eventName});
                // The wildcard key IS the named key for an event whose name is
                // empty; visiting it twice would deliver that event twice.
                if (!m.eventName.empty()) collect({m.object, std::string{}});
            }
            for (auto& cb : cbs) cb(m);

        } else if constexpr (std::is_same_v<T, CallMessage>) {
            if (!m_handler) return;
            auto self = shared_from_this();
            m_handler->onCall(m, [self](ResultMessage res) {
                self->emitMessage(AnyMessage{std::move(res)});
            });

        } else if constexpr (std::is_same_v<T, MethodsMessage>) {
            if (!m_handler) return;
            auto self = shared_from_this();
            m_handler->onMethods(m, [self](MethodsResultMessage res) {
                self->emitMessage(AnyMessage{std::move(res)});
            });

        } else if constexpr (std::is_same_v<T, SubscribeMessage>) {
            if (!m_handler) return;
            // weak_ptr capture so the host's stored sink doesn't keep the
            // connection alive past its natural lifetime — without this,
            // `[self]` would leak every subscribed connection until
            // unsubscribe (which a crashing client never sends).
            std::weak_ptr<RpcPeer> weak = shared_from_this();
            const void* connId = static_cast<const void*>(this);
            m_handler->onSubscribe(m, [weak](EventMessage evt) {
                if (auto self = weak.lock()) self->sendEvent(std::move(evt));
            }, connId);

        } else if constexpr (std::is_same_v<T, UnsubscribeMessage>) {
            if (m_handler)
                m_handler->onUnsubscribe(m, static_cast<const void*>(this));

        } else if constexpr (std::is_same_v<T, TokenMessage>) {
            if (m_handler) m_handler->onToken(m);
        }
    }, std::move(msg));
}

std::future<ResultMessage> RpcPeer::sendCall(CallMessage msg)
{
    auto p = std::make_shared<std::promise<ResultMessage>>();
    auto f = p->get_future();
    // The promise is now just one shape of handler. Everything the future path
    // relied on — registration under m_mu before the write, the stopped answer,
    // fail()'s sweep — lives in sendCallAsync and is shared verbatim, which is
    // also why this path inherits the register-then-re-check ordering there
    // instead of needing its own.
    sendCallAsync(std::move(msg), [p](ResultMessage r) {
        try { p->set_value(std::move(r)); } catch (...) {}
    });
    return f;
}

// REGISTER FIRST, THEN RE-CHECK — the order is the whole of this function, and
// it is the opposite of what reads naturally.
//
// The obvious spelling is "if stopped, answer inline; otherwise register". It
// has a hole, because the two things it does are ordered the opposite way round
// from fail(): this reads m_stopped and then takes m_mu, while fail() writes
// m_stopped and then takes m_mu. A fail() that completes in between sweeps a
// map this caller has not written to yet —
//
//     caller                              fail()
//     ------------------------------      -------------------------------
//     m_stopped.load()  -> false
//                                         CAS m_stopped -> true
//                                         lock(m_mu); swap(m_pendingCalls)
//                                         unlock(m_mu)   ... the map was EMPTY
//     lock(m_mu); m_pendingCalls[id] = h
//     emitMessage()                       ... drops: stopped
//
// — and the handler is left in the pending map of a connection nobody will
// sweep again. fail() runs once and has been; no reply can arrive on a closed
// wire; the message was never written. THE CALL IS ANSWERED BY NOTHING, and
// what answers instead is the caller's deadline: callMethodAsyncWithError
// reports "timeout" after its full timeoutMs, and getMethods() after a
// hard-coded five seconds. A connection already known to be gone is reported as
// a peer that was merely slow, which is also the code callers retry on.
//
// Registering unconditionally and then re-reading m_stopped closes it, and the
// reason it closes it is an ordering argument rather than a smaller window:
//
//   * if fail()'s sweep ran BEFORE the registration, then its CAS ran before
//     that, so this re-read cannot see false. The reclaim below finds the
//     entry and answers the call.
//   * if this re-read DOES see false, then in the total order over m_stopped
//     this load precedes fail()'s store, and the registration is
//     sequenced-before this load, so it precedes fail()'s lock of m_mu — the
//     sweep is guaranteed to find the entry and answer the call.
//
// There is no third case, and neither branch can lose the handler: exactly one
// of the reclaim and the sweep extracts it, because both extract-and-erase
// under m_mu. That is the same single-winner rule dispatchIncoming and
// cancelPending already play by, with one more contender.
//
// WHAT WAS REJECTED, since the tempting fixes are the ones that deadlock:
//
//   * "Hold m_mu across the check AND the delivery" — i.e. answer the doomed
//     call from inside the lock. It self-deadlocks on the first inline
//     delivery: a handler here is AsyncCall's, which calls deliver(), which
//     calls RpcConnectionBase::cancelPending() to withdraw its own
//     registration, which takes m_mu — and m_mu is not recursive. The same
//     objection kills the symmetric version (fail() invoking its swept handlers
//     under the lock). Nothing in this class may invoke a handler with m_mu
//     held, which is why the reclaim below drops the lock first.
//   * "Move fail()'s once-only CAS under m_mu and check m_stopped under the
//     same lock here." Correct, and it does not deadlock — but it makes
//     teardown's flag wait on a mutex every in-flight send and every decoded
//     reply also takes, so m_stopped stops being the instantly-visible "stop
//     writing" signal that the write paths all read lock-free. That trades a
//     race for a teardown-latency regression, and teardown latency is a
//     guarantee here.
//   * "Let the write path notice." RpcConnection's writeFrame() already
//     re-checks m_stopped on the strand — and DROPS, silently, which is exactly
//     the state being fixed. It also cannot answer anything if the io_context
//     is stopped and the posted handler never runs.
//
// The cost is one map insert plus one erase for a call made on a connection
// that is already dead — a case that used to return without touching the map.
// That buys a single registration path (the already-stopped call and the raced
// one are now the same code) instead of two that have to be kept in agreement.
//
// ONE BEHAVIOURAL CONSEQUENCE OF REGISTERING UNCONDITIONALLY, which is benign
// but is not obvious: a call on an already-dead connection is now briefly
// visible in m_pendingCalls, so a cancelPending() for the same id landing in
// that window takes the entry and this function delivers nothing. That is
// correct rather than tolerated. The only thing that can cancel an id whose
// sendCallAsync has not returned yet is AsyncCall's deadline (armTimer runs
// before the send), and reaching deliver() means the caller has already had its
// one callback — a second one would be the bug. It is also exactly what
// cancelPending() is documented to mean: the caller has been answered, so a
// later answer must be dropped.
void RpcPeer::sendCallAsync(CallMessage msg, ResultHandler handler)
{
    if (!handler) return;
    const uint64_t id = msg.id;
    {
        std::lock_guard<std::mutex> g(m_mu);
        m_pendingCalls[id] = std::move(handler);
    }

    if (m_stopped.load()) {
        // Take back OUR OWN registration — by id, so this can never remove
        // somebody else's (ids come from nextId() and are unique across both
        // pending maps). Finding nothing is the ordinary outcome when fail()'s
        // sweep got here first, and it means the call has already been
        // answered.
        ResultHandler mine;
        {
            std::lock_guard<std::mutex> g(m_mu);
            auto it = m_pendingCalls.find(id);
            if (it != m_pendingCalls.end()) {
                mine = std::move(it->second);
                m_pendingCalls.erase(it);
            }
        }
        // Answered INLINE, on the caller's thread, with m_mu RELEASED. That is
        // the same shape the future path had (it set the promise before
        // returning it), and it is why every handler in this codebase has to be
        // non-blocking and has to hand user code off to the Qt loop rather than
        // run it here — this thread can be the io thread, since a user event
        // callback runs inline on it and is allowed to make calls.
        if (mine) {
            ResultMessage r;
            r.id = id; r.ok = false;
            r.err = "connection stopped"; r.errCode = "TRANSPORT_CLOSED";
            mine(std::move(r));
        }
        return;
    }

    emitMessage(AnyMessage{std::move(msg)});
}

// The same register-then-re-check as sendCallAsync, for the same reason and
// with the same ordering argument — see the comment there. Kept as its own
// registration rather than folded into that one because the maps hold different
// things (a promise, not a handler); what must not diverge is the ORDER, and it
// does not.
//
// It matters more here, if anything: the only caller, getMethods(), waits on
// this future for a hard-coded five seconds that no caller can shorten, so a
// lost registration is a fixed five-second stall in module introspection every
// time a connection drops underneath it.
std::future<MethodsResultMessage> RpcPeer::sendMethods(MethodsMessage msg)
{
    auto p = std::make_shared<std::promise<MethodsResultMessage>>();
    auto f = p->get_future();
    const uint64_t id = msg.id;
    {
        std::lock_guard<std::mutex> g(m_mu);
        m_pendingMethods[id] = p;
    }

    if (m_stopped.load()) {
        std::shared_ptr<std::promise<MethodsResultMessage>> mine;
        {
            std::lock_guard<std::mutex> g(m_mu);
            auto it = m_pendingMethods.find(id);
            if (it != m_pendingMethods.end()) {
                mine = std::move(it->second);
                m_pendingMethods.erase(it);
            }
        }
        if (mine) {
            MethodsResultMessage r;
            r.id = id; r.ok = false; r.err = "connection stopped";
            // Same containment as fail()'s sweep: a promise whose future has
            // already been consumed throws, and that is not this caller's
            // problem.
            try { mine->set_value(std::move(r)); } catch (...) {}
        }
        return f;
    }

    emitMessage(AnyMessage{std::move(msg)});
    return f;
}

// THE THIRD REGISTRATION WITH THE SAME SHAPE, and deliberately NOT given the
// same treatment — recorded here so the next reader does not have to wonder
// whether it was missed. A fail() landing between the map write below and the
// emitMessage() leaves a callback in m_eventSubs that fail() has already
// cleared, exactly as it would have left a pending call.
//
// What that costs is different in kind, and that is the whole reason: nobody is
// WAITING on a subscription. There is no deadline to blow, no error code to get
// wrong and no caller to strand — the connection is dead, so no event can ever
// arrive to invoke it, and the entry dies with the connection (which the handle
// does not outlive). Adding a reclaim here would buy one map erase and a second
// mutex round trip on the subscribe path in exchange for nothing observable, so
// the pending maps get the fix and this does not.
RpcConnectionBase::SubscriptionId
RpcPeer::sendSubscribe(SubscribeMessage msg, std::function<void(EventMessage)> cb)
{
    const SubscriptionId sid = m_nextSubId.fetch_add(1, std::memory_order_relaxed);
    // The message is written WITH m_mu HELD, which is not tidiness. emitMessage
    // only serializes and hands the bytes to the wire (no user code, no m_mu),
    // so the lock orders the WRITES by the order the registry decisions were
    // made. Without that, an unsubscribe that had just decided "I am the last
    // one" could have its Unsubscribe overtaken by a concurrent handle's
    // Subscribe, and the host would end up honouring the unsubscribe LAST —
    // dropping the sink of a handle that is still registered locally, which is
    // exactly the silent "subscribed but no events" state this exists to
    // remove.
    std::lock_guard<std::mutex> g(m_mu);
    const EventKey key{msg.object, msg.eventName};
    m_eventSubs[key].push_back(EventSubscription{sid, std::move(cb)});
    m_subKeys.emplace(sid, key);
    // Re-sent for every registration, not just the first: it is idempotent at the
    // host (onSubscribe assigns one sink per object/event/connection) and it makes
    // a second handle re-assert a subscription the host may have dropped — an
    // object that was not published yet when the first Subscribe arrived is
    // silently ignored there.
    emitMessage(AnyMessage{std::move(msg)});
    return sid;
}

void RpcPeer::sendUnsubscribe(SubscriptionId id)
{
    std::lock_guard<std::mutex> g(m_mu);   // held across the write — see sendSubscribe
    auto kit = m_subKeys.find(id);
    if (kit == m_subKeys.end()) return;    // already withdrawn, or never existed
    const EventKey key = kit->second;
    m_subKeys.erase(kit);

    auto vit = m_eventSubs.find(key);
    if (vit != m_eventSubs.end()) {
        auto& subs = vit->second;
        subs.erase(std::remove_if(subs.begin(), subs.end(),
                                  [id](const EventSubscription& s) { return s.id == id; }),
                   subs.end());
        // STILL WANTED LOCALLY: say nothing. The Unsubscribe message is
        // connection-wide — the host has one sink for this connection and would
        // drop it — so telling the host now would silence every sibling handle
        // that is still subscribed. That is the whole of the host-side half of
        // this rule, and it needs no wire change to work.
        if (!subs.empty()) return;
        m_eventSubs.erase(vit);
    }

    UnsubscribeMessage msg;
    msg.object    = key.first;
    msg.eventName = key.second;
    emitMessage(AnyMessage{std::move(msg)});
}

void RpcPeer::sendEvent(EventMessage msg)
{
    emitMessage(AnyMessage{std::move(msg)});
}

void RpcPeer::sendToken(TokenMessage msg)
{
    emitMessage(AnyMessage{std::move(msg)});
}

void RpcPeer::fail(const std::string& reason)
{
    bool expected = false;
    if (!m_stopped.compare_exchange_strong(expected, true)) return;

    // Fail every pending call with a transport-level error.
    std::map<uint64_t, ResultHandler>                                       calls;
    std::map<uint64_t, std::shared_ptr<std::promise<MethodsResultMessage>>> methods;
    ErrorHandler errCb;
    {
        std::lock_guard<std::mutex> g(m_mu);
        calls.swap(m_pendingCalls);
        methods.swap(m_pendingMethods);
        errCb.swap(m_error);
        m_eventSubs.clear();
        m_subKeys.clear();
    }
    for (auto& [id, h] : calls) {
        ResultMessage r; r.id = id; r.ok = false;
        r.err = reason; r.errCode = "TRANSPORT_ERROR";
        // Runs on WHATEVER THREAD called stop() — usually not the io thread.
        // Handlers are written for that (see ResultHandler); the try/catch is
        // the same containment the promise sweep already had.
        try { h(std::move(r)); } catch (...) {}
    }
    for (auto& [id, p] : methods) {
        MethodsResultMessage r; r.id = id; r.ok = false; r.err = reason;
        try { p->set_value(std::move(r)); } catch (...) {}
    }

    closeTransport();

    // Notify the dispatch handler so it can drop any subscriptions still
    // keyed to this connection. Without this, a connection that drops
    // without sending Unsubscribe leaks sinks in the host's per-event map.
    if (m_handler) {
        try { m_handler->onConnectionClosed(static_cast<const void*>(this)); }
        catch (...) {}
    }

    if (errCb) errCb(reason);
}

} // namespace logos::plain
