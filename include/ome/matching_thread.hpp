#pragma once

// ---------------------------------------------------------------------------
// The matching thread. THE single writer of the book.
//
// Everything this class owns is touched by exactly one thread and therefore
// needs no synchronization at all:
//
//   - the MatchingEngine and its OrderBook
//   - owner_of_        exchange_order_id -> which session placed it
//   - session_orders_  session -> its live exchange order ids
//   - next_exchange_id_ a plain counter, no atomics needed
//
// The only shared state is the inbound SPSC queue and the per-session egress
// queues, and those are the entire concurrency surface of the design. Being
// able to say that in one sentence is the point of building it this way.
//
// WHY THE OWNERSHIP MAP EXISTS
//
// A Trade carries buyer_id and seller_id, which are ORDER ids. Routing a fill
// to a socket needs a SESSION id. Nothing in the engine knows about sessions —
// nor should it — so the mapping lives here, on the only thread that can
// maintain it consistently with the book it describes.
// ---------------------------------------------------------------------------

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "matching_engine/matching_engine.hpp"
#include "ome/commands.hpp"
#include "ome/book_snapshot.hpp"
#include "ome/egress.hpp"
#include "ome/risk_config.hpp"
#include "ome/waiter.hpp"
#include "spsc_queue.h"

namespace ome {

inline constexpr std::size_t kInboundCapacity = 8192;
using InboundQueue = SpscQueue<OrderCommand, kInboundCapacity>;

struct MatchingStats {
    std::uint64_t commands_applied{0};
    std::uint64_t events_emitted{0};
    std::uint64_t egress_overflows{0};
    std::uint64_t market_data_dropped{0};
    std::uint64_t sessions_retired{0};
};

class MatchingThread {
public:
    MatchingThread(InboundQueue& inbound, Waiter& waiter, RiskConfig risk = {})
        : inbound_(inbound), waiter_(waiter), risk_(risk) {}

    MatchingThread(const MatchingThread&) = delete;
    MatchingThread& operator=(const MatchingThread&) = delete;

    void start() {
        running_.store(true, std::memory_order_relaxed);
        thread_ = std::thread([this] { run(); });
    }

