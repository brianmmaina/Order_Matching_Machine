#pragma once

// ---------------------------------------------------------------------------
// Market data: a fixed-size top-of-book snapshot, and the queue it travels on.
//
// WHY MARKET DATA GETS ITS OWN QUEUE, SEPARATE FROM ORDER EVENTS
//
// The build plan said to fan out book updates through the existing per-session
// egress queue — "no new mechanism". That does not survive contact with the
// conflation requirement, and the reason is worth stating because it IS the
// design point of this session.
//
// The two streams need OPPOSITE overflow policies:
//
//   order flow    an Ack or a Fill is a distinct fact about a client's
//                 position that it cannot reconstruct. Dropping one leaves the
//                 client believing something false about its own orders. The
//                 only correct response to a full queue is to disconnect.
//
//   market data   a BookUpdate is a complete snapshot of the top of book. A
//                 newer one makes an older one irrelevant — there is nothing
//                 in the old message that the new one does not supersede.
//                 Disconnecting a subscriber for falling behind would be
//                 absurd; the right response is to skip ahead.
//
// One queue can only have one policy. Sharing would force market data to be
// undroppable (unbounded memory for a slow subscriber) or order flow to be
// droppable (silent position corruption). Two queues is what makes the
// asymmetry expressible at all.
//
// HOW CONFLATION IS IMPLEMENTED: on the consumer side.
//
// The obvious reading of "replace the most recent pending update" means
// mutating an entry already in the ring, which breaks the SPSC contract — the
// consumer may be reading that slot right now.
//
// Instead the network thread drains the ENTIRE queue each pass and encodes only
// the last snapshot it found. A subscriber that keeps up sees every update; one
// that falls behind skips straight to current. Conflation without touching a
// published slot, and without a lock.
//
// FIXED SIZE, NO HEAP: this crosses a lock-free ring by value, so it cannot own
// memory. 10 levels a side is the protocol's cap anyway.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <type_traits>

#include "ome/protocol.hpp"
#include "spsc_queue.h"

namespace ome {

struct BookSnapshot {
    std::uint64_t seq{0};
    std::uint8_t n_bids{0};
    std::uint8_t n_asks{0};
    // Parallel arrays rather than an array of pairs: keeps the struct trivially
    // copyable with no padding surprises between elements.
    std::int64_t bid_ticks[protocol::kMaxBookDepth]{};
    std::uint64_t bid_qty[protocol::kMaxBookDepth]{};
    std::int64_t ask_ticks[protocol::kMaxBookDepth]{};
    std::uint64_t ask_qty[protocol::kMaxBookDepth]{};
};

static_assert(std::is_trivially_copyable<BookSnapshot>::value,
              "BookSnapshot crosses a lock-free ring by value");

// Deliberately small. A deep queue would let a slow subscriber accumulate a
// long history of snapshots that are all about to be conflated away — memory
// spent to hold data that will be discarded. 64 is enough to absorb a burst
// between two network-thread passes.
inline constexpr std::size_t kMarketDataCapacity = 64;
using MarketDataQueue = SpscQueue<BookSnapshot, kMarketDataCapacity>;

}  // namespace ome
