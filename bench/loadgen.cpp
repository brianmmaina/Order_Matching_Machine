// Multi-client latency load generator.
//
// Measures ORDER-TO-ACK latency: the time from a client writing a NewOrder to
// that client reading the corresponding Ack. Client-side, monotonic clock.
//
// OPEN LOOP, NOT CLOSED LOOP. A closed-loop generator sends the next order only
// after the previous ack arrives, which caps offered load at 1/latency per
// client and makes it impossible to load the server past that point — the
// measurement silently becomes "how fast is a round trip" rather than "what is
// latency under load". Here each client sends on a schedule regardless of what
// has come back, so queueing delay is visible, which is the entire point of a
// p99.
//
// WORKER POOL, NOT A THREAD PER CLIENT. This machine has 12 cores; 100 client
// threads would oversubscribe it 8x and the resulting p99 would be measuring
// the load generator's own scheduling, not the gateway. Each worker drives a
// share of the connections through one poll() loop, so thread count stays at
// or below core count and the generator is not the bottleneck.
//
// WHAT IS IN THE TIMED PATH: serialization, the loopback socket write, the
// server's read and framing, the risk checks, the SPSC hop, the matching
// thread's apply, the egress hop, the server's write, and the client's read
// and parse. What is NOT: order construction and the client's own map lookup
// on send.
//
// The honesty requirements from docs/BENCHMARK.md apply to every number this
// prints: loopback is not a network, this is one machine, and client-side
// measurement includes client-side scheduling noise.

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "ome/frame_reader.hpp"
#include "ome/protocol.hpp"

namespace {

using Clock = std::chrono::steady_clock;

// Upper bound on how long a worker will sit in poll(). Small enough that the
// generator's own sleep never dominates a latency sample.
constexpr int kMaxPollBlockMs = 1;

// Worker threads by default. Four leaves ample room for the gateway's network
// and matching threads on a 12-core machine; --workers overrides it.
constexpr int kDefaultWorkers = 4;

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
            .count());
}

struct Config {
    std::string host = "127.0.0.1";
    std::uint16_t port = 9001;
    int clients = 10;
    double rate_per_client = 100.0;  // orders/sec/client
    double duration_s = 10.0;
    double warmup_s = 2.0;
    int workers = 0;  // 0 = auto
    bool csv = false;
    // Quote crossing prices so orders actually match. Off by default because
    // the latency sweep wants orders to rest (a crossing workload also returns
    // fills, changing both the work done and the message count). On for the
    // kill test, which has to exercise the matching path or it only verifies
    // that two implementations can both accumulate a book.
    bool cross = false;
};

struct Sample {
    std::vector<std::int64_t> latencies_ns;
    std::uint64_t sent = 0;
    std::uint64_t acked = 0;
    std::uint64_t rejected = 0;
    std::uint64_t send_failed = 0;
};

int dial(const std::string& host, std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1) {
        ::close(fd);
        return -1;
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0) {
        ::close(fd);
        return -1;
    }
    // TCP_NODELAY on the CLIENT too. Without it Nagle holds a small write
    // waiting to coalesce with the next one, injecting up to 40ms of delay that
    // would be attributed to the server.
    const int one = 1;
    static_cast<void>(::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)));
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) static_cast<void>(::fcntl(fd, F_SETFL, flags | O_NONBLOCK));
    return fd;
}

// One connection's state, driven by a worker's poll loop.
struct Client {
    int fd = -1;
    ome::FrameReader reader;
    std::uint64_t next_coid = 1;
    std::uint64_t next_send_ns = 0;
    // client_order_id -> send timestamp. Reserved up front so a rehash never
    // lands inside the timed path.
    std::unordered_map<std::uint64_t, std::uint64_t> in_flight;
    std::vector<std::uint8_t> outbuf;
};

