// THE lp_* SUBSET A WASM IMAGE OWES ITS MODULE.
//
// A Bare module's generated cdylib glue is the same code on every platform, and
// it calls lp_token_save, lp_token_save_inbound, lp_grant_host_services (and,
// for a trust root, lp_token_get / lp_token_keys) directly — no dlsym, no null
// check. On a desktop host those resolve into the full logos-protocol image; a
// Wasm host has no such image, because logos_protocol.cpp is written over
// TokenManager, QVariant and the Qt-boundary types and none of that reaches
// wasm32.
//
// So this file is what a wasm image links instead: the same C ABI, the same
// return codes, the same refusals, over WasmTokenStore.
//
// AND, SINCE THE OUTBOUND DOOR, the consumer stack too — lp_client_create,
// lp_client_destroy and lp_invoke_async, over the web transport the host
// already speaks (wasm_outbound_door.h) rather than by growing this file into a
// second copy of logos_protocol.cpp. There is no LogosAPI in here, no
// TokenManager and no QtRO replica: a client is a target name, an origin name
// and a lifetime, and the call is a CallMessage on the image's one channel.
//
// lp_invoke — the SYNCHRONOUS twin — IS STILL UNDEFINED, ON PURPOSE. See the
// block above lp_invoke_async for the whole argument; the short form is that a
// Worker is one event loop, this image is built without ASYNCIFY (ADR 0004),
// and a call that blocked for its reply would deadlock the loop that delivers
// it. Defining it as an always-error stub would let such a module link and fail
// on a phone; leaving it undefined makes it fail to BUILD, naming the symbol,
// which is where an author can still do something about it.
//
// WHAT IS DUPLICATED HERE AND WHY THAT IS RIGHT: the direction rule, the
// reserved-key refusal and the token-registry carve-out are restated rather
// than shared. They cannot be shared — the only implementation of them lives on
// the Qt side of a wall wasm cannot cross — and they are pinned against the
// Qt version's behaviour case by case in tests/protocol/test_wasm_token_store.cpp,
// which is the coupling that has teeth. A change to lp_token_save_inbound's
// semantics owes an edit here and a case there.

#include "logos_protocol.h"
#include "wasm_outbound_door.h"
#include "wasm_token_store.h"

#include "json_mapping.h"
#include "logos_call_error.h"
#include "rpc_message.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {

using logos::wasm::WasmTokenStore;

// Same ownership contract as lpStrdup in logos_protocol.cpp: malloc'd, freed by
// the caller through lp_string_free.
char* lpStrdup(const std::string& s)
{
    char* out = static_cast<char*>(std::malloc(s.size() + 1));
    if (!out) return nullptr;
    std::memcpy(out, s.c_str(), s.size() + 1);
    return out;
}

// The host-service grant, with the same bits and the same names as the Qt
// image. Per-image and fail-closed, exactly as there — and here the "image" is
// the wasm instance's linear memory, which is the strongest form of that the
// stack has.
enum HostService : unsigned {
    ServiceTokenRegistry = 1u << 0,  // "token_registry" — lp_token_keys
    ServiceTokenDelivery = 1u << 1,  // "token_delivery" — lp_inform_module_token_to
};

std::mutex g_hostServicesMutex;
unsigned g_hostServices = 0;

bool hostServiceBit(const std::string& name, unsigned& bit)
{
    if (name == "token_registry") { bit = ServiceTokenRegistry; return true; }
    if (name == "token_delivery") { bit = ServiceTokenDelivery; return true; }
    return false;
}

bool hostServiceGranted(unsigned service)
{
    std::lock_guard<std::mutex> lock(g_hostServicesMutex);
    return (g_hostServices & service) != 0;
}

} // namespace

