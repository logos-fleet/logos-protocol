// THE OUTBOUND DOOR — a wasm image CALLING a dependency.
//
// The inbound half of the wasm ABI is pinned next door in
// test_wasm_token_store.cpp. This is the other direction: lp_client_create /
// lp_client_destroy / lp_invoke_async over the one connection the host installs
// (wasm_outbound_door.h), which is what a `codegen.rust` module's generated
// `<method>_async` bottoms out in.
//
// LINKED NATIVELY AND ALONE, for the same reason and against the same archive:
// `logos_protocol_wasm` is the exact source list emcc compiles, so a regression
// in the door shows up here in seconds rather than in an emcc build three repos
// downstream. What is native about it is only the clock — the channel, the
// codec, RpcPeer and the frames are the shipping ones.
//
// THE FAR END IS A REAL PEER, not a mock of one. `makeInMemoryChannelPair` is
// what a webview bridge is minus the webview, and the stub on the other side
// answers as an IncomingCallHandler, so every assertion below is about a frame
// that went through JsonCodec and came back.
//
// WHAT IT PINS, in order:
//   * a reply reaches the callback, with the value the target returned;
//   * the outbound frame carries the token THIS IMAGE holds for that target;
//   * a target this image has no token for is not called at all — the door
//     asks capability_module for one first, and a refusal there refuses the
//     call rather than forwarding it naked;
//   * the minted token is remembered, so the handshake is once per target;
//   * with no channel installed, a call is refused rather than dropped.

#include "logos_protocol.h"
#include "in_memory_channel.h"
#include "json_mapping.h"
#include "rpc_message.h"
#include "wasm_outbound_door.h"
#include "wasm_token_store.h"
#include "web_rpc_connection.h"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using logos::plain::CallMessage;
using logos::plain::MethodsMessage;
using logos::plain::ResultMessage;
using logos::plain::SubscribeMessage;
using logos::plain::TokenMessage;
using logos::plain::UnsubscribeMessage;

// What the container is to a wasm image, for the purposes of this file: it
// answers calls and it remembers what it was asked.
class StubContainer : public logos::plain::IncomingCallHandler {
public:
    struct Seen {
        std::string object;
        std::string method;
        std::string authToken;
        nlohmann::json args;
    };

    void onCall(const CallMessage& req, CallReply reply) override
    {
        Seen s;
        s.object = req.object;
        s.method = req.method;
        s.authToken = req.authToken;
        s.args = nlohmann::json::array();
        for (const auto& a : req.args) s.args.push_back(logos::plain::rpcValueToJson(a));
        {
            std::lock_guard<std::mutex> g(m_mu);
            m_seen.push_back(s);
        }
        m_cv.notify_all();

        ResultMessage res;
        res.id = req.id;
        if (req.object == "capability_module" && req.method == "requestModule") {
            res.ok = true;
            res.value = logos::plain::jsonToRpcValue(nlohmann::json(m_mintedToken));
            reply(std::move(res));
            return;
        }
        if (m_refuseTarget) {
            res.ok = false;
            res.err = "no such module";
            res.errCode = "MODULE_NOT_LOADED";
            reply(std::move(res));
            return;
        }
        res.ok = true;
        res.value = logos::plain::jsonToRpcValue(m_answer);
        reply(std::move(res));
    }

    void onMethods(const MethodsMessage&, MethodsReply) override { }
    void onSubscribe(const SubscribeMessage&, EventSink, const void*) override { }
    void onUnsubscribe(const UnsubscribeMessage&, const void*) override { }
    void onConnectionClosed(const void*) override { }
    void onToken(const TokenMessage&) override { }

    void mintToken(std::string t) { m_mintedToken = std::move(t); }
    void answerWith(nlohmann::json v) { m_answer = std::move(v); }
    void refuseTarget() { m_refuseTarget = true; }

    std::vector<Seen> seen() const
    {
        std::lock_guard<std::mutex> g(m_mu);
        return m_seen;
    }

private:
    mutable std::mutex m_mu;
    std::condition_variable m_cv;
    std::vector<Seen> m_seen;
    std::string m_mintedToken;          // "" == capability_module refuses
    nlohmann::json m_answer = 42;
    bool m_refuseTarget = false;
};

// One lp_invoke_async outcome, captured from the C callback.
struct Outcome {
    bool fired = false;
    int ok = -1;
    std::string json;
};

void captureCb(int ok, const char* json, void* user_data)
{
    auto* out = static_cast<Outcome*>(user_data);
    out->fired = true;
    out->ok = ok;
    out->json = json ? json : "";
}