void run_worker(const Config& cfg, std::vector<Client>& clients, std::uint64_t start_ns,
                std::uint64_t warmup_end_ns, std::uint64_t end_ns, Sample& out) {
    const auto interval_ns = static_cast<std::uint64_t>(1e9 / cfg.rate_per_client);
    std::vector<pollfd> pfds(clients.size());

    for (auto& c : clients) {
        c.next_send_ns = start_ns;
        c.in_flight.reserve(4096);
    }

    for (;;) {
        const std::uint64_t t = now_ns();
        if (t >= end_ns) break;

        // --- paced sends ---
        std::uint64_t earliest_due = end_ns;
        for (auto& c : clients) {
            if (c.fd < 0) continue;
            while (c.next_send_ns <= t) {
                ome::protocol::NewOrder m{};
                m.client_order_id = c.next_coid++;
                // Alternate sides around a fixed price so orders rest and
                // cross rather than piling up on one side, which would make
                // the book grow without bound and change what is measured.
                const bool bid = (m.client_order_id % 2) == 0;
                // resting: bid below ask, nothing matches.
                // crossing: bid ABOVE ask, so every pair trades.
                m.price_ticks = cfg.cross ? (1000000 + (bid ? 100 : -100))
                                          : (1000000 + (bid ? -100 : 100));
                // Under --cross, vary size so pairs only partially fill. Equal
                // sizes would trade away completely and leave an empty book,
                // and a digest comparison on an empty book verifies very little.
                m.quantity = cfg.cross
                                 ? static_cast<std::uint32_t>(5 + (m.client_order_id % 7) * 3)
                                 : 10;
                m.side = bid ? ome::protocol::Side::Bid : ome::protocol::Side::Ask;
                m.order_type = ome::protocol::OrderType::Limit;

                const auto frame = ome::protocol::encode_frame(
                    ome::protocol::MessageType::NewOrder, m);
                // Timestamp as late as possible: after serialization, right
                // before the syscall.
                const std::uint64_t send_ts = now_ns();
                const ssize_t n = ::send(c.fd, frame.data(), frame.size(), 0);
                if (n == static_cast<ssize_t>(frame.size())) {
                    c.in_flight[m.client_order_id] = send_ts;
                    ++out.sent;
                } else {
                    // A partial or refused send means the client's own socket
                    // buffer is full: the generator could not offer the load.
                    // Counted rather than retried, because retrying would bias
                    // the latency of whatever eventually got through.
                    ++out.send_failed;
                }
                c.next_send_ns += interval_ns;
                // Do not try to catch up on a large backlog: that produces a
                // burst that measures the recovery, not the target rate.
                if (c.next_send_ns + interval_ns * 100 < t) {
                    c.next_send_ns = t + interval_ns;
                }
            }
            earliest_due = std::min(earliest_due, c.next_send_ns);
        }

        // --- reads ---
        pfds.clear();
        for (auto& c : clients) {
            pollfd p{};
            p.fd = c.fd;
            p.events = POLLIN;
            pfds.push_back(p);
        }
        // Cap the block hard. poll()'s resolution is milliseconds, so a worker
        // that sleeps until its next send is due cannot notice an ack sooner
        // than that granularity allows — and at low offered rates the next send
        // is milliseconds away. That turns the GENERATOR's sleep into the
        // reported tail latency, which is why an early version of this showed
        // p99 getting WORSE as load got LIGHTER: an impossible shape for a
        // server, and the signal that the instrument was measuring itself.
        const std::uint64_t wake = std::min(earliest_due, end_ns);
        const std::uint64_t nowt = now_ns();
        int timeout_ms = 0;
        if (wake > nowt) {
            timeout_ms = static_cast<int>((wake - nowt) / 1000000ULL);
            timeout_ms = std::min(timeout_ms, kMaxPollBlockMs);
        }
        static_cast<void>(::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), timeout_ms));

        std::uint8_t buf[64 * 1024];
        for (std::size_t i = 0; i < clients.size(); ++i) {
            if (!(pfds[i].revents & POLLIN)) continue;
            Client& c = clients[i];
            for (;;) {
                const ssize_t n = ::recv(c.fd, buf, sizeof(buf), 0);
                if (n <= 0) break;
                c.reader.append(buf, static_cast<std::size_t>(n));
                while (auto frame = c.reader.next_frame()) {
                    const std::uint64_t recv_ts = now_ns();
                    const auto type =
                        static_cast<ome::protocol::MessageType>(frame->header.type);
                    if (type == ome::protocol::MessageType::Ack) {
                        const auto ack = ome::protocol::decode<ome::protocol::Ack>(
                            frame->payload.data(), frame->payload.size());
                        if (!ack) continue;
                        const auto it = c.in_flight.find(ack->client_order_id);
                        if (it == c.in_flight.end()) continue;
                        // Warm-up is excluded by SEND time, not receive time:
                        // an order sent during warm-up whose ack lands after it
                        // still carries warm-up conditions.
                        if (it->second >= warmup_end_ns) {
                            out.latencies_ns.push_back(
                                static_cast<std::int64_t>(recv_ts - it->second));
                        }
                        c.in_flight.erase(it);
                        ++out.acked;
                    } else if (type == ome::protocol::MessageType::Reject) {
                        ++out.rejected;
                    }
                    // Fills, book updates and heartbeats are read and discarded
                    // — they must still be drained or the server's write buffer
                    // fills and it disconnects us.
                }
                if (static_cast<std::size_t>(n) < sizeof(buf)) break;
            }
        }
    }
}