    void stop() {
        running_.store(false, std::memory_order_relaxed);
        // The consumer may be parked; nudge it so shutdown is prompt rather
        // than waiting out the block timeout.
        waiter_.signal();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    ~MatchingThread() { stop(); }

    // Test-only accessors, valid after stop() has joined the thread. Calling
    // these while it runs would be exactly the race this design prevents.
    [[nodiscard]] const MatchingEngine& engine() const noexcept { return engine_; }
    [[nodiscard]] const MatchingStats& stats() const noexcept { return stats_; }

    // Drives one batch synchronously on the CALLING thread. For tests that want
    // determinism instead of a background thread; never used in production.
    std::size_t drain_once() { return drain(); }

private:
    void run() {
        while (running_.load(std::memory_order_relaxed)) {
            // Spin briefly, then park. See waiter.hpp for the missed-wake-up
            // race this ordering avoids.
            // The predicate reads the queue directly. An auxiliary "there is
            // work" flag would have to be written by the producer thread, which
            // is a second piece of shared state to get wrong for no benefit.
            waiter_.wait([this] {
                return !inbound_.empty() || !running_.load(std::memory_order_relaxed);
            });
            drain();
        }
        drain();  // final sweep so shutdown does not strand queued commands
    }

    std::size_t drain() {
        std::size_t applied = 0;
        while (auto cmd = inbound_.pop()) {
            apply(*cmd);
            ++applied;
            ++stats_.commands_applied;
        }
        return applied;
    }

    void apply(const OrderCommand& c) {
        switch (c.type) {
            case CommandType::SessionOpened:
                egress_[c.session] = c.egress;
                md_[c.session] = static_cast<MarketDataQueue*>(c.md_queue);
                return;
            case CommandType::Subscribe:
                // The broadcast set is matching-thread state, reached only
                // through the command stream. A network thread writing it
                // directly would be shared mutable state between the two.
                subscribers_[c.session] =
                    static_cast<std::uint8_t>(std::min<std::uint32_t>(
                        c.quantity == 0 ? protocol::kMaxBookDepth : c.quantity,
                        protocol::kMaxBookDepth));
                emit(egress_for(c.session),
                     OrderEvent::ack(c.session, 0, 0));
                publish_book();  // a fresh subscriber gets current state at once
                return;
            case CommandType::NewOrder:
                apply_new_order(c);
                return;
            case CommandType::Cancel:
                apply_cancel(c);
                return;
            case CommandType::Modify:
                apply_modify(c);
                return;
            case CommandType::CancelAllForSession:
                apply_cancel_all(c);
                return;
        }
    }

    // Book-state risk checks. Runs on the matching thread because the price
    // band is relative to the last trade or the mid, and only this thread may
    // read the book. Returns NONE when the order is acceptable.
    [[nodiscard]] RejectReason check_risk(const OrderCommand& c) const {
        if (c.quantity == 0) {
            return RejectReason::INVALID_QTY;
        }
        if (c.quantity > risk_.max_order_qty) {
            return RejectReason::RISK_MAX_ORDER_SIZE;
        }
        // A market order carries no meaningful price, so price checks do not
        // apply to it — it takes whatever the book offers.
        if (c.order_type == protocol::OrderType::Market) {
            return RejectReason::NONE;
        }
        if (c.price_ticks <= 0) {
            return RejectReason::INVALID_PRICE;
        }
        // Integer modulo, not an epsilon comparison. Prices are tick counts, so
        // "aligned" is exact or it is nothing.
        if (risk_.tick_size > 1 && (c.price_ticks % risk_.tick_size) != 0) {
            return RejectReason::INVALID_PRICE;
        }
        if (risk_.price_band_bp > 0) {
            std::int64_t reference = 0;
            if (!reference_price(reference)) {
                // No last trade and no two-sided book: nothing to be far from.
                // Rejecting here would refuse the first order ever placed.
                return RejectReason::NONE;
            }
            // All integer arithmetic. The band is computed as a signed distance
            // scaled by 10000 rather than by dividing, so nothing rounds toward
            // zero and turns a marginal breach into an acceptance.
            const std::int64_t distance = (c.price_ticks > reference)
                                              ? c.price_ticks - reference
                                              : reference - c.price_ticks;
            if (distance * 10000 > reference * risk_.price_band_bp) {
                return RejectReason::RISK_PRICE_BAND;
            }
        }
        return RejectReason::NONE;
    }

    // Last trade if there has been one, else the mid of a two-sided book.
    [[nodiscard]] bool reference_price(std::int64_t& out) const {
        if (last_trade_ticks_ > 0) {
            out = last_trade_ticks_;
            return true;
        }
        const auto bids = engine_.book().bid_levels_ticks(1);
        const auto asks = engine_.book().ask_levels_ticks(1);
        if (bids.empty() || asks.empty()) {
            return false;
        }
        out = (bids[0].first + asks[0].first) / 2;
        return true;
    }

    void apply_new_order(const OrderCommand& c) {
        EgressQueue* eg = egress_for(c.session);

        // Risk gate first: nothing reaches the book until it has passed.
        const RejectReason risk = check_risk(c);
        if (risk != RejectReason::NONE) {
            emit(eg, OrderEvent::reject(c.session, c.client_order_id, risk));
            return;
        }

        Order o{};
        o.id = next_exchange_id_++;
        o.price_ticks = c.price_ticks;
        o.quantity = c.quantity;
        o.side = (c.side == protocol::Side::Bid) ? Order::BID : Order::ASK;
        o.type = (c.order_type == protocol::OrderType::Market) ? Order::MARKET : Order::LIMIT;
        o.timestamp = ++logical_clock_;

        // Ownership is recorded BEFORE the apply, because the apply can generate
        // fills for this very order and those fills have to be routable.
        owner_of_[o.id] = c.session;
        client_id_of_[o.id] = c.client_order_id;
        remaining_of_[o.id] = c.quantity;
        side_of_[o.id] = o.side;
        session_orders_[c.session].insert(o.id);

        const std::size_t before = engine_.trade_log_size();
        const ApplyResult r = engine_.processOrder(o);

        if (!r.accepted) {
            forget_order(o.id);
            emit(eg, OrderEvent::reject(c.session, c.client_order_id, r.reason));
            return;
        }

        // Ack precedes Fill. The engine rests an aggressive limit before
        // matching it, so this ordering is not a choice made here — it is the
        // engine's behavior, and docs/PROTOCOL.md commits to it.
        emit(eg, OrderEvent::ack(c.session, c.client_order_id, o.id));
        publish_trades(before);
        publish_book();

        // A market order's unfilled remainder is discarded rather than rested,
        // so it never becomes a resting order anyone could cancel.
        if (o.type == Order::MARKET) {
            forget_order(o.id);
        }
    }

    void apply_cancel(const OrderCommand& c) {
        EgressQueue* eg = egress_for(c.session);
        const std::uint64_t xoid = find_order(c.session, c.client_order_id);
        if (xoid == 0) {
            emit(eg, OrderEvent::reject(c.session, c.client_order_id, RejectReason::UNKNOWN_ORDER));
            return;
        }

        Order cancel{};
        cancel.id = xoid;
        cancel.type = Order::CANCEL;
        const ApplyResult r = engine_.processOrder(cancel);
        if (!r.accepted) {
            emit(eg, OrderEvent::reject(c.session, c.client_order_id, r.reason));
            return;
        }
        forget_order(xoid);
        emit(eg, OrderEvent::ack(c.session, c.client_order_id, xoid, /*closes=*/true));
        publish_book();
    }

    void apply_modify(const OrderCommand& c) {
        // Modify is cancel + new, applied as a unit here so no other command can
        // interleave between the two halves. Time priority is therefore lost on
        // any reprice or size increase — stated in docs/PROTOCOL.md as protocol
        // behavior, because clients must know their queue position moved.
        EgressQueue* eg = egress_for(c.session);
        const std::uint64_t old_id = find_order(c.session, c.client_order_id);
        if (old_id == 0) {
            emit(eg, OrderEvent::reject(c.session, c.client_order_id, RejectReason::UNKNOWN_ORDER));
            return;
        }

        // Captured before the cancel, which erases the bookkeeping: a modify
        // cannot change side, so the replacement inherits it.
        const Order::Side old_side = side_of_[old_id];

        Order cancel{};
        cancel.id = old_id;
        cancel.type = Order::CANCEL;
        if (!engine_.processOrder(cancel).accepted) {
            emit(eg, OrderEvent::reject(c.session, c.client_order_id, RejectReason::UNKNOWN_ORDER));
            return;
        }
        forget_order(old_id);

        OrderCommand replacement = c;
        replacement.type = CommandType::NewOrder;
        replacement.order_type = protocol::OrderType::Limit;
        // Side is not carried on Modify, so it is recovered from the order being
        // replaced — a modify cannot change side.
        replacement.side = (old_side == Order::BID) ? protocol::Side::Bid : protocol::Side::Ask;
        apply_new_order(replacement);
    }

    // Builds one top-of-book snapshot and pushes it to every subscriber.
    //
    // Called after any command that could have moved the book. A subscriber
    // that keeps up sees each of these; one that falls behind has them
    // conflated on the consumer side. See book_snapshot.hpp.
    void publish_book() {
        if (subscribers_.empty()) {
            return;  // nothing to build, so do not walk the book at all
        }
        const auto bids = engine_.book().bid_levels_ticks(protocol::kMaxBookDepth);
        const auto asks = engine_.book().ask_levels_ticks(protocol::kMaxBookDepth);

        BookSnapshot snap{};
        snap.seq = ++book_seq_;
        snap.n_bids = static_cast<std::uint8_t>(bids.size());
        snap.n_asks = static_cast<std::uint8_t>(asks.size());
        for (std::size_t i = 0; i < bids.size(); ++i) {
            snap.bid_ticks[i] = bids[i].first;
            snap.bid_qty[i] = bids[i].second;
        }
        for (std::size_t i = 0; i < asks.size(); ++i) {
            snap.ask_ticks[i] = asks[i].first;
            snap.ask_qty[i] = asks[i].second;
        }

        for (const auto& [session, depth] : subscribers_) {
            static_cast<void>(depth);
            const auto it = md_.find(session);
            if (it == md_.end() || it->second == nullptr) {
                continue;
            }
            if (!it->second->push(snap)) {
                // Full. DROPPING IS CORRECT HERE and is the whole asymmetry:
                // a newer snapshot supersedes an older one, so a subscriber
                // that cannot keep up loses intermediate states and not
                // information. Contrast emit(), where a full queue disconnects.
                ++stats_.market_data_dropped;
            }
        }
    }

    void apply_cancel_all(const OrderCommand& c) {
        // Cancel-on-disconnect, for real. It arrives through the same queue as
        // everything else, so the network thread never touches the book.
        auto it = session_orders_.find(c.session);
        if (it != session_orders_.end()) {
            // Copy: cancelling mutates the set being iterated.
            const std::vector<std::uint64_t> ids(it->second.begin(), it->second.end());
            for (const std::uint64_t xoid : ids) {
                Order cancel{};
                cancel.id = xoid;
                cancel.type = Order::CANCEL;
                static_cast<void>(engine_.processOrder(cancel));
                forget_order(xoid);
            }
        }
        engine_.clear_trade_log();

        // The tombstone: last event this queue will ever receive. See egress.hpp.
        EgressQueue* eg = egress_for(c.session);
        if (eg != nullptr) {
            static_cast<void>(eg->push(OrderEvent::session_retired(c.session)));
        }
        session_orders_.erase(c.session);
        egress_.erase(c.session);
        md_.erase(c.session);
        subscribers_.erase(c.session);
        ++stats_.sessions_retired;
        publish_book();  // the cancels changed the book
    }

    // Turns the trades this command produced into per-session fills.
    void publish_trades(std::size_t from) {
        const auto& log = engine_.trade_log();
        for (std::size_t i = from; i < log.size(); ++i) {
            const Trade& t = log[i];
            last_trade_ticks_ = t.price_ticks;
            route_fill(t.buyer_id, t);
            route_fill(t.seller_id, t);
        }
        // Drain every iteration so the log stays bounded; it otherwise grows for
        // the lifetime of the process.
        engine_.clear_trade_log();
    }

    void route_fill(std::uint64_t xoid, const Trade& t) {
        const auto owner = owner_of_.find(xoid);
        if (owner == owner_of_.end()) {
            return;  // an order from a session already retired
        }
        const SessionId s = owner->second;

        std::uint32_t& rem = remaining_of_[xoid];
        rem = (rem > t.quantity) ? rem - t.quantity : 0;

        std::uint64_t coid = 0;
        const auto ci = client_id_of_.find(xoid);
        if (ci != client_id_of_.end()) {
            coid = ci->second;
        }
        emit(egress_for(s), OrderEvent::fill(s, coid, xoid, t.price_ticks, t.quantity, rem));
        if (rem == 0) {
            forget_order(xoid);
        }
    }

    void emit(EgressQueue* eg, const OrderEvent& e) {
        if (eg == nullptr) {
            return;  // session already retired; nothing to deliver to
        }
        if (!eg->push(e)) {
            // Cannot report a full queue through the queue. Latch it and let
            // the network thread disconnect — order flow is never dropped.
            eg->mark_overflowed();
            ++stats_.egress_overflows;
            return;
        }
        ++stats_.events_emitted;
    }

    [[nodiscard]] EgressQueue* egress_for(SessionId s) {
        const auto it = egress_.find(s);
        return it == egress_.end() ? nullptr : it->second;
    }

    [[nodiscard]] std::uint64_t find_order(SessionId s, std::uint64_t client_order_id) const {
        const auto it = session_orders_.find(s);
        if (it == session_orders_.end()) {
            return 0;
        }
        for (const std::uint64_t xoid : it->second) {
            const auto c = client_id_of_.find(xoid);
            if (c != client_id_of_.end() && c->second == client_order_id) {
                return xoid;
            }
        }
        return 0;
    }

    void forget_order(std::uint64_t xoid) {
        const auto owner = owner_of_.find(xoid);
        if (owner != owner_of_.end()) {
            const auto so = session_orders_.find(owner->second);
            if (so != session_orders_.end()) {
                so->second.erase(xoid);
            }
            owner_of_.erase(owner);
        }
        client_id_of_.erase(xoid);
        remaining_of_.erase(xoid);
        side_of_.erase(xoid);
    }

private:
    MatchingEngine engine_;
    InboundQueue& inbound_;
    Waiter& waiter_;
    std::thread thread_;
    std::atomic<bool> running_{false};

    std::unordered_map<SessionId, EgressQueue*> egress_;
    std::unordered_map<SessionId, MarketDataQueue*> md_;
    std::unordered_map<SessionId, std::uint8_t> subscribers_;
    std::unordered_map<std::uint64_t, SessionId> owner_of_;
    std::unordered_map<std::uint64_t, std::uint64_t> client_id_of_;
    std::unordered_map<std::uint64_t, std::uint32_t> remaining_of_;
    std::unordered_map<std::uint64_t, Order::Side> side_of_;
    std::unordered_map<SessionId, std::unordered_set<std::uint64_t>> session_orders_;

    RiskConfig risk_;
    // Reference for the price band. 0 means "no trade yet", which falls back to
    // the mid and then to accepting anything.
    std::int64_t last_trade_ticks_{0};
    std::uint64_t book_seq_{0};
    std::uint64_t next_exchange_id_{1};
    std::uint64_t logical_clock_{0};
    MatchingStats stats_;
};

}  // namespace ome
