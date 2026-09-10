#include "in_memory_channel.h"

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace logos::web {

namespace {

// The half of an endpoint the pump thread touches, with a lifetime of its own.
//
// SEPARATE FROM THE ENDPOINT ON PURPOSE. The pump has to be able to outlive the
// InMemoryChannel that started it, because close() is reachable FROM the pump:
// a decode error inside a receiver runs RpcPeer::fail(), which calls
// closeTransport(), which closes this very channel. A design where the endpoint
// joins its own pump unconditionally deadlocks on exactly that path (a thread
// cannot join itself), and one where the pump holds the endpoint alive never
// destroys it at all. So the shared state is refcounted, the pump owns a
// reference to it, and the endpoint owns only the std::thread handle — which it
// joins when it can and detaches when it is the pump.
struct ChannelState {
    std::mutex                     mu;
    std::condition_variable        cv;
    std::deque<std::string>        inbox;
    bool                           closed = false;

    // Held across the DELIVERY, so setReceiver(nullptr) waits for an in-flight
    // receiver to return — the second half of the IMessageChannel contract, and
    // what lets a peer detach and then be destroyed without racing its own
    // callback. A separate mutex from `mu` because the receiver calls back into
    // the transport, which sends, which queues onto some channel's inbox.
    //
    // RECURSIVE, and it has to be: the receiver is allowed to detach itself.
    // A malformed message runs RpcPeer::fail(), which calls closeTransport(),
    // which is setReceiver(nullptr) on THIS channel — re-entering this mutex on
    // the pump's own thread. With a plain std::mutex that is a self-deadlock on
    // the one path a transport most needs to survive. Recursion changes nothing
    // for any OTHER thread, which still waits out the delivery.
    std::recursive_mutex           receiverMu;
    IMessageChannel::Receiver      receiver;

    bool queue(const std::string& message)
    {
        {
            std::lock_guard<std::mutex> g(mu);
            if (closed) return false;
            inbox.push_back(message);
        }
        cv.notify_one();
        return true;
    }

    void close()
    {
        {
            std::lock_guard<std::mutex> g(mu);
            if (closed) return;
            closed = true;
            inbox.clear();
        }
        cv.notify_all();
    }

    void drain()
    {
        for (;;) {
            std::string message;
            {
                std::unique_lock<std::mutex> lock(mu);
                cv.wait(lock, [this] { return closed || !inbox.empty(); });
                if (closed) return;
                message = std::move(inbox.front());
                inbox.pop_front();
            }
            // `mu` released: the receiver will call back into the transport (a
            // Result arriving wakes a caller that immediately sends the next
            // Call), and holding the inbox lock across that would serialize the
            // conversation against itself.
            std::lock_guard<std::recursive_mutex> g(receiverMu);
            // COPIED, then invoked. A receiver may detach itself mid-delivery
            // (see receiverMu), and calling the MEMBER directly would then be
            // executing a std::function that setReceiver has just destroyed
            // underneath it. The copy outlives the reassignment.
            const IMessageChannel::Receiver cb = receiver;
            if (cb) cb(message);
        }
    }
};

class InMemoryChannel : public IMessageChannel {
public:
    InMemoryChannel()
        : m_state(std::make_shared<ChannelState>())
    {
        auto state = m_state;   // the pump's own reference — see ChannelState
        m_pump = std::thread([state] { state->drain(); });
    }

    ~InMemoryChannel() override { shutdown(); }

    void setPeer(const std::shared_ptr<InMemoryChannel>& peer) { m_peer = peer->m_state; }

    void setReceiver(Receiver receiver) override
    {
        std::lock_guard<std::recursive_mutex> g(m_state->receiverMu);
        m_state->receiver = std::move(receiver);
    }

    bool send(const std::string& message) override
    {
        auto peer = m_peer.lock();
        if (!peer) return false;
        // Queued on the PEER's inbox and delivered by the PEER's pump, so this
        // returns without touching the far end's receiver — the asynchronous
        // half of the IMessageChannel contract.
        return peer->queue(message);
    }

    void close() override { shutdown(); }

    bool isOpen() const override
    {
        std::lock_guard<std::mutex> g(m_state->mu);
        return !m_state->closed;
    }

private:
    void shutdown()
    {
        m_state->close();
        std::lock_guard<std::mutex> g(m_pumpMu);
        if (!m_pump.joinable()) return;
        if (m_pump.get_id() == std::this_thread::get_id()) {
            // We ARE the pump: a receiver closed the channel it was delivering
            // on. Detaching is safe because the thread holds its own reference
            // to the state and will fall out of drain() on its next turn.
            m_pump.detach();
        } else {
            m_pump.join();
        }
    }

    std::shared_ptr<ChannelState> m_state;
    std::weak_ptr<ChannelState>   m_peer;
    std::mutex                    m_pumpMu;
    std::thread                   m_pump;
};

} // namespace

std::pair<MessageChannelPtr, MessageChannelPtr> makeInMemoryChannelPair()
{
    auto a = std::make_shared<InMemoryChannel>();
    auto b = std::make_shared<InMemoryChannel>();
    a->setPeer(b);
    b->setPeer(a);
    return { a, b };
}

} // namespace logos::web
