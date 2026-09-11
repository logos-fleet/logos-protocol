// THE WASM IMAGE'S TOKEN STORE, and the lp_* doors that write it.
//
// A Wasm host has no Qt in it, so it has no TokenManager: TokenManager is a
// QObject over QHash/QMutex and porting it would mean porting Qt. What the
// generated module glue actually reaches for is five symbols, and this suite
// pins that the wasm build's answer to them keeps TokenManager's ONE rule that
// authorization depends on — the two directions are two key namespaces and
// neither can read the other's.
//
// It links `logos_protocol_wasm` and nothing else (see
// tests/protocol/CMakeLists.txt), which is the second thing it proves: the
// subset compiled for wasm32 is closed. A stray include of a Qt or Boost header
// in that source list fails THIS link, natively, in seconds — long before the
// emcc build in nix/wasm.nix would have to say so.
//
// PER-IMAGE, which is acceptance criterion 3 of slice 26 ("two Wasm hosts in two
// webviews cannot see each other's tokens"). It is structural rather than
// enforced: the store is a file-static in the wasm module's own linear memory,
// and two instantiations of a wasm module share nothing. There is nothing here
// to test for that beyond the fact that no path in this file can name another
// image's store — the tests below reset the one store between cases precisely
// because a single native process has exactly one.

#include "logos_protocol.h"
#include "wasm_token_store.h"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <string>

