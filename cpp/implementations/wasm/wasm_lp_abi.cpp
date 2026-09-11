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
// return codes, the same refusals, over WasmTokenStore. It is deliberately NOT
// the whole of logos_protocol.h — a wasm module that tried to lp_client_create()
// would fail to LINK, naming the symbol, which is the honest answer for an image
// with no transport it could dial. When a Wasm module gains outbound calls they
// arrive over the web transport the host already speaks, through a new door,
// not by growing this file into a second copy of logos_protocol.cpp.
//
// WHAT IS DUPLICATED HERE AND WHY THAT IS RIGHT: the direction rule, the
// reserved-key refusal and the token-registry carve-out are restated rather
// than shared. They cannot be shared — the only implementation of them lives on
// the Qt side of a wall wasm cannot cross — and they are pinned against the
// Qt version's behaviour case by case in tests/protocol/test_wasm_token_store.cpp,
// which is the coupling that has teeth. A change to lp_token_save_inbound's
// semantics owes an edit here and a case there.

#include "logos_protocol.h"
#include "wasm_token_store.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

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
