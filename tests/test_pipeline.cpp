// The full pipeline: sockets -> SPSC queue -> matching thread -> egress -> sockets.
//
// This is the file that proves the architecture, so it runs the REAL matching
// thread rather than a stub. Run it under -DOME_SANITIZE=thread: the whole
// point of the design is that the only shared state is the two queues, and a
// TSan-clean run over cross-session trading is the evidence.

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "ome/commands.hpp"
#include "ome/matching_thread.hpp"
#include "ome/protocol.hpp"
#include "ome/risk_config.hpp"
#include "ome/tcp_server.hpp"
#include "ome/waiter.hpp"

using namespace ome;
using namespace ome::protocol;

namespace {

// Server + matching thread, the production shape.
class Gateway {
public:
    Gateway() {
        matcher_ = std::make_unique<MatchingThread>(inbound_, waiter_);
        TcpServerConfig cfg{};
        cfg.port = 0;
        cfg.poll_timeout_ms = 5;
        server_ = std::make_unique<TcpServer>(cfg, &inbound_, &waiter_);
        started_ = server_->start();
        if (started_) {
            port_ = server_->bound_port();
            matcher_->start();
            net_ = std::thread([this] { server_->run(); });
        }
    }

    ~Gateway() {
        if (started_) {
            server_->stop();
            if (net_.joinable()) net_.join();
            matcher_->stop();
        }
    }

    [[nodiscard]] bool started() const { return started_; }
    [[nodiscard]] std::uint16_t port() const { return port_; }

private:
    InboundQueue inbound_;
    Waiter waiter_;
    std::unique_ptr<MatchingThread> matcher_;
    std::unique_ptr<TcpServer> server_;
    std::thread net_;
    bool started_{false};
    std::uint16_t port_{0};
};

int dial(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0) {
        ::close(fd);
        return -1;
    }
    timeval tv{};
    tv.tv_sec = 5;
    static_cast<void>(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)));
    return fd;
}

bool read_exactly(int fd, std::uint8_t* out, std::size_t n) {
    std::size_t got = 0;
    while (got < n) {
        const ssize_t r = ::recv(fd, out + got, n - got, 0);
        if (r <= 0) return false;
        got += static_cast<std::size_t>(r);
    }
    return true;
}

struct Msg {
    MessageHeader header;
    std::vector<std::uint8_t> payload;
};

bool next_msg(int fd, Msg& m) {
    std::uint8_t hb[kHeaderSize];
    if (!read_exactly(fd, hb, kHeaderSize)) return false;
    const auto h = decode_header(hb, kHeaderSize);
    if (!h.has_value()) return false;
    m.header = *h;
    m.payload.resize(h->length);
    return h->length == 0 || read_exactly(fd, m.payload.data(), h->length);
}

// Reads until a message of the wanted type arrives, skipping heartbeats.
bool next_of_type(int fd, MessageType want, Msg& m, int budget = 40) {
    for (int i = 0; i < budget; ++i) {
        if (!next_msg(fd, m)) return false;
        if (static_cast<MessageType>(m.header.type) == want) return true;
    }
    return false;
}

void send_order(int fd, std::uint64_t coid, std::int64_t px, std::uint32_t qty, Side side,
                OrderType type = OrderType::Limit) {
    NewOrder m{};
    m.client_order_id = coid;
    m.price_ticks = px;
    m.quantity = qty;
    m.side = side;
    m.order_type = type;
    const auto f = encode_frame(MessageType::NewOrder, m);
    ASSERT_EQ(::send(fd, f.data(), f.size(), 0), static_cast<ssize_t>(f.size()));
}

}  // namespace

TEST(Pipeline, order_reaches_the_engine_and_the_ack_comes_back) {
    Gateway g;
    ASSERT_TRUE(g.started());
    const int fd = dial(g.port());
    ASSERT_GE(fd, 0);

    send_order(fd, 1, 1000000, 10, Side::Bid);

    Msg m{};
    ASSERT_TRUE(next_of_type(fd, MessageType::Ack, m));
    const auto ack = decode<Ack>(m.payload.data(), m.payload.size());
    ASSERT_TRUE(ack.has_value());
    EXPECT_EQ(ack->client_order_id, 1u);
    // The exchange id is assigned by the MATCHING thread, not fabricated on the
    // network side as it was in 1.2/1.3.
    EXPECT_GT(ack->exchange_order_id, 0u);
    ::close(fd);
}

