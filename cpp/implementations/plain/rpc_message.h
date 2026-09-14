#ifndef LOGOS_PLAIN_RPC_MESSAGE_H
#define LOGOS_PLAIN_RPC_MESSAGE_H

#include "rpc_value.h"

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace logos::plain {

// -----------------------------------------------------------------------------
// Wire message definitions.
//
// Each on-wire frame is [4-byte big-endian length][1-byte type tag][payload].
// The payload encoding is decided by the codec (JSON today, CBOR planned).
//
// `MessageType` doubles as the 1-byte type tag so the codec can decode the
// right struct without peeking inside the payload. Keep the values stable —
// they're on the wire.
// -----------------------------------------------------------------------------

enum class MessageType : uint8_t {
    Call        = 1,
    Result      = 2,
    Subscribe   = 3,
    Unsubscribe = 4,
    Event       = 5,
    Token       = 6,
    Methods     = 7,
    MethodsResult = 8,
};

struct MethodMetadata {
    std::string name;
    std::string signature;
    std::string returnType;
    bool        isInvokable = true;
    RpcList     parameters; // list of {name, type} maps — schema-flexible
};

// Call <module>.<method>(args...). The response is a Result with the same id.
//
// `caller` is the logos-protocol CALLER DOCUMENT of the dispatch this call is
// being relayed out of — the same JSON object logos_module_set_call_caller()
// takes, whose normative shape is in logos_module_impl.h. It exists because a
// RELAY cannot be identified by its token: a Web module's container presents
// that module's own root credential on every call it forwards, so a page
// deriving the caller from the token it was handed answers with ITS OWN NAME
// for every caller in the fleet (logos-workspace#129).
//
// EMPTY IS "NOT SUPPLIED", and it is the default. It is not
// `{"kind":"unknown"}`: a peer that fills this field and a peer that has never
// heard of it are different facts, and only the first one is an assertion about
// who is calling. An empty `caller` is omitted from the encoding entirely, so
// a frame from a consumer that never sets it is byte-identical to what it sent
// before the field existed and a receiver keeps whatever fallback it had.
//
// IT IS NOT A CREDENTIAL. Nothing about this document authorizes anything —
// `authToken` still does that, and it is still checked first. What licenses a
// receiver to believe the document is the CHANNEL it arrived on (ADR 0005): a
// web module's channel is written by its container and by nothing else, so a
// document on it is the container's statement, not a peer's claim.
struct CallMessage {
    uint64_t              id;
    std::string           authToken;
    std::string           object;
    std::string           method;
    std::vector<RpcValue> args;
    std::string           caller;
};

// Response to a Call (or Methods) message, matched by id.
struct ResultMessage {
    uint64_t    id;
    bool        ok = false;
    RpcValue    value;   // present when ok
    std::string err;     // present when !ok
    std::string errCode; // present when !ok
};

// Subscribe / unsubscribe to a named event on an object.
// After Subscribe, the peer pushes EventMessage frames whenever the provider
// emits that event until an Unsubscribe arrives (or the connection closes).
// eventName "" means "all events on this object" (wildcard).
struct SubscribeMessage {
    std::string object;
    std::string eventName;
};
struct UnsubscribeMessage {
    std::string object;
    std::string eventName;
};

// Fire-and-forget event delivery from provider → subscriber.
struct EventMessage {
    std::string           object;
    std::string           eventName;
    std::vector<RpcValue> data;
};

// Authorization token that the consumer wants registered for a specific
// module name. Mirrors LogosObject::informModuleToken today.
struct TokenMessage {
    std::string authToken;
    std::string moduleName;
    std::string token;
};

// Query the set of methods a published object exposes. Response is a
// MethodsResult keyed to the same id.
struct MethodsMessage {
    uint64_t    id;
    std::string authToken;
    std::string object;
};
struct MethodsResultMessage {
    uint64_t                    id;
    bool                        ok = false;
    std::vector<MethodMetadata> methods;
    std::string                 err;
};

// Tagged union of every message the wire stack knows.
using AnyMessage = std::variant<
    CallMessage,
    ResultMessage,
    SubscribeMessage,
    UnsubscribeMessage,
    EventMessage,
    TokenMessage,
    MethodsMessage,
    MethodsResultMessage
>;

// Lookup: AnyMessage variant ↔ MessageType tag.
MessageType messageTypeOf(const AnyMessage& m);

} // namespace logos::plain

#endif // LOGOS_PLAIN_RPC_MESSAGE_H
