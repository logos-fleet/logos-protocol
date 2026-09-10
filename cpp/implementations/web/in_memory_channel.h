#ifndef LOGOS_WEB_IN_MEMORY_CHANNEL_H
#define LOGOS_WEB_IN_MEMORY_CHANNEL_H

#include "message_channel.h"

#include <utility>

namespace logos::web {

// -----------------------------------------------------------------------------
// An in-process pair of message channels, wired to each other.
//
// What a webview bridge is, minus the webview: whatever one endpoint sends is
// delivered to the other endpoint's receiver, whole, in order, and on a thread
// that is not the sender's. That last part is not a testing convenience — it is
// the contract on IMessageChannel, and a pair that delivered inline would
// deadlock the peer above it. So each endpoint drains its inbox on its own
// thread, exactly as a postMessage queue does.
//
// Used by the tests to drive the real host and the real connection against each
// other, and usable by any host that wants both ends of the web transport in
// one process (logoscore --container web, before a webview exists).
//
// Closing either endpoint closes the conversation: the peer's send() starts
// failing and its pending deliveries are dropped, which is what a webview going
// away looks like.
// -----------------------------------------------------------------------------
std::pair<MessageChannelPtr, MessageChannelPtr> makeInMemoryChannelPair();

} // namespace logos::web

#endif // LOGOS_WEB_IN_MEMORY_CHANNEL_H
