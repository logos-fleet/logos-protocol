#include "wasm_token_store.h"

namespace logos::wasm {

WasmTokenStore& WasmTokenStore::instance()
{
    // Function-local static: the image's one store, constructed on first use and
    // never destroyed before the module's own teardown could still read it.
    static WasmTokenStore store;
    return store;
}

bool WasmTokenStore::isReservedKey(const std::string& key)
{
    return key.find(kNamespaceChar) != std::string::npos;
}

bool WasmTokenStore::saveOutbound(const std::string& moduleName, const std::string& token)
{
    if (moduleName.empty() || isReservedKey(moduleName)) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_outbound[moduleName] = token;
    return true;
}

std::string WasmTokenStore::outbound(const std::string& moduleName) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_outbound.find(moduleName);
    return it == m_outbound.end() ? std::string() : it->second;
}

std::vector<std::string> WasmTokenStore::outboundKeys() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string> keys;
    keys.reserve(m_outbound.size());
    for (const auto& entry : m_outbound) keys.push_back(entry.first);
    return keys;
}

bool WasmTokenStore::saveInbound(const std::string& caller, const std::string& token)
{
    // An empty TOKEN is refused here and allowed on the outbound door, and that
    // asymmetry is TokenManager's: an outbound write with no value is this image
    // dropping its own credential, while an inbound write with no value would be
    // recording that a caller may present nothing.
    if (caller.empty() || token.empty() || isReservedKey(caller)) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_inbound[caller] = token;
    return true;
}

bool WasmTokenStore::inboundMatches(const std::string& caller, const std::string& token) const
{
    if (caller.empty() || token.empty()) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_inbound.find(caller);
    return it != m_inbound.end() && it->second == token;
}

void WasmTokenStore::clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_outbound.clear();
    m_inbound.clear();
}

} // namespace logos::wasm
