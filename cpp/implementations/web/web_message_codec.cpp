#include "web_message_codec.h"

#include "json_mapping.h"
#include "rpc_framing.h"
#include "wire_codec.h"

#include <nlohmann/json.hpp>

#include <cstdint>

namespace logos::web {

using json = nlohmann::json;
using logos::plain::AnyMessage;
using logos::plain::CodecError;
using logos::plain::FramingError;
using logos::plain::kMaxFrameLength;
using logos::plain::MessageType;

namespace {

// The plain transport's own accounting: encodeFrame() measures one tag byte
// plus the payload against the cap, so the web transport measures the tag it
// carries in the envelope plus the text. Same limit, same arithmetic, same
// error — see the header.
bool exceedsCap(std::size_t textLength)
{
    return 1u + static_cast<std::uint64_t>(textLength) > kMaxFrameLength;
}

} // namespace

std::string encodeWebMessage(const AnyMessage& msg)
{
    json envelope;
    envelope["type"]    = static_cast<int>(logos::plain::messageTypeOf(msg));
    envelope["payload"] = logos::plain::messageToJson(msg);
    std::string text = envelope.dump();
    if (exceedsCap(text.size())) throw FramingError("frame too large");
    return text;
}

AnyMessage decodeWebMessage(const std::string& text)
{
    // Checked BEFORE the parse, not after: the cap exists to stop a hostile
    // peer from making us allocate, and a JSON document is at its largest as a
    // parse tree, not as the text it came from.
    if (exceedsCap(text.size()))
        throw FramingError("frame length exceeds cap");

    json envelope;
    try {
        envelope = json::parse(text);
    } catch (const std::exception& e) {
        throw CodecError(std::string("web message parse failed: ") + e.what());
    }
    if (!envelope.is_object() || !envelope.contains("type")
        || !envelope.contains("payload")) {
        throw CodecError("web message is not a {type, payload} envelope");
    }
    if (!envelope["type"].is_number_integer())
        throw CodecError("web message type tag is not an integer");

    const int tag = envelope["type"].get<int>();
    if (tag < static_cast<int>(MessageType::Call)
        || tag > static_cast<int>(MessageType::MethodsResult)) {
        throw CodecError("web message carries an unknown type tag");
    }

    return logos::plain::jsonToMessage(static_cast<MessageType>(tag),
                                       envelope["payload"]);
}

} // namespace logos::web