// The callback lands on the channel's delivery thread, so a test waits for it.
bool waitFor(const Outcome& out, int ms = 2000)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!out.fired && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    return out.fired;
}

class WasmOutboundDoorTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        logos::wasm::WasmTokenStore::instance().clear();

        auto pair = logos::web::makeInMemoryChannelPair();
        // The IMAGE's end: a pure consumer, exactly as the wasm host's own
        // connection is for outbound traffic.
        m_imageConn = std::make_shared<logos::web::WebRpcConnection>(pair.first, nullptr);
        m_containerConn = std::make_shared<logos::web::WebRpcConnection>(pair.second, &m_container);
        m_imageConn->start();
        m_containerConn->start();
        logos::wasm::setOutboundConnection(m_imageConn);
    }

    void TearDown() override
    {
        logos::wasm::setOutboundConnection(nullptr);
        if (m_containerConn) m_containerConn->stop();
        if (m_imageConn) m_imageConn->stop();
        m_containerConn.reset();
        m_imageConn.reset();
        logos::wasm::WasmTokenStore::instance().clear();
    }

    StubContainer m_container;
    std::shared_ptr<logos::web::WebRpcConnection> m_imageConn;
    std::shared_ptr<logos::web::WebRpcConnection> m_containerConn;
};

TEST_F(WasmOutboundDoorTest, TheDoorIsOpenOnceTheHostInstallsItsConnection)
{
    EXPECT_TRUE(logos::wasm::outboundDoorIsOpen());
    logos::wasm::setOutboundConnection(nullptr);
    EXPECT_FALSE(logos::wasm::outboundDoorIsOpen());
}

TEST_F(WasmOutboundDoorTest, ClientCreationRefusesAnUnnamedTarget)
{
    EXPECT_EQ(nullptr, lp_client_create(nullptr, "me", nullptr, nullptr));
    EXPECT_EQ(nullptr, lp_client_create("", "me", nullptr, nullptr));
}

TEST_F(WasmOutboundDoorTest, AReplyLandsInTheCallback)
{
    ASSERT_EQ(LP_OK, lp_token_save("bare_counter", "tok-counter"));
    m_container.answerWith(43);

    lp_client* c = lp_client_create("bare_counter", "bare_relay", nullptr, nullptr);
    ASSERT_NE(nullptr, c);

    Outcome out;
    ASSERT_EQ(LP_OK, lp_invoke_async(c, "increment", "[1]", 0, &captureCb, &out));
    ASSERT_TRUE(waitFor(out));
    EXPECT_EQ(1, out.ok);
    EXPECT_EQ("43", out.json);

    const auto seen = m_container.seen();
    ASSERT_EQ(1u, seen.size());
    EXPECT_EQ("bare_counter", seen[0].object);
    EXPECT_EQ("increment", seen[0].method);
    EXPECT_EQ(nlohmann::json::array({ 1 }), seen[0].args);

    lp_client_destroy(c);
}

TEST_F(WasmOutboundDoorTest, TheOutboundFrameCarriesTheTokenHeldForThatTarget)
{
    ASSERT_EQ(LP_OK, lp_token_save("bare_counter", "tok-counter"));

    lp_client* c = lp_client_create("bare_counter", "bare_relay", nullptr, nullptr);
    ASSERT_NE(nullptr, c);
    Outcome out;
    ASSERT_EQ(LP_OK, lp_invoke_async(c, "current", "[]", 0, &captureCb, &out));
    ASSERT_TRUE(waitFor(out));

    const auto seen = m_container.seen();
    ASSERT_EQ(1u, seen.size());
    EXPECT_EQ("tok-counter", seen[0].authToken);

    lp_client_destroy(c);
}

TEST_F(WasmOutboundDoorTest, WithNoTokenTheDoorAsksCapabilityModuleAndRemembersTheAnswer)
{
    m_container.mintToken("minted-for-counter");

    lp_client* c = lp_client_create("bare_counter", "bare_relay", nullptr, nullptr);
    ASSERT_NE(nullptr, c);
    Outcome out;
    ASSERT_EQ(LP_OK, lp_invoke_async(c, "current", "[]", 0, &captureCb, &out));
    ASSERT_TRUE(waitFor(out));
    EXPECT_EQ(1, out.ok);

    const auto seen = m_container.seen();
    ASSERT_EQ(2u, seen.size());
    EXPECT_EQ("capability_module", seen[0].object);
    EXPECT_EQ("requestModule", seen[0].method);
    EXPECT_EQ("bare_counter", seen[1].object);
    EXPECT_EQ("minted-for-counter", seen[1].authToken);

    // Once per target, not once per call.
    char* held = lp_token_get("bare_counter");
    ASSERT_NE(nullptr, held);
    EXPECT_EQ(std::string("minted-for-counter"), std::string(held));
    lp_string_free(held);

    lp_client_destroy(c);
}

