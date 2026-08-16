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
#include <string>
#include <chrono>
#include <cstdint>
#include <functional>
#include <thread>
#include <unordered_map>
#include <utility>
#include <unordered_set>
#include <vector>

#include "matching_engine/matching_engine.hpp"
#include "ome/commands.hpp"
#include "ome/book_snapshot.hpp"
#include "ome/egress.hpp"
#include "ome/notifier.hpp"
#include "ome/risk_config.hpp"
#include "ome/snapshot.hpp"
#include "ome/wal.hpp"
#include "ome/waiter.hpp"
#include "spsc_queue.h"

namespace ome {

struct SnapshotPolicy {
    std::string path;            // empty disables snapshotting
    std::uint64_t every_n = 0;   // commands between snapshots; 0 disables
};

inline constexpr std::size_t kInboundCapacity = 8192;
using InboundQueue = SpscQueue<OrderCommand, kInboundCapacity>;

struct MatchingStats {
    std::uint64_t commands_applied{0};
    std::uint64_t events_emitted{0};
    std::uint64_t egress_overflows{0};
    std::uint64_t wal_failures{0};
    std::uint64_t market_data_dropped{0};
    std::uint64_t snapshots{0};
    std::uint64_t snapshot_failures{0};
    std::uint64_t sessions_retired{0};
};

class MatchingThread {
public:
    MatchingThread(InboundQueue& inbound, Waiter& waiter, RiskConfig risk = {},
                   Notifier* egress_ready = nullptr, Wal* wal = nullptr,
                   SnapshotPolicy snap = {})
        : inbound_(inbound), waiter_(waiter), egress_ready_(egress_ready), wal_(wal),
          risk_(risk), snap_(std::move(snap)) {}

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

    // Rebuilds state from a log, before start().
    //
    // THE COMMANDS GO THROUGH apply() — the exact function live traffic uses.
    // Not a parallel "recovery apply", not a simplified fast path. If replay
    // used different code, "the recovered book is identical" would be a claim
    // about two implementations agreeing, which is a much weaker and much more
    // fragile thing than the same code seeing the same inputs.
    //
    // The one difference is that recovery must not emit events: the sessions
    // those events belong to do not exist yet, and their egress queues are not
    // registered. That is handled by suppressing emission rather than by
    // branching inside apply — see replaying_.
    //
    // Returns the number of commands applied.
    std::size_t recover(const std::vector<OrderCommand>& commands) {
        replaying_ = true;
        for (const auto& c : commands) {
            apply(c);
        }
        replaying_ = false;
        // Trades produced during replay were already reflected in the book; the
        // log would otherwise grow with history nobody will read.
        engine_.clear_trade_log();
        recovered_ = commands.size();
        return recovered_;
    }

    [[nodiscard]] std::size_t recovered_commands() const noexcept { return recovered_; }

    // The book's canonical fingerprint. Safe to call before start() or after
    // stop(); calling it while the thread runs is the race this design exists
    // to prevent.
    [[nodiscard]] std::uint64_t digest() const { return engine_.book().digest(); }

    // Everything needed to reconstruct this thread's state. Call between
    // batches on the matching thread, or before start()/after stop().
    [[nodiscard]] SnapshotData export_snapshot(std::uint64_t last_seq) const {
        SnapshotData d{};
        d.last_seq = last_seq;
        d.last_trade_ticks = last_trade_ticks_;
        d.next_exchange_id = next_exchange_id_;
        for (const auto& o : engine_.book().all_orders()) {
            SnapshotOrder so{};
            so.exchange_id = o.id;
            so.price_ticks = o.price_ticks;
            so.quantity = o.quantity;
            so.side = (o.side == Order::BID) ? 0 : 1;
            so.timestamp = o.timestamp;
            const auto owner = owner_of_.find(o.id);
            so.session = (owner == owner_of_.end()) ? 0 : owner->second;
            const auto cid = client_id_of_.find(o.id);
            so.client_order_id = (cid == client_id_of_.end()) ? 0 : cid->second;
            d.orders.push_back(so);
        }
        return d;
    }

    // Rebuilds from a snapshot. Before start(), like recover().
    //
    // Orders are re-added in the order they were exported — book order, which
    // is time priority within each level — so addOrder reconstructs the same
    // book. The ownership maps are rebuilt alongside, otherwise a cancel
    // arriving after recovery would find nothing.
    // Called after restore() so the first snapshot is not taken immediately on
    // a restart that already loaded one.
    void set_snapshot_baseline(std::uint64_t seq) {
        last_snapshot_seq_ = seq;
        next_snapshot_at_ = seq + snap_.every_n;
    }