extern "C" {

/* ---------------------------------------------------------------- version */

const char* lp_protocol_version(void)
{
    return LOGOS_PROTOCOL_VERSION_STRING;
}

int lp_protocol_abi_major(void)
{
    return LOGOS_PROTOCOL_VERSION_MAJOR;
}

/* ----------------------------------------------------------------- memory */

void lp_string_free(char* s)
{
    std::free(s);
}

/* ----------------------------------------------------------------- tokens */

char* lp_token_get(const char* module_name)
{
    if (!module_name) return nullptr;
    const std::string token = WasmTokenStore::instance().outbound(module_name);
    if (token.empty()) return nullptr;
    return lpStrdup(token);
}

int lp_token_save(const char* module_name, const char* token)
{
    if (!module_name || !token) return LP_ERR_INVALID_ARG;
    return WasmTokenStore::instance().saveOutbound(module_name, token)
        ? LP_OK
        : LP_ERR_INVALID_ARG;
}

int lp_token_save_inbound(const char* caller, const char* token)
{
    if (!caller || !token) return LP_ERR_INVALID_ARG;
    if (!WasmTokenStore::instance().saveInbound(caller, token))
        return LP_ERR_INVALID_ARG;

    // THE TOKEN-REGISTRY CARVE-OUT. informModuleToken means opposite things
    // depending on who receives it: to an ordinary provider "this caller may
    // call you", to the image holding the registry "here is module X's token,
    // present it when you call X". The grant is the declaration of that role, so
    // the grant decides — see lp_token_save_inbound in logos_protocol.h for the
    // full argument and for the measured consequence of leaving it out.
    if (hostServiceGranted(ServiceTokenRegistry))
        WasmTokenStore::instance().saveOutbound(caller, token);

    return LP_OK;
}

char* lp_token_keys(void)
{
    // Gate first, before any state is read: an image without the grant gets one
    // answer regardless.
    if (!hostServiceGranted(ServiceTokenRegistry)) return nullptr;

    nlohmann::json out = nlohmann::json::array();
    for (const std::string& key : WasmTokenStore::instance().outboundKeys())
        out.push_back(key);
    return lpStrdup(out.dump());
}

/* --------------------------------------------------------- host services */

int lp_grant_host_services(const char* services_json)
{
    unsigned granted = 0;
    if (services_json && *services_json) {
        nlohmann::json parsed = nlohmann::json::parse(services_json, nullptr,
                                                      /*allow_exceptions=*/false);
        if (parsed.is_discarded() || !parsed.is_array()) return LP_ERR_INVALID_ARG;
        for (const nlohmann::json& entry : parsed) {
            unsigned bit = 0;
            if (!entry.is_string() || !hostServiceBit(entry.get<std::string>(), bit))
                return LP_ERR_INVALID_ARG;   // rejected wholesale, grant untouched
            granted |= bit;
        }
    }
    std::lock_guard<std::mutex> lock(g_hostServicesMutex);
    g_hostServices = granted;
    return LP_OK;
}

} // extern "C"

namespace {
// ── the outbound door's state ───────────────────────────────────────────────
//
// One connection per image, installed by the host. Held WEAKLY: the host owns
// it (it owns the channel underneath), and a client that outlives the host's
// teardown must fail rather than keep a dead conversation alive.
std::mutex g_outboundMutex;
std::weak_ptr<logos::plain::RpcConnectionBase> g_outbound;

std::shared_ptr<logos::plain::RpcConnectionBase> outboundConnection()
{
    std::lock_guard<std::mutex> lock(g_outboundMutex);
    return g_outbound.lock();
}

// The module every outbound token comes from. Named once: it is the ONE target
// the door may dial without already holding a credential for it, and that
// exception is the bootstrap.
constexpr const char* kCapabilityModule = "capability_module";

// Every failure this door reports, in one place. The payload is the canonical
// {code, message, origin} object, byte-identical in shape to makeErrorJson in
// logos_protocol.cpp — lp_result_cb's contract is the same on both sides of the
// wall, so a Rust or C++ consumer decodes one shape. Nothing else in this file
// calls `cb` with ok == 0.
void failCall(lp_result_cb cb, void* userData, const logos::CallError& err)
{
    nlohmann::json e;
    e["code"] = err.code;
    e["message"] = err.message;
    e["origin"] = err.origin;
    const std::string json = e.dump();
    cb(0, json.c_str(), userData);
}

// A client's liveness cell. lp_client_destroy promises that no further callback
// fires, and the wire cannot be un-sent: the pending handler stays registered
// on the connection and simply drops its answer. Same shape, same reason, as
// CbGuard on the Qt side.
using AliveCell = std::shared_ptr<std::atomic<bool>>;

} // namespace

// The handle the C ABI hands out. Defined here rather than in the header for
// the same reason the Qt image defines its own: `lp_client` is an opaque tag to
// everyone outside the implementation that vends it.
struct lp_client {
    std::string target;
    std::string origin;
    AliveCell   alive = std::make_shared<std::atomic<bool>>(true);
};