TEST(Pipeline, cross_session_fill_routes_to_both_sides) {
    // THE test for the ownership map. A Trade carries order ids, not session
    // ids; without owner_of_ there is no way to decide which socket each side
    // of a fill belongs to.
    Gateway g;
    ASSERT_TRUE(g.started());

    const int maker = dial(g.port());
    const int taker = dial(g.port());
    ASSERT_GE(maker, 0);
    ASSERT_GE(taker, 0);

    // maker rests an ask
    send_order(maker, 100, 1000000, 10, Side::Ask);
    Msg m{};
    ASSERT_TRUE(next_of_type(maker, MessageType::Ack, m));

    // taker crosses it
    send_order(taker, 200, 1000000, 4, Side::Bid);
    ASSERT_TRUE(next_of_type(taker, MessageType::Ack, m));
    EXPECT_EQ(decode<Ack>(m.payload.data(), m.payload.size())->client_order_id, 200u);

    // taker's fill: fully filled, nothing remaining
    ASSERT_TRUE(next_of_type(taker, MessageType::Fill, m)) << "taker never got a fill";
    const auto tf = decode<Fill>(m.payload.data(), m.payload.size());
    ASSERT_TRUE(tf.has_value());
    EXPECT_EQ(tf->price_ticks, 1000000);
    EXPECT_EQ(tf->quantity, 4u);
    EXPECT_EQ(tf->remaining_quantity, 0u);

    // maker's fill: same trade, but 6 of its 10 still rest
    ASSERT_TRUE(next_of_type(maker, MessageType::Fill, m)) << "maker never got a fill";
    const auto mf = decode<Fill>(m.payload.data(), m.payload.size());
    ASSERT_TRUE(mf.has_value());
    EXPECT_EQ(mf->price_ticks, 1000000);
    EXPECT_EQ(mf->quantity, 4u);
    EXPECT_EQ(mf->remaining_quantity, 6u) << "maker's remaining quantity is wrong";

    ::close(maker);
    ::close(taker);
}

TEST(Pipeline, ack_arrives_before_fill) {
    // Protocol commitment: the engine rests an aggressive limit and then matches
    // it, so a marketable order is acked before it is filled. Clients model
    // their own state on this ordering.
    Gateway g;
    ASSERT_TRUE(g.started());
    const int maker = dial(g.port());
    const int taker = dial(g.port());
    ASSERT_GE(maker, 0);
    ASSERT_GE(taker, 0);

    send_order(maker, 1, 1000000, 5, Side::Ask);
    Msg m{};
    ASSERT_TRUE(next_of_type(maker, MessageType::Ack, m));

    send_order(taker, 2, 1000000, 5, Side::Bid);

    // first non-heartbeat message on the taker must be the Ack
    std::vector<MessageType> seen;
    for (int i = 0; i < 20 && seen.size() < 2; ++i) {
        ASSERT_TRUE(next_msg(taker, m));
        const auto t = static_cast<MessageType>(m.header.type);
        if (t == MessageType::Heartbeat) continue;
        seen.push_back(t);
    }
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[0], MessageType::Ack) << "Fill preceded Ack";
    EXPECT_EQ(seen[1], MessageType::Fill);

    ::close(maker);
    ::close(taker);
}

TEST(Pipeline, cancel_is_decided_by_the_book_not_the_network_thread) {
    Gateway g;
    ASSERT_TRUE(g.started());
    const int fd = dial(g.port());
    ASSERT_GE(fd, 0);

    send_order(fd, 7, 1000000, 10, Side::Bid);
    Msg m{};
    ASSERT_TRUE(next_of_type(fd, MessageType::Ack, m));

    const auto c = encode_frame(MessageType::Cancel, Cancel{7});
    ASSERT_EQ(::send(fd, c.data(), c.size(), 0), static_cast<ssize_t>(c.size()));
    ASSERT_TRUE(next_of_type(fd, MessageType::Ack, m));
    EXPECT_EQ(decode<Ack>(m.payload.data(), m.payload.size())->client_order_id, 7u);

    // cancelling it again must be rejected by the engine
    ASSERT_EQ(::send(fd, c.data(), c.size(), 0), static_cast<ssize_t>(c.size()));
    ASSERT_TRUE(next_of_type(fd, MessageType::Reject, m));
    EXPECT_EQ(decode<Reject>(m.payload.data(), m.payload.size())->reason,
              RejectReason::UNKNOWN_ORDER);
    ::close(fd);
}