namespace {

// A char* the lp_* ABI hands back, freed the way that ABI says to.
std::string takeString(char* s)
{
    if (!s) return {};
    std::string out(s);
    lp_string_free(s);
    return out;
}

class WasmTokenStoreTest : public ::testing::Test {
protected:
    void SetUp() override { logos::wasm::WasmTokenStore::instance().clear(); }
    void TearDown() override
    {
        logos::wasm::WasmTokenStore::instance().clear();
        lp_grant_host_services("[]");
    }
};

TEST_F(WasmTokenStoreTest, AnOutboundTokenIsReadBackByName)
{
    EXPECT_EQ(LP_OK, lp_token_save("chat_module", "tok-out"));
    EXPECT_EQ("tok-out", takeString(lp_token_get("chat_module")));
}

TEST_F(WasmTokenStoreTest, AnAbsentOutboundTokenIsNullRatherThanEmpty)
{
    EXPECT_EQ(nullptr, lp_token_get("chat_module"));
}

// THE RULE THE WHOLE FILE EXISTS FOR. A token filed as "chat_module may call
// us" must never come back as "here is what we present when we call
// chat_module": that is the direction collision the two doors were split to
// stop, and a flat map would pass every other test here.
TEST_F(WasmTokenStoreTest, TheInboundDoorIsNotReadableThroughTheOutboundOne)
{
    EXPECT_EQ(LP_OK, lp_token_save_inbound("chat_module", "tok-in"));
    EXPECT_EQ(nullptr, lp_token_get("chat_module"));
}

TEST_F(WasmTokenStoreTest, TheTwoDirectionsHoldDifferentValuesForOneName)
{
    ASSERT_EQ(LP_OK, lp_token_save("chat_module", "tok-out"));
    ASSERT_EQ(LP_OK, lp_token_save_inbound("chat_module", "tok-in"));

    EXPECT_EQ("tok-out", takeString(lp_token_get("chat_module")));
    EXPECT_TRUE(logos::wasm::WasmTokenStore::instance()
                    .inboundMatches("chat_module", "tok-in"));
    EXPECT_FALSE(logos::wasm::WasmTokenStore::instance()
                     .inboundMatches("chat_module", "tok-out"));
}

TEST_F(WasmTokenStoreTest, NullArgumentsAreRefusedAtBothDoors)
{
    EXPECT_EQ(LP_ERR_INVALID_ARG, lp_token_save(nullptr, "t"));
    EXPECT_EQ(LP_ERR_INVALID_ARG, lp_token_save("m", nullptr));
    EXPECT_EQ(LP_ERR_INVALID_ARG, lp_token_save_inbound(nullptr, "t"));
    EXPECT_EQ(LP_ERR_INVALID_ARG, lp_token_save_inbound("m", nullptr));
    EXPECT_EQ(nullptr, lp_token_get(nullptr));
}

// `caller` arrives over RPC, named by capability_module, so it must not be able
// to address a key it does not own. The reserved character is the one
// TokenManager uses to build its inbound keys.
TEST_F(WasmTokenStoreTest, AReservedKeyIsRefusedAtBothDoors)
{
    const std::string sneaky = std::string("\x01") + "in" + std::string("\x01") + "core";
    EXPECT_EQ(LP_ERR_INVALID_ARG, lp_token_save(sneaky.c_str(), "t"));
    EXPECT_EQ(LP_ERR_INVALID_ARG, lp_token_save_inbound(sneaky.c_str(), "t"));
}

TEST_F(WasmTokenStoreTest, AnEmptyNameOrTokenIsRefusedOnTheInboundDoor)
{
    EXPECT_EQ(LP_ERR_INVALID_ARG, lp_token_save_inbound("", "t"));
    EXPECT_EQ(LP_ERR_INVALID_ARG, lp_token_save_inbound("m", ""));
}

// ── the trust-root surface ──────────────────────────────────────────────────

TEST_F(WasmTokenStoreTest, TokenKeysIsShutWithoutTheGrant)
{
    ASSERT_EQ(LP_OK, lp_token_save("chat_module", "tok"));
    EXPECT_EQ(nullptr, lp_token_keys());
}

TEST_F(WasmTokenStoreTest, TokenKeysListsTheOutboundHalfWithTheGrant)
{
    ASSERT_EQ(LP_OK, lp_token_save("chat_module", "tok"));
    ASSERT_EQ(LP_OK, lp_token_save_inbound("waku_module", "tok-in"));
    ASSERT_EQ(LP_OK, lp_grant_host_services(R"(["token_registry"])"));

    const nlohmann::json keys = nlohmann::json::parse(takeString(lp_token_keys()));
    ASSERT_TRUE(keys.is_array());
    // waku_module is in there BECAUSE of the carve-out below, not because
    // lp_token_keys reads the inbound half — chat_module is the control.
    EXPECT_NE(keys.end(), std::find(keys.begin(), keys.end(), "chat_module"));
}

// THE TOKEN-REGISTRY CARVE-OUT, carried over verbatim in meaning from
// lp_token_save_inbound's Qt implementation. Without it a wasm image holding
// the registry role empties its roster and refuses every requestModule.
TEST_F(WasmTokenStoreTest, TheRegistryGrantMakesTheInboundDoorWriteOutboundToo)
{
    ASSERT_EQ(LP_OK, lp_grant_host_services(R"(["token_registry"])"));
    ASSERT_EQ(LP_OK, lp_token_save_inbound("chat_module", "tok"));
    EXPECT_EQ("tok", takeString(lp_token_get("chat_module")));
}

TEST_F(WasmTokenStoreTest, WithoutTheGrantTheInboundDoorWritesOnlyInbound)
{
    ASSERT_EQ(LP_OK, lp_token_save_inbound("chat_module", "tok"));
    EXPECT_EQ(nullptr, lp_token_get("chat_module"));
}

TEST_F(WasmTokenStoreTest, AnUnknownHostServiceIsRejectedWholesale)
{
    EXPECT_EQ(LP_ERR_INVALID_ARG,
              lp_grant_host_services(R"(["token_registry","teleport"])"));
    // ...and the grant that WOULD have been made is not in force.
    ASSERT_EQ(LP_OK, lp_token_save("chat_module", "tok"));
    EXPECT_EQ(nullptr, lp_token_keys());
}

TEST_F(WasmTokenStoreTest, AGrantIsReplacedRatherThanAccumulated)
{
    ASSERT_EQ(LP_OK, lp_grant_host_services(R"(["token_registry"])"));
    ASSERT_EQ(LP_OK, lp_grant_host_services(R"(["token_delivery"])"));
    ASSERT_EQ(LP_OK, lp_token_save("chat_module", "tok"));
    EXPECT_EQ(nullptr, lp_token_keys());
}

TEST_F(WasmTokenStoreTest, MalformedGrantJsonIsRefused)
{
    EXPECT_EQ(LP_ERR_INVALID_ARG, lp_grant_host_services("not json"));
    EXPECT_EQ(LP_ERR_INVALID_ARG, lp_grant_host_services(R"({"a":1})"));
}

// The version handshake a no-Qt host negotiates with, answered by the same
// image the module is linked into.
TEST_F(WasmTokenStoreTest, TheImageReportsTheProtocolVersionItWasBuiltFrom)
{
    EXPECT_STREQ(LOGOS_PROTOCOL_VERSION_STRING, lp_protocol_version());
    EXPECT_EQ(LOGOS_PROTOCOL_VERSION_MAJOR, lp_protocol_abi_major());
}

} // namespace
