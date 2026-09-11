#ifndef LOGOS_PLAIN_JSON_MAPPING_H
#define LOGOS_PLAIN_JSON_MAPPING_H

// Shared RpcValue ↔ nlohmann::json conversion used by both JsonCodec
// (dump / parse as text) and CborCodec (to_cbor / from_cbor as bytes).
// The JSON representation is the canonical in-memory form; codecs differ
// only in how they serialize that form to the wire.

#include "rpc_message.h"
#include "rpc_value.h"
#include "wire_codec.h"

#include <nlohmann/json.hpp>

namespace logos::plain {

nlohmann::json   messageToJson(const AnyMessage& msg);
AnyMessage       jsonToMessage(MessageType tag, const nlohmann::json& j);

// THE VALUE MAPPING, on its own.
//
// Public because a HOST outside this repo has to reach it. A message's payload
// crosses the wire as RpcValue and a module's C ABI takes JSON text
// (logos_module_impl.h: args are a JSON array, the result a JSON value), so
// anything that serves a module over this transport converts in both
// directions. The Wasm host in logos-module-builder is the first such caller;
// before it, every consumer of this mapping was inside these two files and the
// conversion lived in an anonymous namespace.
//
// Same encoding as messageToJson uses for a payload, necessarily: it is the
// same function underneath. Bytes are the canonical {"_bytes": base64url} form.
nlohmann::json   rpcValueToJson(const RpcValue& v);
RpcValue         jsonToRpcValue(const nlohmann::json& j);

} // namespace logos::plain

#endif // LOGOS_PLAIN_JSON_MAPPING_H
