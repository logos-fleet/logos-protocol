#ifndef LOGOS_WASM_TOKEN_STORE_H
#define LOGOS_WASM_TOKEN_STORE_H

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace logos::wasm {

// -----------------------------------------------------------------------------
// THE WASM IMAGE'S TOKEN STORE.
//
// TokenManager, minus Qt, minus everything a Wasm host cannot have and does not
// need. It is not a port and must not become one: what the generated module glue
// reaches for is five lp_* symbols, and this is the smallest object that answers
// them while keeping the one rule authorization depends on.
//
// THE RULE: the two directions are TWO KEY NAMESPACES and neither can read the
// other's.
//
//   OUTBOUND  "what this image presents when it CALLS <name>"
//             written by lp_token_save, read by lp_token_get / lp_token_keys.
//   INBOUND   "what <name> may present when it CALLS this image"
//             written by lp_token_save_inbound, read only by the image's own
//             validator.
//
// A flat map would satisfy every call site and quietly let a grant one way be a
// grant the other — the direction collision that split the doors in the first
// place (see logos_module_impl.h's two `accept_token` entries). Two `std::map`s
// rather than TokenManager's one-map-with-a-reserved-prefix scheme, because
// there is no ABI here to freeze and separate members cannot be confused; the
// reserved character is still refused as a KEY, so a `caller` named over RPC
// cannot address a name it does not own if the two ever meet across a wire.
//
// PER IMAGE, and structurally so. A wasm module instance owns its linear memory
// and shares none of it: two Wasm hosts in two webviews have two of these and
// cannot name each other's. That is acceptance criterion 3 of slice 26, and it
// holds because of where this lives rather than because of anything it does.
//
// STILL LOCKED. Single-threaded is the shipping configuration, not a
// guarantee — this same subset builds natively (which is how it is tested) and
// emscripten's pthread mode is a link flag away — so the mutex stays.
// -----------------------------------------------------------------------------
class WasmTokenStore {
public:
    static WasmTokenStore& instance();

    // The reserved namespace character, spelled the same as TokenManager's:
    // U+0001. A key containing it is refused at both doors.
    static constexpr char kNamespaceChar = '\x01';
    static bool isReservedKey(const std::string& key);

    // OUTBOUND. Refuses an empty or reserved name; an empty VALUE is allowed
    // and means "forget it", mirroring what an empty read answers.
    bool saveOutbound(const std::string& moduleName, const std::string& token);

    // Empty when absent — the lp_ door turns that into NULL.
    std::string outbound(const std::string& moduleName) const;

    std::vector<std::string> outboundKeys() const;

    // INBOUND. Refuses an empty name, an empty token, and a reserved name.
    bool saveInbound(const std::string& caller, const std::string& token);

    // The one reader of the inbound half. Compares rather than vends: nothing
    // outside this image has any business learning what a caller must present,
    // and there is no lp_* function that could ask.
    bool inboundMatches(const std::string& caller, const std::string& token) const;

    // Test seam. There is exactly one store per image, so a suite that runs
    // more than one case in a process has to be able to empty it.
    void clear();

private:
    WasmTokenStore() = default;

    mutable std::mutex m_mutex;
    std::map<std::string, std::string> m_outbound;
    std::map<std::string, std::string> m_inbound;
};

} // namespace logos::wasm

#endif // LOGOS_WASM_TOKEN_STORE_H