namespace {

// Put one Call on the wire and route its Result to `cb`, exactly once.
void sendOutboundCall(const std::shared_ptr<logos::plain::RpcConnectionBase>& conn,
                      const AliveCell& alive,
                      const std::string& target, const std::string& method,
                      const std::string& authToken,
                      std::vector<logos::plain::RpcValue> args,
                      lp_result_cb cb, void* userData)
{
    logos::plain::CallMessage call;
    call.id = conn->nextId();
    call.object = target;
    call.method = method;
    call.args = std::move(args);
    // THE CREDENTIAL, AND NOTHING ELSE THIS IMAGE COULD ASSERT. A wasm image
    // mints nothing and holds no other module's secrets (ADR 0005); what it
    // presents is the token the core or capability_module granted it for THIS
    // target, read out of its own store — the same store lp_token_get reads and
    // the same one TokenManager is on a desktop host.
    call.authToken = authToken;

    conn->sendCallAsync(std::move(call), [alive, cb, userData, target](
                                             logos::plain::ResultMessage res) {
        // A destroyed client answers nothing. The handler cannot be withdrawn
        // from the peer (cancelPending is by id and the id is gone with the
        // call), so the drop happens here.
        if (!alive->load()) return;
        if (!res.ok) {
            failCall(cb, userData,
                     logos::callErrorFromWire(target, res.errCode, res.err));
            return;
        }
        const std::string json = logos::plain::rpcValueToJson(res.value).dump();
        cb(1, json.c_str(), userData);
    });
}

// ── no credential for this target: ask for one, do not call without it ──────
//
// The same handshake a native client runs transparently on its first call
// (logos_api_client.cpp's requestModule flow), spelled out here because a wasm
// image has no LogosAPI to run it. ONE DIFFERENCE, deliberate: the native
// client forwards the call tokenless when the handshake yields nothing, and
// this one refuses. A relay that forwarded a call it was not granted is a hole
// that ends at whatever the far side happens to check, and on this wire the far
// side is a container whose job is to relay.
void requestTokenThenSend(const std::shared_ptr<logos::plain::RpcConnectionBase>& conn,
                          const AliveCell& alive,
                          const std::string& target, const std::string& origin,
                          const std::string& method,
                          std::vector<logos::plain::RpcValue> args,
                          lp_result_cb cb, void* userData)
{
    logos::plain::CallMessage req;
    req.id = conn->nextId();
    req.object = kCapabilityModule;
    req.method = "requestModule";
    req.authToken = WasmTokenStore::instance().outbound(kCapabilityModule);
    // `fromModuleName` is leftover ABI — capability_module ignores it and takes
    // the caller from the dispatch — but it is sent honestly anyway, because a
    // log line naming the wrong module is a debugging cost paid much later.
    req.args.push_back(logos::plain::RpcValue(origin));
    req.args.push_back(logos::plain::RpcValue(target));

    // The call the grant is for, parked until there is a credential to stamp on
    // it. A shared_ptr because a ResultHandler is copyable and a vector of args
    // is not free to copy.
    auto pending = std::make_shared<std::vector<logos::plain::RpcValue>>(std::move(args));

    conn->sendCallAsync(std::move(req), [conn, alive, target, method, pending,
                                         cb, userData](
                                            logos::plain::ResultMessage res) {
        if (!alive->load()) return;

        std::string minted;
        if (res.ok) {
            const nlohmann::json v = logos::plain::rpcValueToJson(res.value);
            if (v.is_string()) minted = v.get<std::string>();
        }
        if (minted.empty()) {
            failCall(cb, userData, logos::CallError{
                "unauthorized",
                "capability_module granted this image no token for '" + target
                    + "', so the call was not forwarded",
                target });
            return;
        }

        // ONCE PER TARGET, not once per call: the grant goes into the image's
        // own store, which is what lp_invoke_async's fast path reads.
        WasmTokenStore::instance().saveOutbound(target, minted);
        sendOutboundCall(conn, alive, target, method, minted,
                         std::move(*pending), cb, userData);
    });
}

} // namespace

