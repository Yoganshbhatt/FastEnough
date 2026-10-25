// Client-side view of the wire protocol.
//
// The authoritative definition lives in server/protocol.h so the Android
// helper and every client share exactly one copy: a command value, a flag or
// a packed struct can no longer drift between two hand-maintained headers.
// `protocol_parity` in the test suite still dumps both translation units and
// compares values and layouts, which now also guards this indirection.
#pragma once

#include "../server/protocol.h"
