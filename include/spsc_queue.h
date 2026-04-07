#pragma once

// spsc = single producer single consumer: exactly one thread may call push() and exactly one
// may call pop(). no mutex — atomic head/tail + release/acquire handoff on the buffer slots.
//
// why this is safe without locks (given spsc):
// - only the producer advances head_ and only the consumer advances tail_, so no concurrent
//   writers on those atomics (no lost updates / torn writes on the indices).
// - each buffer cell is written only by the producer, and only after a successful push
//   the producer does release-store head so the consumer’s acquire-load of head happens-after
//   the slot write (the consumer never reads a cell before the producer published that index).
// - symmetrically, release-store tail after a pop pairs with the producer’s acquire-load of tail,
//   so the producer only reuses a slot after the consumer’s read/move out is published.
// - with multiple producers or consumers, two threads could race on the same slot or index
//   without stronger sync (e.g. mutex or mpmc algorithm) — that is why the spsc contract matters.

#include <array>
#include <atomic>
#include <cstddef>
#include <optional>

#include "order.h"

template <std::size_t N>
class SpscQueue {
    // capacity n means at most n orders buffered; we use n+1 slots so “full” and “empty”
    // (head == tail vs. (head+1)%slots == tail) stay distinguishable without a third counter.
    static_assert(N > 0, "spsc: template capacity n must be > 0");
    static constexpr std::size_t kSlots = N + 1;

    std::array<Order, kSlots> buffer_{};

    // false sharing: head_ and tail_ live in different logical variables but if they share one
    // cache line (e.g. 64 bytes), a write on one core invalidates that whole line on other cores.
    // the producer mostly stores head_ and acquire-loads tail_; the consumer mostly stores tail_ and
    // acquire-loads head_. without padding/alignment, each push/pop can force the other core to
    // refetch the same line even though they update different atomics — extra coherence traffic.
    // alignas(64) on each atomic starts tail_ on its own line so producer and consumer traffic
    // targets different lines under typical 64-byte cache-line size.
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};

public:
    SpscQueue() = default;

    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    // producer-only: returns false if queue is full (no block / spin — typical lock-free try).
    [[nodiscard]] bool push(Order o) {
        // relaxed ok here: only this thread modifies head_; intra-thread order still visible.
        const std::size_t h = head_.load(std::memory_order_relaxed);
        const std::size_t t = tail_.load(std::memory_order_acquire);
        const std::size_t next = (h + 1) % kSlots;
        if (next == t) {
            return false;
        }
        buffer_[h] = std::move(o);
        // release: pairs with consumer’s acquire load of head_ so slot h is visible before consumer reads it.
        head_.store(next, std::memory_order_release);
        return true;
    }

    // consumer-only: empty queue → null optional; else one order (moved out of the ring).
    [[nodiscard]] std::optional<Order> pop() {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        // acquire: see producer’s release on head_ before reading buffer_[t].
        const std::size_t h = head_.load(std::memory_order_acquire);
        if (t == h) {
            return std::nullopt;
        }
        Order out = std::move(buffer_[t]);
        // release: pairs with producer’s acquire load of tail_ so reuse of slot t is safe.
        tail_.store((t + 1) % kSlots, std::memory_order_release);
        return out;
    }
};