    void restore(const SnapshotData& d) {
        last_trade_ticks_ = d.last_trade_ticks;
        next_exchange_id_ = d.next_exchange_id;
        for (const auto& so : d.orders) {
            Order o{};
            o.id = so.exchange_id;
            o.price_ticks = so.price_ticks;
            o.quantity = so.quantity;
            o.side = (so.side == 0) ? Order::BID : Order::ASK;
            o.type = Order::LIMIT;
            o.timestamp = so.timestamp;
            engine_.book().addOrder(o);

            owner_of_[so.exchange_id] = so.session;
            client_id_of_[so.exchange_id] = so.client_order_id;
            remaining_of_[so.exchange_id] = so.quantity;
            side_of_[so.exchange_id] = o.side;
            session_orders_[so.session].insert(so.exchange_id);
            by_client_id_[key_of(so.session, so.client_order_id)] = so.exchange_id;
        }
        // Logical timestamps must not restart below what the restored orders
        // already carry, or a later order could sort ahead of an earlier one.
        for (const auto& so : d.orders) {
            if (so.timestamp > logical_clock_) {
                logical_clock_ = so.timestamp;
            }
        }
    }

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

    // Monotonic nanoseconds for the WAL's group-commit timer.
    static std::uint64_t monotonic_ns() {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    std::size_t drain() {
        std::size_t applied = 0;
        while (auto cmd = inbound_.pop()) {
            apply(*cmd);
            ++applied;
            ++stats_.commands_applied;
        }
        // One wake-up per BATCH, not per event. The network thread only needs
        // to be told that something is waiting; telling it a thousand times
        // for a thousand fills would put a syscall per fill on this thread.
        if (applied > 0 && egress_ready_ != nullptr) {
            egress_ready_->notify();
        }
        // Give the group commit a chance at the end of every batch, so an
        // interval boundary is not missed just because the queue went quiet.
        if (applied > 0 && wal_ != nullptr) {
            wal_->poll_sync(monotonic_ns());
        }
        // BETWEEN batches, never mid-batch: the snapshot must reflect a
        // consistent point in the command stream, and the WAL sequence it
        // records has to line up with a command boundary or recovery would
        // replay a partially applied batch.
        maybe_snapshot();
        return applied;
    }

    // Pause-the-world: the matching thread writes the snapshot itself and stops
    // matching while it does. Measured at 28ms for 100k resting orders — see
    // docs/BENCHMARK.md for why the fix is to move the WRITE off this thread
    // rather than the copy.
    void maybe_snapshot() {
        if (snap_.path.empty() || snap_.every_n == 0 || wal_ == nullptr) {
            return;
        }
        const std::uint64_t seq = wal_->last_seq();
        if (seq < next_snapshot_at_) {
            return;
        }

        // Keep the previous snapshot: if a crash lands while the new one is
        // being written, rename() guarantees the old file is intact, and
        // recovery falls back to it. The WAL is only truncated up to the
        // PREVIOUS snapshot's sequence, so the older snapshot still has the
        // records it needs.
        const std::string prev = snap_.path + ".prev";
        static_cast<void>(::rename(snap_.path.c_str(), prev.c_str()));

        std::string err;
        if (!write_snapshot(snap_.path, export_snapshot(seq), err)) {
            ++stats_.snapshot_failures;
            return;
        }
        if (last_snapshot_seq_ > 0) {
            std::string terr;
            static_cast<void>(wal_->truncate_before(last_snapshot_seq_, terr));
        }
        last_snapshot_seq_ = seq;
        next_snapshot_at_ = seq + snap_.every_n;
        ++stats_.snapshots;
    }

    void apply(const OrderCommand& c) {
        // WRITE-AHEAD: the record goes down before the book is touched.
        //
        // The asymmetry this ordering chooses: a crash between the append and
        // the apply leaves a record for a command that never reached the book,
        // and recovery replays it — applied once, just later than the client
        // believed. The reverse order would mutate the book for a command whose
        // record never landed, and nothing afterwards could detect that the
        // rebuilt book differs from the one that existed.
        //
        // Only state-mutating commands are logged. A SessionOpened or Subscribe
        // describes a connection that will not exist after a restart, and
        // replaying it would produce events for a session that is gone.
        // Replay must not re-log what it is reading: appending during recovery
        // would double the log every time the process restarted.
        if (!replaying_ && wal_ != nullptr && is_loggable(c.type)) {
            if (!wal_->append(c, monotonic_ns())) {
                // The log is the source of truth for recovery. Applying a
                // command we failed to record would silently break the
                // guarantee, so stop instead — a halted exchange is recoverable,
                // a quietly diverged one is not.
                ++stats_.wal_failures;
                return;
            }
        }

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
        // Best PRICE only. This runs on every order when the band is enabled,
        // and asking for the level's aggregate quantity here made it walk the
        // top level's orders for a number it then discarded.
        std::int64_t bid = 0;
        std::int64_t ask = 0;
        if (!engine_.book().best_bid_ticks(bid) || !engine_.book().best_ask_ticks(ask)) {
            return false;
        }
        out = (bid + ask) / 2;
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
        by_client_id_[key_of(c.session, c.client_order_id)] = o.id;

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
        if (replaying_) {
            return;  // nobody is subscribed during recovery
        }
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
            const auto it = md_.find(session);
            if (it == md_.end() || it->second == nullptr) {
                continue;
            }
            // Honor the depth the client asked for. Sending more than requested
            // is not a harmless generosity: the client sized its book for
            // `depth` levels and the extra ones are bandwidth it did not want.
            BookSnapshot sized = snap;
            if (depth < snap.n_bids) sized.n_bids = depth;
            if (depth < snap.n_asks) sized.n_asks = depth;
            if (!it->second->push(sized)) {
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
        if (replaying_) {
            // The session this belongs to does not exist yet. Suppressed here,
            // in one place, rather than by threading a flag through apply() —
            // which would make the replay path structurally different from the
            // live one and defeat the point of sharing it.
            return;
        }
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

    // O(1). This used to scan every order the session held, doing a hash lookup
    // per element, for EVERY cancel and modify — a session with ten thousand
    // resting orders paid ten thousand lookups to cancel one of them.
    [[nodiscard]] std::uint64_t find_order(SessionId s, std::uint64_t client_order_id) const {
        const auto it = by_client_id_.find(key_of(s, client_order_id));
        return it == by_client_id_.end() ? 0 : it->second;
    }

    // Session and client_order_id packed into one key. client_order_id is
    // client-chosen and only unique within a session, so the session must be
    // part of the key or two clients numbering from 1 would collide.
    [[nodiscard]] static std::uint64_t key_of(SessionId s, std::uint64_t coid) noexcept {
        return (static_cast<std::uint64_t>(s) << 40) ^ (coid * 0x9E3779B97F4A7C15ULL);
    }

    void forget_order(std::uint64_t xoid) {
        const auto owner = owner_of_.find(xoid);
        if (owner != owner_of_.end()) {
            const auto ci = client_id_of_.find(xoid);
            if (ci != client_id_of_.end()) {
                by_client_id_.erase(key_of(owner->second, ci->second));
            }
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
    Notifier* egress_ready_{nullptr};
    Wal* wal_{nullptr};
    std::thread thread_;
    std::atomic<bool> running_{false};
    // Set only inside recover(), on the calling thread, before start().
    bool replaying_{false};
    std::size_t recovered_{0};

    std::unordered_map<SessionId, EgressQueue*> egress_;
    std::unordered_map<SessionId, MarketDataQueue*> md_;
    std::unordered_map<SessionId, std::uint8_t> subscribers_;
    std::unordered_map<std::uint64_t, SessionId> owner_of_;
    std::unordered_map<std::uint64_t, std::uint64_t> client_id_of_;
    std::unordered_map<std::uint64_t, std::uint32_t> remaining_of_;
    std::unordered_map<std::uint64_t, Order::Side> side_of_;
    std::unordered_map<SessionId, std::unordered_set<std::uint64_t>> session_orders_;
    // (session, client_order_id) -> exchange_order_id
    std::unordered_map<std::uint64_t, std::uint64_t> by_client_id_;

    RiskConfig risk_;
    SnapshotPolicy snap_;
    std::uint64_t next_snapshot_at_{0};
    std::uint64_t last_snapshot_seq_{0};
    // Reference for the price band. 0 means "no trade yet", which falls back to
    // the mid and then to accepting anything.
    std::int64_t last_trade_ticks_{0};
    std::uint64_t book_seq_{0};
    std::uint64_t next_exchange_id_{1};
    std::uint64_t logical_clock_{0};
    MatchingStats stats_;
};

}  // namespace ome
