#ifndef LOGOS_WASM_OUTBOUND_DOOR_H
#define LOGOS_WASM_OUTBOUND_DOOR_H

// THE OUTBOUND DOOR'S ONE SEAM WITH ITS HOST.
//
// wasm_lp_abi.cpp implements lp_client_create / lp_client_destroy /
// lp_invoke_async, which is everything a module's generated consumer stack
// calls when it wants to reach a dependency. What it cannot own is the CHANNEL:
// a wasm image has exactly one, the host built it (the Worker's message port in
// logos-module-builder's logos_wasm_host.cpp, an in-memory pair in this repo's
// tests), and the host is the only thing that knows when it is up.
//
// So the host installs it here, once, immediately after start(), and the ABI
// above borrows it. One function rather than a registry: there is one image,
// one channel and one connection, and a second would be a second identity on a
// wire whose whole authorization model is "identity is the channel" (ADR 0005).
//
// WHY NOT A NEW lp_* ENTRY POINT. Nothing outside the image ever calls this —
// it is C++ to C++ inside one linear memory, between two files that are linked
// together by construction — and putting it on the frozen C ABI would publish a
// door for a host to hand a wasm image somebody else's connection.

#include "rpc_connection_base.h"

#include <memory>

namespace logos::wasm {

// Hand the image's one connection to the outbound door. Pass a null pointer to
// take it back (a host tearing its channel down), after which every outbound
// call is refused with "transport_error" rather than reaching a dead peer.
//
// The door holds a WEAK reference: the host owns the connection's lifetime, and
// a client that outlives it must fail rather than keep it alive.
void setOutboundConnection(const std::shared_ptr<logos::plain::RpcConnectionBase>& connection);

// Whether a call made right now would reach a live connection. The image is up
// before its host has finished wiring the channel, so a module calling out from
// its own on_context_ready has to be able to ask.
bool outboundDoorIsOpen();

} // namespace logos::wasm

#endif // LOGOS_WASM_OUTBOUND_DOOR_H
