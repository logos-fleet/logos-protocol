#ifndef LOGOS_WEB_MESSAGE_CODEC_H
#define LOGOS_WEB_MESSAGE_CODEC_H

#include "rpc_message.h"

#include <string>

namespace logos::web {

// -----------------------------------------------------------------------------
// One protocol message ⇄ one JSON text.
//
// THE SAME MESSAGE SET AS THE PLAIN TRANSPORT, and the same payload shapes: the
// body is exactly what json_mapping.cpp produces for the plain wire, bytes and
// all ({"_bytes":"<base64url>"}). Only the framing is gone, because a message
// channel already delivers whole messages:
//
//     plain wire : [4-byte length][1-byte type tag][payload bytes]
//     web wire   : {"type": <tag>, "payload": <the same payload>}
//
// `type` carries the SAME numbers MessageType does on the plain wire (Call = 1
// … MethodsResult = 8) rather than a second set of names, so the two transports
// cannot drift and a browser SDK written against either reads the other.
//
// THE SIZE CAP IS THE PLAIN TRANSPORT'S. logos::plain::kMaxFrameLength exists to
// stop a malformed peer from provoking a runaway allocation, and a transport
// that dropped it because it has no length prefix would simply be the way
// around it. Both directions therefore enforce it, accounted exactly as
// encodeFrame() does (one tag byte plus the payload), and refuse with the same
// logos::plain::FramingError.
// -----------------------------------------------------------------------------

// Throws logos::plain::FramingError when the encoded message exceeds the cap.
std::string encodeWebMessage(const logos::plain::AnyMessage& msg);

// Throws logos::plain::FramingError when `text` exceeds the cap, and
// logos::plain::CodecError when it is not a well-formed envelope.
logos::plain::AnyMessage decodeWebMessage(const std::string& text);

} // namespace logos::web

#endif // LOGOS_WEB_MESSAGE_CODEC_H
