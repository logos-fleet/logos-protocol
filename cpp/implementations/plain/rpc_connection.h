#ifndef LOGOS_PLAIN_RPC_CONNECTION_H
#define LOGOS_PLAIN_RPC_CONNECTION_H

#include "incoming_call_handler.h"
#include "rpc_framing.h"
#include "rpc_message.h"
#include "rpc_peer.h"
#include "wire_codec.h"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <deque>
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
// RpcConnection<Stream> — one full-duplex RPC conversation over a Boost.Asio
// stream-like socket (plain TCP or SSL-wrapped TCP, sharing this template).
//
// Roles: the same connection supports both directions. Either peer can
// initiate Call / Methods / Subscribe / Token / Event messages. Provider-side
// dispatch of inbound Call/Methods/Subscribe/Token goes through an
// IncomingCallHandler supplied at construction (may be null for pure-consumer
// connections).
//
// Lifecycle: heap-allocated via std::make_shared; call start() once the
// socket is ready; call stop() (or destroy) to tear down.
//
// Everything that is NOT about the socket — the pending-call registry, the
// subscription registry, the ordering rules that keep a teardown from
// stranding a caller — lives in RpcPeer and is shared with the web transport.
// What is left here is the framing (a 4-byte length prefix and a type tag) and
// the Asio strand that serializes every touch of the socket.
// -----------------------------------------------------------------------------
template <typename Stream>
class RpcConnection : public RpcPeer
{
public:
    RpcConnection(Stream stream,
                  std::shared_ptr<IWireCodec> codec,
                  IncomingCallHandler* handler = nullptr);

    void start() override;

private:
    void doRead();
    void handleFrame(MessageType tag, std::vector<uint8_t> payload);
    void writeFrame(std::vector<uint8_t> frame);
    void doWrite();

    // RpcPeer
    void emitMessage(const AnyMessage& msg) override {
        writeFrame(encodeFrame(*m_codec, msg));
    }
    void closeTransport() override { closeStreamOnStrand(); }

    // Close the socket. MUST run on m_strand — see closeStreamOnStrand().
    void closeStream();
    void closeStreamOnStrand();

    // RpcPeer holds the only enable_shared_from_this base, so the Asio handlers
    // — which need the DERIVED type to reach m_stream — narrow it here rather
    // than each writing the cast out.
    std::shared_ptr<RpcConnection<Stream>> sharedSelf() {
        return std::static_pointer_cast<RpcConnection<Stream>>(shared_from_this());
    }

    Stream                                       m_stream;
    std::shared_ptr<IWireCodec>                  m_codec;
    boost::asio::strand<boost::asio::any_io_executor> m_strand;

    // Read side
    FrameReader                                  m_reader;
    std::vector<uint8_t>                         m_readBuf;

    // Write side
    std::deque<std::vector<uint8_t>>             m_writeQueue;
    bool                                         m_writing = false;
};

// ── Template implementation (must be visible at instantiation sites) ─────

template <typename Stream>
RpcConnection<Stream>::RpcConnection(Stream stream,
                                     std::shared_ptr<IWireCodec> codec,
                                     IncomingCallHandler* handler)
    : RpcPeer(handler)
    , m_stream(std::move(stream))
    , m_codec(std::move(codec))
    , m_strand(boost::asio::make_strand(m_stream.get_executor()))
{
    m_readBuf.resize(4096);
}

template <typename Stream>
void RpcConnection<Stream>::start()
{
    bool expected = false;
    if (!m_started.compare_exchange_strong(expected, true)) return;
    auto self = sharedSelf();
    boost::asio::post(m_strand, [self] { self->doRead(); });
}

template <typename Stream>
void RpcConnection<Stream>::doRead()
{
    auto self = sharedSelf();
    m_stream.async_read_some(boost::asio::buffer(m_readBuf),
        boost::asio::bind_executor(m_strand,
            [self](const boost::system::error_code& ec, std::size_t n) {
                if (ec) { self->fail(ec.message()); return; }
                // fail() may have run on another thread while this read was in
                // flight. Before the close moved onto the strand it aborted the
                // read immediately, so a stopped connection could not deliver
                // one more frame; now the socket stays open until the strand
                // gets to it, and a frame arriving in that gap would be
                // dispatched into an IncomingCallHandler its owner may already
                // have torn down. The connection is dead either way — drop it.
                if (self->m_stopped.load()) return;
                try {
                    self->m_reader.append(self->m_readBuf.data(), n);
                    MessageType tag;
                    std::vector<uint8_t> payload;
                    while (self->m_reader.next(tag, payload)) {
                        self->handleFrame(tag, std::move(payload));
                    }
                } catch (const std::exception& e) {
                    self->fail(std::string("frame error: ") + e.what());
                    return;
                }
                self->doRead();
            }));
}

