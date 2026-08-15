#pragma once

// ---------------------------------------------------------------------------
// Per-session egress queue: matching thread produces, network thread consumes.
//
// One SPSC queue per session rather than one shared MPSC queue. The matching
// thread is the single producer for every one of them and each network-side
// consumer is unique, so the SPSC contract holds per queue. A shared queue
// would need multi-consumer synchronization on the drain path and would let one
// slow session's backlog delay every other session's events.
//
// THE LIFETIME PROBLEM
//
// A session dies while its egress queue still holds events, and possibly while
// the matching thread is midway through pushing another. Freeing the queue when
// the socket closes would free memory the matching thread is actively writing.
// A mutex around the queue would put a lock on the hot path, which is the one
// thing this architecture exists to avoid.
//
// THE ANSWER: a tombstone carried on the queue itself.
//
//   1. network thread notices the disconnect. It closes the socket but KEEPS
//      the Connection and its queue alive, and pushes CancelAllForSession.
//   2. matching thread performs the cancels, then pushes SessionRetired as the
//      last event it will ever put on that queue, then forgets the session.
//   3. network thread drains the queue, sees SessionRetired, and only then
//      destroys the queue.
//
// Because SessionRetired is pushed last and the matching thread drops its
// pointer immediately after, observing that event is proof that no further push
// can occur. No lock, no reference counting, no window.
//
// The plan sketched "freed only by the matching thread". Freeing on the network
// thread after the tombstone is equally safe — the tombstone is what orders the
// two, not which thread calls the destructor — and it keeps deallocation off
// the matching thread, which should not be calling free() on its hot path.
// ---------------------------------------------------------------------------

#include <atomic>
#include <cstddef>

#include "ome/commands.hpp"
#include "spsc_queue.h"

namespace ome {

// 4096 events. An event is ~48 bytes, so ~200 KiB per session — large enough to
// absorb a burst of fills while the network thread is busy elsewhere, small
// enough that a thousand sessions is not a surprise.
inline constexpr std::size_t kEgressCapacity = 4096;

class EgressQueue {
public:
    // Matching thread only. Returns false if full.
    [[nodiscard]] bool push(const OrderEvent& e) { return q_.push(e); }

    // Network thread only.
    [[nodiscard]] std::optional<OrderEvent> pop() { return q_.pop(); }

    // Network thread only. Lets the loop decide not to block when work is
    // already waiting, so delivery does not depend on the wake-up channel
    // being perfect. See notifier.hpp.
    [[nodiscard]] bool empty() const { return q_.empty(); }

    // Set by the matching thread when a push fails, read by the network thread.
    //
    // This exists because the matching thread cannot report a full queue THROUGH
    // the queue. Order flow must not be dropped — an ack or fill is a distinct
    // fact about a client's position, not idempotent state — so the only
    // correct response is to disconnect the session, and the network thread is
    // the one that can do that.
    //
    // relaxed is sufficient: it is a one-way latch whose exact timing does not
    // matter. A late observation costs one extra loop iteration.
    void mark_overflowed() noexcept { overflowed_.store(true, std::memory_order_relaxed); }
    [[nodiscard]] bool overflowed() const noexcept {
        return overflowed_.load(std::memory_order_relaxed);
    }

private:
    SpscQueue<OrderEvent, kEgressCapacity> q_;
    std::atomic<bool> overflowed_{false};
};

}  // namespace ome