TEST_F(WasmOutboundDoorTest, ATargetThisImageHasNoTokenForIsRefusedNotForwarded)
{
    m_container.mintToken("");   // capability_module says no

    lp_client* c = lp_client_create("bare_counter", "bare_relay", nullptr, nullptr);
    ASSERT_NE(nullptr, c);
    Outcome out;
    ASSERT_EQ(LP_OK, lp_invoke_async(c, "current", "[]", 0, &captureCb, &out));
    ASSERT_TRUE(waitFor(out));

    EXPECT_EQ(0, out.ok);
    const nlohmann::json err = nlohmann::json::parse(out.json, nullptr, false);
    ASSERT_FALSE(err.is_discarded());
    EXPECT_EQ("unauthorized", err.value("code", std::string()));
    EXPECT_EQ("bare_counter", err.value("origin", std::string()));

    // The target itself was never dialled.
    const auto seen = m_container.seen();
    ASSERT_EQ(1u, seen.size());
    EXPECT_EQ("capability_module", seen[0].object);

    lp_client_destroy(c);
}

TEST_F(WasmOutboundDoorTest, AFailedCallReachesTheCallbackAsTheCanonicalErrorObject)
{
    ASSERT_EQ(LP_OK, lp_token_save("bare_counter", "tok-counter"));
    m_container.refuseTarget();

    lp_client* c = lp_client_create("bare_counter", "bare_relay", nullptr, nullptr);
    ASSERT_NE(nullptr, c);
    Outcome out;
    ASSERT_EQ(LP_OK, lp_invoke_async(c, "current", "[]", 0, &captureCb, &out));
    ASSERT_TRUE(waitFor(out));

    EXPECT_EQ(0, out.ok);
    const nlohmann::json err = nlohmann::json::parse(out.json, nullptr, false);
    ASSERT_FALSE(err.is_discarded());
    EXPECT_EQ("object_unavailable", err.value("code", std::string()));

    lp_client_destroy(c);
}

TEST_F(WasmOutboundDoorTest, WithNoChannelACallIsRefusedRatherThanDropped)
{
    ASSERT_EQ(LP_OK, lp_token_save("bare_counter", "tok-counter"));
    logos::wasm::setOutboundConnection(nullptr);

    lp_client* c = lp_client_create("bare_counter", "bare_relay", nullptr, nullptr);
    ASSERT_NE(nullptr, c);
    Outcome out;
    ASSERT_EQ(LP_OK, lp_invoke_async(c, "current", "[]", 0, &captureCb, &out));
    ASSERT_TRUE(out.fired);      // refusals are answered before this returns
    EXPECT_EQ(0, out.ok);
    const nlohmann::json err = nlohmann::json::parse(out.json, nullptr, false);
    ASSERT_FALSE(err.is_discarded());
    EXPECT_EQ("transport_error", err.value("code", std::string()));

    lp_client_destroy(c);
}

TEST_F(WasmOutboundDoorTest, ArgumentValidationFailsSynchronouslyWithoutCallingBack)
{
    lp_client* c = lp_client_create("bare_counter", "bare_relay", nullptr, nullptr);
    ASSERT_NE(nullptr, c);
    Outcome out;
    EXPECT_EQ(LP_ERR_INVALID_ARG, lp_invoke_async(c, "", "[]", 0, &captureCb, &out));
    EXPECT_EQ(LP_ERR_INVALID_ARG, lp_invoke_async(c, "m", "{\"not\":\"an array\"}", 0,
                                                  &captureCb, &out));
    EXPECT_EQ(LP_ERR_INVALID_ARG, lp_invoke_async(nullptr, "m", "[]", 0, &captureCb, &out));
    EXPECT_FALSE(out.fired);
    lp_client_destroy(c);
}

// A destroyed client answers nothing more — the C ABI's promise, and the one
// thing that makes a fire-and-forget call from a temporary safe.
TEST_F(WasmOutboundDoorTest, DestroyingAClientSilencesItsPendingCall)
{
    ASSERT_EQ(LP_OK, lp_token_save("bare_counter", "tok-counter"));
    lp_client* c = lp_client_create("bare_counter", "bare_relay", nullptr, nullptr);
    ASSERT_NE(nullptr, c);
    Outcome out;
    ASSERT_EQ(LP_OK, lp_invoke_async(c, "current", "[]", 0, &captureCb, &out));
    lp_client_destroy(c);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_FALSE(out.fired);
}

} // namespace