TEST(Pipeline, unknown_cancel_is_rejected_by_the_engine) {
    Gateway g;
    ASSERT_TRUE(g.started());
    const int fd = dial(g.port());
    ASSERT_GE(fd, 0);

    const auto c = encode_frame(MessageType::Cancel, Cancel{999999});
    ASSERT_EQ(::send(fd, c.data(), c.size(), 0), static_cast<ssize_t>(c.size()));

    Msg m{};
    ASSERT_TRUE(next_of_type(fd, MessageType::Reject, m));
    EXPECT_EQ(decode<Reject>(m.payload.data(), m.payload.size())->reason,
              RejectReason::UNKNOWN_ORDER);
    ::close(fd);
}

TEST(Pipeline, modify_loses_priority_and_gets_a_new_exchange_id) {
    // Modify is cancel + new applied as a unit on the matching thread. The new
    // exchange_order_id is the observable evidence that queue position moved,
    // which docs/PROTOCOL.md commits to.
    Gateway g;
    ASSERT_TRUE(g.started());
    const int fd = dial(g.port());
    ASSERT_GE(fd, 0);

    send_order(fd, 11, 1000000, 10, Side::Bid);
    Msg m{};
    ASSERT_TRUE(next_of_type(fd, MessageType::Ack, m));
    const std::uint64_t first_id = decode<Ack>(m.payload.data(), m.payload.size())->exchange_order_id;

    Modify mod{};
    mod.client_order_id = 11;
    mod.new_price_ticks = 1010000;
    mod.new_quantity = 10;
    const auto f = encode_frame(MessageType::Modify, mod);
    ASSERT_EQ(::send(fd, f.data(), f.size(), 0), static_cast<ssize_t>(f.size()));

    ASSERT_TRUE(next_of_type(fd, MessageType::Ack, m));
    const auto ack = decode<Ack>(m.payload.data(), m.payload.size());
    ASSERT_TRUE(ack.has_value());
    EXPECT_EQ(ack->client_order_id, 11u);
    EXPECT_NE(ack->exchange_order_id, first_id) << "reprice kept its exchange id";
    ::close(fd);
}

TEST(Pipeline, disconnect_cancels_resting_orders_through_the_command_path) {
    // Cancel-on-disconnect, for real this time: the stub is gone and the
    // cancels are performed by the matching thread from a CancelAllForSession
    // command travelling the same queue as everything else.
    Gateway g;
    ASSERT_TRUE(g.started());

    const int maker = dial(g.port());
    ASSERT_GE(maker, 0);
    send_order(maker, 1, 1000000, 10, Side::Ask);
    Msg m{};
    ASSERT_TRUE(next_of_type(maker, MessageType::Ack, m));
    ::close(maker);  // vanish while holding a resting ask

    // Give the command time to be applied.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // A new session crossing the old price must find nothing: the book is empty.
    const int taker = dial(g.port());
    ASSERT_GE(taker, 0);
    send_order(taker, 2, 1000000, 10, Side::Bid);
    ASSERT_TRUE(next_of_type(taker, MessageType::Ack, m));

    // No fill should follow. Read a few messages; heartbeats are fine, a Fill
    // means the dead session's order was still resting.
    for (int i = 0; i < 3; ++i) {
        Msg extra{};
        if (!next_msg(taker, extra)) break;
        EXPECT_NE(static_cast<MessageType>(extra.header.type), MessageType::Fill)
            << "a disconnected session's order was still matchable";
    }
    ::close(taker);
}