extern "C" {

/* ------------------------------------------------- consumer: the outbound door
 *
 * WHY THERE IS NO lp_invoke HERE, stated where a reader looking for it will be.
 *
 * A Worker is ONE event loop. A reply to an outbound call arrives as a message
 * on it, and this image is built without ASYNCIFY (ADR 0004 — it roughly
 * doubles the image and slows every dispatch), so a call that blocked waiting
 * for its reply would deadlock the very loop that was going to deliver it. The
 * synchronous spelling is not missing; the target forbids it. logos-module-
 * builder's logos_web_module_call.h states the identical rule for the `ui_qml`
 * twin of this door, and the reasoning does not change with the language.
 *
 * SO IT IS ABSENT RATHER THAN STUBBED, and that was the decision to make. A
 * stub that always returned LP_ERR_UNAVAILABLE would let a module calling the
 * sync twin LINK and then fail on a phone, at the one moment nobody can act on
 * it. Undefined, wasm-ld names the symbol and the build stops. The generated
 * Rust client makes that diagnosis better still: logos-rust-sdk compiles no
 * synchronous path on emscripten at all (`cfg(not(target_os = "emscripten"))`),
 * and lidl-gen emits each sync method under the same gate, so a `codegen.rust`
 * module that calls `dep.add(1, 2)` for `web` fails to COMPILE naming `add` and
 * is told to call `add_async`. A C++ universal module hears it one step later,
 * from the linker, naming lp_invoke.
 */

lp_client* lp_client_create(const char* target_module,
                            const char* origin_module,
                            const char* /*target_transport_json*/,
                            const char* /*capability_transport_json*/)
{
    if (!target_module || !*target_module) return nullptr;

    // THE TRANSPORT ARGUMENTS ARE IGNORED, and they have to be. The frozen
    // signature lets a desktop caller pick a wire; a wasm image HAS one wire,
    // the channel its host bound, and there is no second one it could be asked
    // for. Accepted and dropped rather than refused, so the same generated
    // consumer code compiles for both targets — it passes NULL in practice
    // (see logos-rust-sdk's shared_client and logos-cpp-sdk's LpClient::ensure).
    auto* client = new lp_client;
    client->target = target_module;
    client->origin = origin_module ? origin_module : "";
    return client;
}

void lp_client_destroy(lp_client* client)
{
    if (!client) return;
    // Announce the death BEFORE freeing: a reply already in flight reads this
    // cell (it holds its own share of it) and drops itself.
    client->alive->store(false);
    delete client;
}

int lp_invoke_async(lp_client* client,
                    const char* method,
                    const char* args_json,
                    int /*timeout_ms*/,
                    lp_result_cb cb,
                    void* user_data)
{
    if (!client || !method || !*method || !cb) return LP_ERR_INVALID_ARG;

    std::vector<logos::plain::RpcValue> args;
    if (args_json && *args_json) {
        const nlohmann::json parsed =
            nlohmann::json::parse(args_json, nullptr, /*allow_exceptions=*/false);
        if (parsed.is_discarded() || !parsed.is_array()) return LP_ERR_INVALID_ARG;
        for (const nlohmann::json& a : parsed)
            args.push_back(logos::plain::jsonToRpcValue(a));
    }

    // THE DEADLINE IS NOT ENFORCED HERE, and that is recorded rather than
    // papered over. This subset has no timer: emscripten's would make the
    // shipping build differ from the one the tests link (they are the same
    // source list, natively compiled — tests/protocol/CMakeLists.txt), and a
    // thread is the one thing a Worker does not have. What answers every
    // pending call today is the container, which replies on all of its paths
    // (WebCallRouter::onCall). The uncovered case is the one web_rpc_connection.h
    // already records for this transport: a channel that CLOSES does not fail
    // its peer's pending calls. Closing that belongs with the host that owns
    // the webview's lifecycle, and it closes both gaps at once.

    // Read off the handle once: everything after this point may outlive it.
    const std::string target = client->target;
    const AliveCell alive = client->alive;

    std::shared_ptr<logos::plain::RpcConnectionBase> conn = outboundConnection();
    if (!conn || !conn->isOpen()) {
        // A REFUSAL IS DELIVERED THROUGH `cb`, INLINE, before this returns —
        // and a LP_OK is still the right return, because the call WAS
        // dispatched as far as this door is concerned and the outcome is the
        // callback's to carry (lp_invoke_async's contract: "a LP_OK return
        // means dispatched, never succeeded"). Inline rather than posted
        // because there is nothing to post to: see the deadline note above.
        // Every consumer in the tree tolerates it — logos-rust-sdk's
        // call_json_async_inner does nothing after the ABI call returns, and
        // logos-cpp-sdk's invokeAsync likewise.
        failCall(cb, user_data, logos::callErrorTransport(
            target, "this wasm image has no host channel: its host installed no "
                    "connection, or the one it installed is closed"));
        return LP_OK;
    }

    const std::string held = WasmTokenStore::instance().outbound(target);
    if (!held.empty() || target == kCapabilityModule) {
        // capability_module is dialled WITHOUT a credential when this image
        // holds none, and it is the only name that may be. It is where a
        // credential comes from; refusing the bootstrap would refuse everything
        // downstream of it. It is also safe to do so: the target authorizes on
        // its own side and names the caller from the dispatch, not from the
        // token (capability_module_impl.cpp's requestModule).
        sendOutboundCall(conn, alive, target, method, held, std::move(args),
                         cb, user_data);
        return LP_OK;
    }

    requestTokenThenSend(conn, alive, target, client->origin, method,
                         std::move(args), cb, user_data);
    return LP_OK;
}

} // extern "C"

namespace logos::wasm {

void setOutboundConnection(const std::shared_ptr<logos::plain::RpcConnectionBase>& connection)
{
    std::lock_guard<std::mutex> lock(g_outboundMutex);
    g_outbound = connection;
}

bool outboundDoorIsOpen()
{
    const std::shared_ptr<logos::plain::RpcConnectionBase> conn = outboundConnection();
    return conn && conn->isOpen();
}

} // namespace logos::wasm