std::int64_t percentile(const std::vector<std::int64_t>& sorted, double q) {
    if (sorted.empty()) return 0;
    const auto idx = static_cast<std::size_t>(static_cast<double>(sorted.size() - 1) * q);
    return sorted[idx];
}

void usage() {
    std::fprintf(stderr,
                 "usage: loadgen [--host H] [--port N] [--clients N] [--rate R]\n"
                 "               [--duration S] [--warmup S] [--workers N] [--csv]\n"
                 "\n"
                 "  --rate R    orders/sec PER CLIENT (open loop: sends do not wait\n"
                 "              for acks, so offered load is clients * rate)\n"
                 "  --warmup S  seconds excluded from statistics, by SEND time\n"
                 "  --workers N connection-driving threads (default: min(clients, cores))\n"
                 "  --csv       one CSV row instead of a human-readable block\n"
                 "  --cross     quote crossing prices so orders match (kill test)\n");
}

}  // namespace

int main(int argc, char** argv) {
    Config cfg{};
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };
        if (a == "--help") { usage(); return 0; }
        else if (a == "--host") cfg.host = next();
        else if (a == "--port") cfg.port = static_cast<std::uint16_t>(std::atoi(next().c_str()));
        else if (a == "--clients") cfg.clients = std::atoi(next().c_str());
        else if (a == "--rate") cfg.rate_per_client = std::atof(next().c_str());
        else if (a == "--duration") cfg.duration_s = std::atof(next().c_str());
        else if (a == "--warmup") cfg.warmup_s = std::atof(next().c_str());
        else if (a == "--workers") cfg.workers = std::atoi(next().c_str());
        else if (a == "--csv") cfg.csv = true;
        else if (a == "--cross") cfg.cross = true;
        else { std::fprintf(stderr, "unknown argument: %s\n", a.c_str()); usage(); return 1; }
    }
    if (cfg.clients <= 0 || cfg.rate_per_client <= 0 || cfg.duration_s <= 0) {
        std::fprintf(stderr, "clients, rate and duration must be > 0\n");
        return 1;
    }

    int nworkers = cfg.workers;
    if (nworkers <= 0) {
        // Deliberately FEWER than the core count. The gateway needs two cores
        // of its own on the same machine, and a worker that wakes every
        // millisecond to service one nearly-idle connection spends its time
        // being context-switched rather than measuring. Driving several
        // connections from one poll loop is both cheaper and more accurate.
        nworkers = std::min(cfg.clients, kDefaultWorkers);
    }
    nworkers = std::min(nworkers, cfg.clients);

    // Connect everything before starting the clock, so connection setup is not
    // inside the measured window.
    std::vector<std::vector<Client>> per_worker(static_cast<std::size_t>(nworkers));
    int connected = 0;
    for (int i = 0; i < cfg.clients; ++i) {
        const int fd = dial(cfg.host, cfg.port);
        if (fd < 0) {
            std::fprintf(stderr, "connect failed for client %d: %s\n", i, ::strerror(errno));
            return 1;
        }
        Client c{};
        c.fd = fd;
        per_worker[static_cast<std::size_t>(i % nworkers)].push_back(std::move(c));
        ++connected;
    }

    const std::uint64_t start = now_ns();
    const std::uint64_t warmup_end = start + static_cast<std::uint64_t>(cfg.warmup_s * 1e9);
    const std::uint64_t end = start + static_cast<std::uint64_t>((cfg.warmup_s + cfg.duration_s) * 1e9);

    std::vector<Sample> samples(static_cast<std::size_t>(nworkers));
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(nworkers));
    for (int w = 0; w < nworkers; ++w) {
        threads.emplace_back([&, w] {
            run_worker(cfg, per_worker[static_cast<std::size_t>(w)], start, warmup_end, end,
                       samples[static_cast<std::size_t>(w)]);
        });
    }
    for (auto& t : threads) t.join();

    Sample all;
    for (auto& s : samples) {
        all.latencies_ns.insert(all.latencies_ns.end(), s.latencies_ns.begin(),
                                s.latencies_ns.end());
        all.sent += s.sent;
        all.acked += s.acked;
        all.rejected += s.rejected;
        all.send_failed += s.send_failed;
    }
    std::sort(all.latencies_ns.begin(), all.latencies_ns.end());

    for (auto& cs : per_worker)
        for (auto& c : cs)
            if (c.fd >= 0) ::close(c.fd);

    const double measured_s = cfg.duration_s;
    const double achieved = static_cast<double>(all.latencies_ns.size()) / measured_s;
    const double offered = cfg.clients * cfg.rate_per_client;
    auto us = [](std::int64_t ns) { return static_cast<double>(ns) / 1000.0; };

    if (cfg.csv) {
        std::printf("%d,%.0f,%.0f,%.0f,%zu,%llu,%llu,%.1f,%.1f,%.1f,%.1f,%.1f\n", cfg.clients,
                    cfg.rate_per_client, offered, achieved, all.latencies_ns.size(),
                    static_cast<unsigned long long>(all.rejected),
                    static_cast<unsigned long long>(all.send_failed),
                    us(percentile(all.latencies_ns, 0.50)), us(percentile(all.latencies_ns, 0.90)),
                    us(percentile(all.latencies_ns, 0.99)), us(percentile(all.latencies_ns, 0.999)),
                    us(all.latencies_ns.empty() ? 0 : all.latencies_ns.back()));
        return 0;
    }

    std::printf("clients=%d  rate=%.0f/s/client  offered=%.0f/s  duration=%.1fs (warmup %.1fs)\n",
                cfg.clients, cfg.rate_per_client, offered, cfg.duration_s, cfg.warmup_s);
    std::printf("workers=%d  connections=%d\n", nworkers, connected);
    std::printf("sent=%llu acked=%llu rejected=%llu send_failed=%llu  samples=%zu\n",
                static_cast<unsigned long long>(all.sent),
                static_cast<unsigned long long>(all.acked),
                static_cast<unsigned long long>(all.rejected),
                static_cast<unsigned long long>(all.send_failed), all.latencies_ns.size());
    std::printf("achieved=%.0f acks/s (%.0f%% of offered)\n", achieved,
                offered > 0 ? 100.0 * achieved / offered : 0.0);
    if (all.latencies_ns.empty()) {
        std::printf("no samples — nothing was acked after the warm-up window\n");
        return 1;
    }
    std::printf("order-to-ack latency (us):  p50=%.1f  p90=%.1f  p99=%.1f  p999=%.1f  max=%.1f\n",
                us(percentile(all.latencies_ns, 0.50)), us(percentile(all.latencies_ns, 0.90)),
                us(percentile(all.latencies_ns, 0.99)), us(percentile(all.latencies_ns, 0.999)),
                us(all.latencies_ns.back()));
    return 0;
}