TEST(Pipeline, sustained_two_way_trading_stays_consistent) {
    // Volume, so TSan has real interleavings to inspect rather than a single
    // tidy handoff. Every taker order crosses a resting maker order, so the
    // fill count is deterministic even though the timing is not.
    Gateway g;
    ASSERT_TRUE(g.started());

    const int maker = dial(g.port());
    const int taker = dial(g.port());
    ASSERT_GE(maker, 0);
    ASSERT_GE(taker, 0);

    constexpr int kRounds = 200;
    std::atomic<int> maker_fills{0};

    // Drain the maker's socket concurrently, so its egress queue keeps moving
    // and the network thread is genuinely interleaved with the matcher.
    std::thread maker_reader([&] {
        Msg m{};
        while (maker_fills.load() < kRounds) {
            if (!next_msg(maker, m)) return;
            if (static_cast<MessageType>(m.header.type) == MessageType::Fill) {
                maker_fills.fetch_add(1);
            }
        }
    });

    for (int i = 0; i < kRounds; ++i) {
        const auto coid = static_cast<std::uint64_t>(i + 1);
        send_order(maker, coid, 1000000, 1, Side::Ask);
        send_order(taker, coid + 100000, 1000000, 1, Side::Bid);
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (maker_fills.load() < kRounds && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    maker_reader.join();

    EXPECT_EQ(maker_fills.load(), kRounds) << "lost fills under sustained load";
    ::close(maker);
    ::close(taker);
}

// --- risk checks, end to end (session 1.5) ---------------------------------
//
// These run through the real matching thread because the price band is defined
// against the last trade or the mid. There is no way to test it without a book,
// which is precisely the argument for where the check lives.

namespace {

class RiskGateway {
public:
    explicit RiskGateway(RiskConfig risk) {
        matcher_ = std::make_unique<MatchingThread>(inbound_, waiter_, risk);
        TcpServerConfig cfg{};
        cfg.port = 0;
        cfg.poll_timeout_ms = 5;
        cfg.risk = risk;
        server_ = std::make_unique<TcpServer>(cfg, &inbound_, &waiter_);
        started_ = server_->start();
        if (started_) {
            port_ = server_->bound_port();
            matcher_->start();
            net_ = std::thread([this] { server_->run(); });
        }
    }
    ~RiskGateway() {
        if (started_) {
            server_->stop();
            if (net_.joinable()) net_.join();
            matcher_->stop();
        }
    }
    [[nodiscard]] bool started() const { return started_; }
    [[nodiscard]] std::uint16_t port() const { return port_; }

private:
    InboundQueue inbound_;
    Waiter waiter_;
    std::unique_ptr<MatchingThread> matcher_;
    std::unique_ptr<TcpServer> server_;
    std::thread net_;
    bool started_{false};
    std::uint16_t port_{0};
};

RejectReason reason_of(int fd, Msg& m) {
    EXPECT_TRUE(next_of_type(fd, MessageType::Reject, m));
    const auto r = decode<Reject>(m.payload.data(), m.payload.size());
    return r.has_value() ? r->reason : RejectReason::NONE;
}

}  // namespace

TEST(Risk, quantity_limit_at_both_boundaries) {
    RiskConfig risk{};
    risk.max_order_qty = 100;
    RiskGateway g(risk);
    ASSERT_TRUE(g.started());
    const int fd = dial(g.port());
    ASSERT_GE(fd, 0);
    Msg m{};

    send_order(fd, 1, 1000000, 100, Side::Bid);  // exactly at the cap
    ASSERT_TRUE(next_of_type(fd, MessageType::Ack, m)) << "the cap itself was rejected";

    send_order(fd, 2, 1000000, 101, Side::Bid);  // one over
    EXPECT_EQ(reason_of(fd, m), RejectReason::RISK_MAX_ORDER_SIZE);
    ::close(fd);
}

TEST(Risk, zero_quantity_is_invalid_not_a_size_breach) {
    RiskGateway g(RiskConfig{});
    ASSERT_TRUE(g.started());
    const int fd = dial(g.port());
    ASSERT_GE(fd, 0);
    Msg m{};
    send_order(fd, 1, 1000000, 0, Side::Bid);
    EXPECT_EQ(reason_of(fd, m), RejectReason::INVALID_QTY);
    ::close(fd);
}

TEST(Risk, non_positive_price_is_rejected) {
    RiskGateway g(RiskConfig{});
    ASSERT_TRUE(g.started());
    const int fd = dial(g.port());
    ASSERT_GE(fd, 0);
    Msg m{};
    send_order(fd, 1, 0, 10, Side::Bid);
    EXPECT_EQ(reason_of(fd, m), RejectReason::INVALID_PRICE);
    send_order(fd, 2, -5000, 10, Side::Bid);
    EXPECT_EQ(reason_of(fd, m), RejectReason::INVALID_PRICE);
    ::close(fd);
}

TEST(Risk, tick_alignment_is_exact_integer_modulo) {
    RiskConfig risk{};
    risk.tick_size = 100;  // prices must be whole cents
    RiskGateway g(risk);
    ASSERT_TRUE(g.started());
    const int fd = dial(g.port());
    ASSERT_GE(fd, 0);
    Msg m{};

    send_order(fd, 1, 1000000, 10, Side::Bid);  // 1000000 % 100 == 0
    ASSERT_TRUE(next_of_type(fd, MessageType::Ack, m));

    send_order(fd, 2, 1000001, 10, Side::Bid);  // one tick off
    EXPECT_EQ(reason_of(fd, m), RejectReason::INVALID_PRICE);
    ::close(fd);
}

TEST(Risk, price_band_needs_the_book_and_only_applies_once_there_is_a_reference) {
    // THE test for the two-layer design. The band is meaningless until the book
    // has a reference price, so the same order is accepted before a trade and
    // rejected after one. No network-thread check could produce that behavior,
    // because the network thread cannot see the book.
    RiskConfig risk{};
    risk.price_band_bp = 1000;  // 10%
    RiskGateway g(risk);
    ASSERT_TRUE(g.started());

    const int a = dial(g.port());
    const int b = dial(g.port());
    ASSERT_GE(a, 0);
    ASSERT_GE(b, 0);
    Msg m{};

    // Empty book, no trades: a wild price has nothing to be far FROM, and
    // rejecting it would refuse the first order ever placed.
    send_order(a, 1, 5000000, 10, Side::Bid);
    ASSERT_TRUE(next_of_type(a, MessageType::Ack, m)) << "rejected with no reference price";

    // Establish a trade at 5000000.
    send_order(b, 100, 5000000, 10, Side::Ask);
    ASSERT_TRUE(next_of_type(b, MessageType::Ack, m));
    ASSERT_TRUE(next_of_type(b, MessageType::Fill, m)) << "no trade, so no reference established";

    // Now the same kind of far-away order must be refused: 10% of 5000000 is
    // 500000, so 5500000 is exactly at the edge and 5500001 is over it.
    send_order(a, 2, 5500000, 10, Side::Bid);
    ASSERT_TRUE(next_of_type(a, MessageType::Ack, m)) << "the band edge itself was rejected";

    send_order(a, 3, 5500001, 10, Side::Bid);
    EXPECT_EQ(reason_of(a, m), RejectReason::RISK_PRICE_BAND);

    send_order(a, 4, 1000000, 10, Side::Bid);  // far below
    EXPECT_EQ(reason_of(a, m), RejectReason::RISK_PRICE_BAND);

    ::close(a);
    ::close(b);
}

TEST(Risk, market_orders_skip_price_checks) {
    // A market order carries no meaningful price; it takes whatever is there.
    RiskConfig risk{};
    risk.tick_size = 100;
    risk.price_band_bp = 1;  // absurdly tight, to prove it is not consulted
    RiskGateway g(risk);
    ASSERT_TRUE(g.started());
    const int maker = dial(g.port());
    const int taker = dial(g.port());
    ASSERT_GE(maker, 0);
    ASSERT_GE(taker, 0);
    Msg m{};

    send_order(maker, 1, 1000000, 10, Side::Ask);
    ASSERT_TRUE(next_of_type(maker, MessageType::Ack, m));

    send_order(taker, 2, 0, 5, Side::Bid, OrderType::Market);
    ASSERT_TRUE(next_of_type(taker, MessageType::Ack, m)) << "market order failed a price check";
    EXPECT_TRUE(next_of_type(taker, MessageType::Fill, m));
    ::close(maker);
    ::close(taker);
}

TEST(Risk, rate_limit_refuses_a_burst_on_the_network_thread) {
    // The limiter is on the network thread, so an over-limit client never
    // reaches the queue at all. Observable here only as a Reject, but the point
    // is what did NOT happen: no matching-thread work was done.
    RiskConfig risk{};
    risk.rate_per_sec = 1.0;   // effectively no refill during the test
    risk.rate_burst = 5.0;
    RiskGateway g(risk);
    ASSERT_TRUE(g.started());
    const int fd = dial(g.port());
    ASSERT_GE(fd, 0);
    Msg m{};

    int accepted = 0;
    bool saw_rate_limit = false;
    for (std::uint64_t i = 1; i <= 20; ++i) {
        send_order(fd, i, 1000000, 1, Side::Bid);
    }
    for (int i = 0; i < 20; ++i) {
        if (!next_msg(fd, m)) break;
        const auto t = static_cast<MessageType>(m.header.type);
        if (t == MessageType::Ack) {
            ++accepted;
        } else if (t == MessageType::Reject) {
            const auto r = decode<Reject>(m.payload.data(), m.payload.size());
            if (r && r->reason == RejectReason::RATE_LIMITED) {
                saw_rate_limit = true;
                break;
            }
        }
    }
    EXPECT_TRUE(saw_rate_limit) << "burst of 20 against a capacity-5 bucket was never limited";
    EXPECT_LE(accepted, 5) << "accepted more than the burst capacity";
    ::close(fd);
}

TEST(Risk, heartbeats_are_exempt_from_the_rate_limit) {
    // Throttling a client's liveness signal would time it out for being chatty.
    RiskConfig risk{};
    risk.rate_per_sec = 1.0;
    risk.rate_burst = 1.0;
    RiskGateway g(risk);
    ASSERT_TRUE(g.started());
    const int fd = dial(g.port());
    ASSERT_GE(fd, 0);

    const auto hb = encode_frame(MessageType::Heartbeat, Heartbeat{123});
    for (int i = 0; i < 50; ++i) {
        ASSERT_EQ(::send(fd, hb.data(), hb.size(), 0), static_cast<ssize_t>(hb.size()));
    }
    // Exhaust the single token, then confirm an order still gets through the
    // refill rather than the session having been throttled to death.
    Msg m{};
    send_order(fd, 1, 1000000, 1, Side::Bid);
    bool got_reply = false;
    for (int i = 0; i < 10 && !got_reply; ++i) {
        if (!next_msg(fd, m)) break;
        const auto t = static_cast<MessageType>(m.header.type);
        got_reply = (t == MessageType::Ack || t == MessageType::Reject);
    }
    EXPECT_TRUE(got_reply) << "session was starved by its own heartbeats";
    ::close(fd);
}

// --- market data broadcast, end to end (session 1.6) -----------------------

namespace {
void subscribe(int fd, std::uint8_t depth = 10) {
    Subscribe s{};
    s.depth = depth;
    const auto f = encode_frame(MessageType::Subscribe, s);
    ASSERT_EQ(::send(fd, f.data(), f.size(), 0), static_cast<ssize_t>(f.size()));
}
}  // namespace

TEST(Broadcast, a_subscriber_receives_book_updates) {
    Gateway g;
    ASSERT_TRUE(g.started());
    const int sub = dial(g.port());
    const int trader = dial(g.port());
    ASSERT_GE(sub, 0);
    ASSERT_GE(trader, 0);

    subscribe(sub);
    Msg m{};
    // Subscribing publishes current state immediately, so a fresh subscriber
    // is not blind until the next trade.
    ASSERT_TRUE(next_of_type(sub, MessageType::BookUpdate, m)) << "no snapshot on subscribe";

    send_order(trader, 1, 1000000, 10, Side::Bid);
    ASSERT_TRUE(next_of_type(trader, MessageType::Ack, m));

    ASSERT_TRUE(next_of_type(sub, MessageType::BookUpdate, m)) << "no update after a book change";
    const auto up = decode<BookUpdate>(m.payload.data(), m.payload.size());
    ASSERT_TRUE(up.has_value());
    ASSERT_FALSE(up->bids.empty()) << "update did not show the resting bid";
    EXPECT_EQ(up->bids[0].price_ticks, 1000000);
    EXPECT_EQ(up->bids[0].quantity, 10u);
    ::close(sub);
    ::close(trader);
}

TEST(Broadcast, an_unsubscribed_session_receives_nothing) {
    Gateway g;
    ASSERT_TRUE(g.started());
    const int quiet = dial(g.port());
    const int trader = dial(g.port());
    ASSERT_GE(quiet, 0);
    ASSERT_GE(trader, 0);

    send_order(trader, 1, 1000000, 10, Side::Bid);
    Msg m{};
    ASSERT_TRUE(next_of_type(trader, MessageType::Ack, m));

    // The quiet session should see only heartbeats.
    for (int i = 0; i < 3; ++i) {
        Msg extra{};
        if (!next_msg(quiet, extra)) break;
        EXPECT_NE(static_cast<MessageType>(extra.header.type), MessageType::BookUpdate)
            << "an unsubscribed session received market data";
    }
    ::close(quiet);
    ::close(trader);
}

TEST(Broadcast, two_subscribers_see_the_same_book) {
    Gateway g;
    ASSERT_TRUE(g.started());
    const int a = dial(g.port());
    const int b = dial(g.port());
    const int trader = dial(g.port());
    ASSERT_GE(a, 0);
    ASSERT_GE(b, 0);
    ASSERT_GE(trader, 0);

    subscribe(a);
    subscribe(b);
    Msg m{};
    ASSERT_TRUE(next_of_type(a, MessageType::BookUpdate, m));
    ASSERT_TRUE(next_of_type(b, MessageType::BookUpdate, m));

    send_order(trader, 1, 1234500, 7, Side::Ask);
    ASSERT_TRUE(next_of_type(trader, MessageType::Ack, m));

    // Read until the update that reflects the order, rather than assuming it is
    // the very next one. Subscribing the SECOND client publishes another
    // snapshot to BOTH, so the first client can have an intervening empty
    // update queued — and conflation decides how many of those survive, which
    // depends on when the network thread happened to drain. Asserting on "the
    // next BookUpdate" made this test fail under ThreadSanitizer, where the
    // timing differs, while passing everywhere else.
    const auto await_ask = [](int fd, std::int64_t want) -> bool {
        Msg m{};
        for (int i = 0; i < 20; ++i) {
            if (!next_of_type(fd, MessageType::BookUpdate, m)) return false;
            const auto up = decode<BookUpdate>(m.payload.data(), m.payload.size());
            if (up && !up->asks.empty() && up->asks[0].price_ticks == want) return true;
        }
        return false;
    };
    EXPECT_TRUE(await_ask(a, 1234500)) << "subscriber a never saw the resting ask";
    EXPECT_TRUE(await_ask(b, 1234500)) << "subscriber b never saw the resting ask";
    ::close(a);
    ::close(b);
    ::close(trader);
}

TEST(Broadcast, updates_carry_a_strictly_increasing_sequence) {
    // seq is the ordering key. Conflation skips values, so it must be
    // increasing rather than contiguous — a client detecting "gaps" as an error
    // would flag normal operation.
    Gateway g;
    ASSERT_TRUE(g.started());
    const int sub = dial(g.port());
    const int trader = dial(g.port());
    ASSERT_GE(sub, 0);
    ASSERT_GE(trader, 0);

    subscribe(sub);
    Msg m{};
    ASSERT_TRUE(next_of_type(sub, MessageType::BookUpdate, m));

    std::uint64_t prev = 0;
    for (std::uint64_t i = 1; i <= 5; ++i) {
        send_order(trader, i, 1000000 + static_cast<std::int64_t>(i) * 100, 1, Side::Bid);
        ASSERT_TRUE(next_of_type(trader, MessageType::Ack, m));
        ASSERT_TRUE(next_of_type(sub, MessageType::BookUpdate, m));
        const auto up = decode<BookUpdate>(m.payload.data(), m.payload.size());
        ASSERT_TRUE(up.has_value());
        EXPECT_GT(up->seq, prev) << "sequence did not advance";
        prev = up->seq;
    }
    ::close(sub);
    ::close(trader);
}

TEST(Broadcast, subscribe_depth_is_honored) {
    // REGRESSION: the requested depth was stored and then ignored, so a client
    // asking for 3 levels received 10.
    Gateway g;
    ASSERT_TRUE(g.started());
    const int sub = dial(g.port());
    const int trader = dial(g.port());
    ASSERT_GE(sub, 0);
    ASSERT_GE(trader, 0);

    subscribe(sub, 3);
    Msg m{};
    ASSERT_TRUE(next_of_type(sub, MessageType::BookUpdate, m));

    for (std::uint64_t i = 1; i <= 6; ++i) {
        send_order(trader, i, 1000000 - static_cast<std::int64_t>(i) * 100, 1, Side::Bid);
        ASSERT_TRUE(next_of_type(trader, MessageType::Ack, m));
    }
    // Drain to the newest update; conflation may have skipped intermediates.
    BookUpdate last{};
    for (int i = 0; i < 12; ++i) {
        Msg u{};
        if (!next_msg(sub, u)) break;
        if (static_cast<MessageType>(u.header.type) != MessageType::BookUpdate) continue;
        const auto up = decode<BookUpdate>(u.payload.data(), u.payload.size());
        if (up) last = *up;
        if (last.bids.size() >= 3) break;
    }
    EXPECT_LE(last.bids.size(), 3u) << "sent more levels than the client asked for";
    ::close(sub);
    ::close(trader);
}