template <typename Stream>
void RpcConnection<Stream>::handleFrame(MessageType tag, std::vector<uint8_t> payload)
{
    AnyMessage msg;
    try {
        msg = m_codec->decode(tag, payload.data(), payload.size());
    } catch (const std::exception& e) {
        fail(std::string("decode error: ") + e.what());
        return;
    }
    dispatchIncoming(std::move(msg));
}

template <typename Stream>
void RpcConnection<Stream>::writeFrame(std::vector<uint8_t> frame)
{
    if (m_stopped.load()) return;
    auto self = sharedSelf();
    boost::asio::post(m_strand, [self, frame = std::move(frame)]() mutable {
        // Re-check inside the strand: the load above is a hint, and fail()
        // can land between it and this handler. Without this the queued
        // frame would start an async_write on a socket fail() is closing.
        if (self->m_stopped.load()) return;
        self->m_writeQueue.push_back(std::move(frame));
        if (!self->m_writing) {
            self->m_writing = true;
            self->doWrite();
        }
    });
}

template <typename Stream>
void RpcConnection<Stream>::doWrite()
{
    // Runs on m_strand. fail() may have closed the socket already (via a
    // close it dispatched onto this same strand); starting another write
    // would only produce a bad_descriptor completion.
    if (m_stopped.load()) { m_writing = false; return; }
    auto self = sharedSelf();
    boost::asio::async_write(m_stream,
        boost::asio::buffer(m_writeQueue.front()),
        boost::asio::bind_executor(m_strand,
            [self](const boost::system::error_code& ec, std::size_t /*n*/) {
                if (ec) { self->fail(ec.message()); return; }
                self->m_writeQueue.pop_front();
                if (self->m_writeQueue.empty()) {
                    self->m_writing = false;
                } else {
                    self->doWrite();
                }
            }));
}

template <typename Stream>
void RpcConnection<Stream>::closeStream()
{
    boost::system::error_code ignore;
    try {
        // lowest_layer() works for plain asio::ip::tcp::socket (returns
        // itself) and for asio::ssl::stream (returns the underlying TCP
        // socket). Closing the lowest layer tears the stack down cleanly
        // without needing protocol-specific shutdown sequences.
        m_stream.lowest_layer().close(ignore);
    } catch (...) {}
}

template <typename Stream>
void RpcConnection<Stream>::closeStreamOnStrand()
{
    // Asio sockets are NOT safe for concurrent use ("Shared objects:
    // Unsafe"), and close() is no exception: it runs
    // cleanup_descriptor_data(), which nulls the reactor's per-descriptor
    // state. Every other touch of m_stream in this class is serialized on
    // m_strand — start()/writeFrame() post onto it, doRead()/doWrite()
    // complete through bind_executor(m_strand, …). A strand serializes
    // *handlers*; a raw call made from outside it is not covered.
    //
    // fail() is reached from both sides: from the io thread (a read/write
    // handler that saw an error, already inside the strand) and from an
    // arbitrary caller thread (stop(), ~PlainTransportConnection,
    // RpcServer::stop()). Closing on the caller's thread let close() run
    // concurrently with an in-flight doWrite() initiating async_write on
    // the io thread, and the reactor dereferenced the descriptor state the
    // close had just nulled → SIGSEGV inside
    // reactive_socket_service_base::start_op().
    //
    // dispatch() (not post()) is deliberate: when fail() is already running
    // inside the strand it invokes closeStream() inline, so the io-thread
    // error path keeps its current synchronous behaviour and cannot
    // deadlock on itself. From any other thread it queues onto the strand
    // and returns immediately — never blocking, so teardown cannot hang.
    //
    // The lambda keeps a shared_ptr to this connection, so a close queued
    // from a destructor still finds a live object. Should the io_context be
    // stopped before the queued close runs, the socket is still closed when
    // the connection (and with it m_stream) is destroyed.
    std::shared_ptr<RpcConnection<Stream>> self;
    try { self = sharedSelf(); } catch (...) {}
    if (!self) {
        // No owning shared_ptr — the object is mid-destruction, so no other
        // thread can still be holding it to run a stream operation.
        closeStream();
        return;
    }
    boost::asio::dispatch(m_strand, [self] { self->closeStream(); });
}

} // namespace logos::plain

#endif // LOGOS_PLAIN_RPC_CONNECTION_H
