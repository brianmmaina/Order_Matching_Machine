#pragma once

// ---------------------------------------------------------------------------
// Session identity and lifecycle.
//
// A connection is a file descriptor. A SESSION is who is on the other end and
// what they currently own — the orders resting in the book on their behalf.
// The distinction matters because the two do not have the same lifetime: a
// socket can die at any instant, but the orders it placed are still in the book
// and still matchable until something removes them.
//
// CANCEL-ON-DISCONNECT is the reason this file exists. When a session dies its
// resting orders are cancelled. Real exchanges do this, and the reason is that
// a client which has lost its connection can no longer manage its risk: it
// cannot cancel, cannot hedge, and cannot even observe that it was filled. An
// order left resting for a client that cannot see it is an unbounded liability
// accruing to someone who has been struck blind. Better to remove it and let
// them re-enter deliberately when they reconnect.
//
// The awkward part, and the thing session 1.4 has to answer: the NETWORK thread
// notices the disconnect, but only the MATCHING thread may touch the book. The
// network thread therefore cannot cancel anything itself. It emits a
// CancelAllForSession command onto the same queue as every other command, and
// the matching thread performs the cancels in order with everything else. One
// path for everything, no special case, no lock.
//
// TIME IS INJECTED, never read from a clock inside this class. Testing a 15
// second timeout by sleeping for 15 seconds produces a slow test suite that
// people start skipping, and a flaky one on a loaded CI runner. Callers pass a
// monotonic timestamp; tests pass whatever they like.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <unordered_set>

namespace ome {

using SessionId = std::uint64_t;

// Monotonic nanoseconds. Monotonic specifically, not wall clock: a session must
// not be declared dead because someone corrected the system time or a leap
// second landed mid-trading.
using Nanos = std::uint64_t;

struct SessionConfig {
    Nanos heartbeat_interval_ns = 5ULL * 1000 * 1000 * 1000;  // send every 5s
    // Three missed heartbeats. Tolerating one lost packet or one scheduling
    // hiccup matters more than detecting death 5 seconds sooner; a false
    // positive here cancels a live client's orders.
    Nanos timeout_ns = 15ULL * 1000 * 1000 * 1000;
};

enum class SessionState { Connected, Dead };

class Session {
public:
    Session(SessionId id, Nanos now, SessionConfig cfg = {})
        : id_(id), cfg_(cfg), last_inbound_ns_(now), last_heartbeat_sent_ns_(now) {}

    [[nodiscard]] SessionId id() const noexcept { return id_; }
    [[nodiscard]] SessionState state() const noexcept { return state_; }
    [[nodiscard]] bool alive() const noexcept { return state_ == SessionState::Connected; }

    // Any inbound traffic proves liveness, not just heartbeats — a client
    // streaming orders is obviously alive and should never time out.
    void on_inbound(Nanos now) noexcept { last_inbound_ns_ = now; }

    [[nodiscard]] bool heartbeat_due(Nanos now) const noexcept {
        return alive() && now - last_heartbeat_sent_ns_ >= cfg_.heartbeat_interval_ns;
    }
    void on_heartbeat_sent(Nanos now) noexcept { last_heartbeat_sent_ns_ = now; }

    [[nodiscard]] bool timed_out(Nanos now) const noexcept {
        return alive() && now - last_inbound_ns_ >= cfg_.timeout_ns;
    }

    // Idempotent: returns true only on the transition, so a caller can use the
    // return value to emit exactly one CancelAllForSession. Double-cancelling
    // is not harmful today but would be once ids are reused.
    [[nodiscard]] bool mark_dead() noexcept {
        if (state_ == SessionState::Dead) {
            return false;
        }
        state_ = SessionState::Dead;
        return true;
    }

    // client_order_id uniqueness is PER SESSION, not global. Clients number
    // their own orders from 1 and cannot be expected to coordinate with each
    // other; the exchange_order_id is the globally unique one.
    [[nodiscard]] bool register_order(std::uint64_t client_order_id) {
        return live_orders_.insert(client_order_id).second;
    }

    [[nodiscard]] bool has_order(std::uint64_t client_order_id) const {
        return live_orders_.count(client_order_id) != 0;
    }

    // Called when an order leaves the book — fully filled or cancelled.
    // NOTE the consequence: the id becomes reusable by that client. That is
    // deliberate, since a long-lived session would otherwise accumulate ids
    // forever, but it does mean client_order_id is unique among LIVE orders
    // rather than for all time. docs/PROTOCOL.md says so.
    bool forget_order(std::uint64_t client_order_id) {
        return live_orders_.erase(client_order_id) != 0;
    }

    [[nodiscard]] const std::unordered_set<std::uint64_t>& live_orders() const noexcept {
        return live_orders_;
    }
    [[nodiscard]] std::size_t live_order_count() const noexcept { return live_orders_.size(); }

private:
    SessionId id_;
    SessionConfig cfg_;
    SessionState state_{SessionState::Connected};
    Nanos last_inbound_ns_;
    Nanos last_heartbeat_sent_ns_;
    std::unordered_set<std::uint64_t> live_orders_;
};

}  // namespace ome
