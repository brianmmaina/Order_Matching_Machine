#include "ome/tcp_server.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>

#include "ome/reject_reason.hpp"

namespace ome {

namespace {

// pfds[0] is the listener, pfds[1] the matching-thread wake-up; connections
// start at this offset.
constexpr std::size_t kFixedPfds = 2;


bool set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

std::string errno_msg(const char* what) {
    return std::string(what) + ": " + ::strerror(errno);
}

// steady_clock, not system_clock: a session must not be declared dead because
// someone corrected the wall clock or an NTP step landed mid-trading.
Nanos monotonic_now() {
    return static_cast<Nanos>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch())
                                  .count());
}

}  // namespace

TcpServer::~TcpServer() {
    for (auto& c : conns_) {
        if (c && c->fd >= 0) {
            ::close(c->fd);
        }
    }
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
    }
}

bool TcpServer::start() {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        last_error_ = errno_msg("socket");
        return false;
    }

    // Without SO_REUSEADDR, a listener that closes while connections are in
    // TIME_WAIT cannot rebind its port for ~60s — which makes restarting the
    // gateway after a crash fail exactly when you most want it to start.
    const int one = 1;
    if (::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) {
        last_error_ = errno_msg("setsockopt SO_REUSEADDR");
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // loopback only: no auth in v1
    addr.sin_port = htons(cfg_.port);

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        last_error_ = errno_msg("bind");
        return false;
    }

    // Recover the real port: cfg_.port may have been 0, meaning "any free one".
    sockaddr_in actual{};
    socklen_t alen = sizeof(actual);
    if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&actual), &alen) == 0) {
        bound_port_ = ntohs(actual.sin_port);
    }

    if (::listen(listen_fd_, cfg_.backlog) < 0) {
        last_error_ = errno_msg("listen");
        return false;
    }
    if (!set_nonblocking(listen_fd_)) {
        last_error_ = errno_msg("fcntl O_NONBLOCK on listener");
        return false;
    }

    running_.store(true, std::memory_order_relaxed);
    return true;
}

void TcpServer::run() {
    std::vector<pollfd> pfds;

    while (running_.load(std::memory_order_relaxed)) {
        pfds.clear();
        pfds.reserve(conns_.size() + 1);

        // POLLIN on the listener means "a connection is queued for accept()".
        const Nanos loop_now = monotonic_now();
        const bool accepting = loop_now >= accept_paused_until_ns_;
        pollfd lp{};
        // fd -1 makes poll() ignore the entry while keeping indices stable, so
        // the conns_[i] <-> pfds[i+1] correspondence below does not shift.
        lp.fd = accepting ? listen_fd_ : -1;
        lp.events = POLLIN;
        pfds.push_back(lp);

        // The matching thread's wake-up channel. Without it, a freshly produced
        // Ack waits for the next poll timeout or for another client to happen
        // to send something — which the load generator measured as milliseconds
        // of latency that had nothing to do with matching.
        pollfd np{};
        np.fd = (egress_ready_ != nullptr) ? egress_ready_->poll_fd() : -1;
        np.events = POLLIN;
        pfds.push_back(np);

        for (const auto& c : conns_) {
            pollfd p{};
            // -1 makes poll() ignore the entry while preserving the
            // conns_[i] <-> pfds[i+1] correspondence used below.
            p.fd = c->retiring ? -1 : c->fd;
            // POLLIN: bytes are readable, or the peer closed (a read returning
            // 0 is how we learn about the close).
            p.events = POLLIN;
            // POLLOUT is requested ONLY when there is something to send.
            // Registering it unconditionally would make poll() return
            // immediately every time — a writable idle socket is always
            // writable — turning the event loop into a busy spin at 100% CPU.
            if (!c->writer.empty()) {
                p.events = static_cast<short>(p.events | POLLOUT);
            }
            pfds.push_back(p);
        }

        // Never block while there is already output waiting. The wake-up pipe
        // is an optimisation; THIS is what guarantees an event is delivered
        // promptly even if a notification is lost. Without it a missed wake-up
        // becomes a latency spike of a whole poll timeout, which is exactly
        // what the load generator caught.
        bool work_pending = false;
        for (const auto& c : conns_) {
            if (!c->writer.empty() || (c->egress && !c->egress->empty()) ||
                (c->subscribed && c->md && !c->md->empty())) {
                work_pending = true;
                break;
            }
        }
        const int timeout = work_pending ? 0 : cfg_.poll_timeout_ms;
        const int n = ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), timeout);
        if (n < 0) {
            // EINTR just means a signal arrived mid-wait; it is not an error.
            if (errno == EINTR) {
                continue;
            }
            last_error_ = errno_msg("poll");
            return;
        }

        if (pfds[0].revents & POLLIN) {
            accept_new();
        }
        if (egress_ready_ != nullptr && (pfds[1].revents & POLLIN)) {
            egress_ready_->drain();
        }

        // Timer work runs every iteration regardless of socket activity, which
        // is why poll() has a bounded timeout: a completely silent set of
        // connections still needs heartbeats sent and dead ones swept.
        service_timers(monotonic_now());

        // Iterate backwards so erasing a closed connection does not disturb
        // the indices of the ones not yet visited.
        for (std::size_t i = conns_.size(); i-- > 0;) {
            // conns_ may have grown via accept_new() above; those new entries
            // have no matching pollfd this iteration and are simply skipped.
            if (i + kFixedPfds >= pfds.size()) {
                continue;
            }
            const short re = pfds[i + kFixedPfds].revents;
            Connection& c = *conns_[i];

            // POLLERR/POLLNVAL are always reported regardless of what we asked
            // for. POLLHUP means the peer hung up; there may still be buffered
            // data to read, so it is handled after the read below.
            if (re & (POLLERR | POLLNVAL)) {
                close_connection(i);
                continue;
            }
            if (re & POLLOUT) {
                handle_writable(c);
            }
            if (re & POLLIN) {
                handle_readable(c);
            }
            if (re & POLLHUP) {
                // Peer hung up. Anything still buffered has nowhere to go.
                c.close_now();
            }
            if (c.ready_to_close(monotonic_now())) {
                close_connection(i);
            }
        }

        // Drain egress AFTER reading and dispatching, so an ack produced by
        // this iteration's orders goes out on this iteration rather than the
        // next. Retiring connections are drained too — they are held open
        // precisely to observe the SessionRetired tombstone.
        for (std::size_t i = conns_.size(); i-- > 0;) {
            Connection& c = *conns_[i];
            drain_egress(c);
            drain_market_data(c);
            if (!c.writer.empty() && c.fd >= 0) {
                // Try to send immediately instead of waiting for POLLOUT next
                // time round: on loopback the socket is almost always writable
                // and this removes a whole poll cycle from the ack path.
                handle_writable(c);
            }
            if (c.ready_to_close(monotonic_now())) {
                close_connection(i);
            }
        }
    }
}

void TcpServer::accept_new() {
    // Loop: poll() is level-triggered, but several connections may have queued
    // since the last wake and draining them now saves a syscall round trip.
    for (;;) {
        const int fd = ::accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) {
            // EAGAIN/EWOULDBLOCK simply means the backlog is now empty.
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            if (errno == EINTR || errno == ECONNABORTED) {
                continue;  // transient: a peer aborted before we accepted
            }
            if (errno == EMFILE || errno == ENFILE) {
                // Out of file descriptors. The connection stays queued, and
                // poll() is level-triggered, so returning here would report the
                // listener readable again immediately and spin the loop at 100%
                // CPU until an fd frees — exactly when the process is already
                // under pressure. Stop polling the listener for a moment
                // instead; existing connections keep being served and will
                // release descriptors.
                accept_paused_until_ns_ = monotonic_now() + cfg_.accept_backoff_ns;
                return;
            }
            return;
        }

        if (!set_nonblocking(fd)) {
            ::close(fd);
            continue;
        }
        // Disable Nagle: it delays a small write hoping to coalesce it with the
        // next one, which is exactly wrong for an order gateway where the
        // whole point is getting one small message out immediately.
        const int one = 1;
        static_cast<void>(::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)));
#ifdef SO_NOSIGPIPE
        // macOS has no MSG_NOSIGNAL; it suppresses SIGPIPE per-socket instead.
        static_cast<void>(::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)));
#endif

        conns_.push_back(std::make_unique<Connection>(fd, next_session_id_++,
                                                      cfg_.max_write_buffer, monotonic_now(),
                                                      cfg_.session, cfg_.risk));
        // Hand the matching thread this session's egress queue before any
        // command for it can arrive. Registration travels the command queue
        // like everything else — there is no side channel between the threads.
        Connection& c = *conns_.back();
        if (inbound_ != nullptr) {
            static_cast<void>(submit(c, OrderCommand::session_opened(c.id(), c.egress.get(), c.md.get())));
        }
    }
}

void TcpServer::handle_readable(Connection& c) {
    std::uint8_t buf[16 * 1024];
    for (;;) {
        const ssize_t n = ::recv(c.fd, buf, sizeof(buf), 0);
        if (n > 0) {
            c.reader.append(buf, static_cast<std::size_t>(n));

            // Drain EVERY complete frame. One read can carry many; stopping
            // after the first would leave the rest buffered until the peer
            // happens to send more bytes — a subtle stall under load.
            // ANY inbound traffic proves liveness, not just Heartbeat frames:
            // a client streaming orders is obviously alive.
            c.session.on_inbound(monotonic_now());

            while (auto frame = c.reader.next_frame()) {
                dispatch(c, *frame);
                if (c.closing()) {
                    return;
                }
            }
            if (c.reader.failed()) {
                // Framing is unrecoverable: without delimiters there is no way
                // to find where the next message starts. Nothing useful can be
                // said to a peer we can no longer parse.
                c.close_now();
                return;
            }
            if (c.reader.buffered() > cfg_.max_read_buffer) {
                // A peer holding open an incomplete frame forever.
                c.close_now();
                return;
            }
            if (static_cast<std::size_t>(n) < sizeof(buf)) {
                return;  // drained the socket
            }
            continue;
        }

        if (n == 0) {
            c.close_now();  // orderly peer close; no point buffering a reply
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;  // nothing more right now
        }
        if (errno == EINTR) {
            continue;
        }
        c.close_now();  // ECONNRESET and friends
        return;
    }
}

void TcpServer::handle_writable(Connection& c) {
    while (!c.writer.empty()) {
        // MSG_NOSIGNAL where available: without it, writing to a socket whose
        // peer has closed raises SIGPIPE and kills the process. macOS spells
        // this SO_NOSIGPIPE (set per-socket); linux uses this flag.
#ifdef MSG_NOSIGNAL
        const int flags = MSG_NOSIGNAL;
#else
        const int flags = 0;
#endif
        const ssize_t n = ::send(c.fd, c.writer.data(), c.writer.pending(), flags);
        if (n > 0) {
            // THE PARTIAL WRITE: send() may accept fewer bytes than offered
            // when the kernel send buffer is nearly full. Treating its return
            // as all-or-nothing silently truncates the message.
            c.writer.consume(static_cast<std::size_t>(n));
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;  // kernel buffer full; POLLOUT will tell us when it drains
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        c.close_now();  // the socket is broken; flushing is not an option
        return;
    }
}

bool TcpServer::submit(Connection& c, const OrderCommand& cmd) {
    if (inbound_ == nullptr) {
        return false;
    }
    if (!inbound_->push(cmd)) {
        // The matching thread is behind by 8192 commands. Backpressure has to
        // land somewhere, and rejecting is the only honest option: silently
        // dropping would leave the client waiting for a response forever, and
        // blocking the network thread would stall every OTHER session too.
        queue(c, protocol::encode_frame(
                     protocol::MessageType::Reject,
                     protocol::Reject{cmd.client_order_id, RejectReason::RATE_LIMITED}));
        return false;
    }
    // Signal AFTER the push is published. See waiter.hpp: reversing these is
    // the missed-wake-up race.
    if (waiter_ != nullptr) {
        waiter_->signal();
    }
    return true;
}

void TcpServer::deliver(Connection& c, const OrderEvent& e) {
    using namespace protocol;
    switch (e.type) {
        case EventType::Ack:
            if (e.closes_order) {
                static_cast<void>(c.session.forget_order(e.client_order_id));
            }
            queue(c, encode_frame(MessageType::Ack, Ack{e.client_order_id, e.exchange_order_id}));
            return;
        case EventType::Reject:
            // The order never rested, so the session must stop holding its
            // client_order_id — otherwise the client could never retry it.
            static_cast<void>(c.session.forget_order(e.client_order_id));
            queue(c, encode_frame(MessageType::Reject, Reject{e.client_order_id, e.reason}));
            return;
        case EventType::Fill:
            if (e.closes_order) {
                static_cast<void>(c.session.forget_order(e.client_order_id));
            }
            queue(c, encode_frame(MessageType::Fill,
                                  Fill{e.exchange_order_id, e.price_ticks, e.quantity,
                                       e.remaining_quantity}));
            return;
        case EventType::SessionRetired:
            // The tombstone. The matching thread will never push here again, so
            // the queue and the Connection can now be destroyed.
            c.retiring = false;
            c.close_now();
            return;
    }
}

void TcpServer::drain_market_data(Connection& c) {
    if (!c.md || !c.subscribed) {
        return;
    }
    // CONFLATION, and it happens here on the consumer side rather than by
    // mutating a published ring slot (which would break the SPSC contract).
    //
    // Drain everything pending and keep only the last. A subscriber that keeps
    // up receives every update; one that falls behind skips straight to
    // current. That is correct for a full snapshot and would be catastrophic
    // for order flow — which is exactly why the two use different queues.
    BookSnapshot latest{};
    bool have = false;
    std::size_t skipped = 0;
    while (auto s = c.md->pop()) {
        if (have) {
            ++skipped;
        }
        latest = *s;
        have = true;
    }
    if (!have) {
        return;
    }
    conflated_ += skipped;

    protocol::BookUpdate up{};
    up.seq = latest.seq;
    for (std::uint8_t i = 0; i < latest.n_bids; ++i) {
        up.bids.push_back({latest.bid_ticks[i], latest.bid_qty[i]});
    }
    for (std::uint8_t i = 0; i < latest.n_asks; ++i) {
        up.asks.push_back({latest.ask_ticks[i], latest.ask_qty[i]});
    }
    queue(c, protocol::encode_frame(protocol::MessageType::BookUpdate, up));
}

void TcpServer::drain_egress(Connection& c) {
    if (!c.egress) {
        return;
    }
    if (c.egress->overflowed()) {
        // The matching thread could not report a full queue THROUGH the queue,
        // so it latched a flag. Order flow is never dropped; the session goes.
        c.close_now();
        return;
    }
    while (auto e = c.egress->pop()) {
        deliver(c, *e);
        if (c.close_mode == CloseMode::Immediate && e->type != EventType::SessionRetired) {
            return;
        }
    }
}

void TcpServer::queue(Connection& c, const std::vector<std::uint8_t>& bytes) {
    if (!c.writer.append(bytes)) {
        // Slow-consumer policy for order flow: disconnect rather than buffer
        // without limit. See include/ome/write_buffer.hpp for why dropping is
        // not an option for acks and fills.
        //
        // IMMEDIATE, not after-flush: the buffer is full precisely because this
        // peer is not reading, so waiting for it to drain waits forever. This
        // was the bug — the abusive connection was the one that never closed.
        c.close_now();
    }
}

void TcpServer::dispatch(Connection& c, const Frame& f) {
    using namespace protocol;

    if (f.header.version != kVersion) {
        queue(c, encode_frame(MessageType::Reject, Reject{0, RejectReason::MALFORMED}));
        return;
    }

    const auto mtype = static_cast<MessageType>(f.header.type);

    // Rate limit before decoding anything but the header. Heartbeats are exempt
    // — throttling a client's liveness signal would time it out for the crime
    // of being chatty, and they cost nothing to process.
    if (mtype != MessageType::Heartbeat && !c.bucket.allow(monotonic_now())) {
        // client_order_id is not known yet: decoding the payload to recover it
        // would do the work the limiter exists to avoid.
        queue(c, encode_frame(MessageType::Reject, Reject{0, RejectReason::RATE_LIMITED}));
        return;
    }

    switch (mtype) {
        case MessageType::NewOrder: {
            const auto m = decode<NewOrder>(f.payload.data(), f.payload.size());
            if (!m.has_value()) {
                queue(c, encode_frame(MessageType::Reject, Reject{0, RejectReason::MALFORMED}));
                return;
            }
            if (m->quantity == 0) {
                queue(c, encode_frame(MessageType::Reject,
                                      Reject{m->client_order_id, RejectReason::INVALID_QTY}));
                return;
            }
            // Duplicate detection is per-session and lives on the network
            // thread: it needs no book state, so keeping it here stops garbage
            // reaching the queue at all. Contrast with the price-band check in
            // 1.5, which needs the book and therefore cannot live here.
            if (!c.session.register_order(m->client_order_id)) {
                queue(c, encode_frame(MessageType::Reject,
                                      Reject{m->client_order_id, RejectReason::DUPLICATE_ORDER_ID}));
                return;
            }
            // Onto the queue. The Ack now comes back from the matching thread
            // through this session's egress queue — the network thread never
            // touches the book.
            if (inbound_ == nullptr) {
                // No engine attached (framing tests): ack locally.
                queue(c, encode_frame(MessageType::Ack,
                                      Ack{m->client_order_id, c.id() * 1000000 + ++acks_}));
                return;
            }
            if (!submit(c, OrderCommand::new_order(c.id(), *m))) {
                static_cast<void>(c.session.forget_order(m->client_order_id));
            }
            return;
        }
        case MessageType::Cancel: {
            const auto m = decode<Cancel>(f.payload.data(), f.payload.size());
            if (!m.has_value()) {
                queue(c, encode_frame(MessageType::Reject, Reject{0, RejectReason::MALFORMED}));
                return;
            }
            // Not answered here any more. Whether the order still rests is book
            // state, and only the matching thread may read the book — an order
            // can fill between the client sending this and the cancel being
            // applied, and only the matching thread sees that ordering.
            if (inbound_ == nullptr) {
                queue(c, encode_frame(MessageType::Reject,
                                      Reject{m->client_order_id, RejectReason::NOT_IMPLEMENTED}));
                return;
            }
            static_cast<void>(submit(c, OrderCommand::cancel(c.id(), m->client_order_id)));
            return;
        }
        case MessageType::Modify: {
            // Recognized and well-formed, but the engine is not wired until
            // 1.4. NOT_IMPLEMENTED rather than MALFORMED: the message was not
            // malformed, and telling a client its correct message was garbage
            // sends it debugging the wrong thing.
            const auto m = decode<Modify>(f.payload.data(), f.payload.size());
            if (!m.has_value()) {
                queue(c, encode_frame(MessageType::Reject, Reject{0, RejectReason::MALFORMED}));
                return;
            }
            if (inbound_ == nullptr) {
                // Echo the client_order_id: a client with several orders in
                // flight cannot act on a rejection that omits which one.
                queue(c, encode_frame(MessageType::Reject,
                                      Reject{m->client_order_id, RejectReason::NOT_IMPLEMENTED}));
                return;
            }
            static_cast<void>(submit(c, OrderCommand::modify(c.id(), *m)));
            return;
        }
        case MessageType::Subscribe: {
            const auto m = decode<Subscribe>(f.payload.data(), f.payload.size());
            if (!m.has_value()) {
                queue(c, encode_frame(MessageType::Reject, Reject{0, RejectReason::MALFORMED}));
                return;
            }
            if (inbound_ == nullptr) {
                queue(c, encode_frame(MessageType::Reject, Reject{0, RejectReason::NOT_IMPLEMENTED}));
                return;
            }
            // The flag here only gates DRAINING. Membership of the broadcast set
            // lives on the matching thread and is set by the command below —
            // this side must never be the authority on who receives what.
            c.subscribed = true;
            static_cast<void>(submit(c, OrderCommand::subscribe(c.id(), m->depth)));
            return;
        }
        case MessageType::Heartbeat:
            // Liveness was already recorded by on_inbound(); nothing else to do.
            //
            // Clients are REQUIRED to speak within the session timeout (see
            // docs/PROTOCOL.md, "Liveness is a client obligation"). Any message
            // counts, so an active client need never send one of these — but a
            // passive one must, or its resting orders are cancelled.
            return;
        default:
            queue(c, encode_frame(MessageType::Reject,
                                  Reject{0, RejectReason::UNKNOWN_MESSAGE_TYPE}));
            return;
    }
}

void TcpServer::kill_session(Connection& c) {
    // mark_dead() returns true only on the transition, so this fires exactly
    // once no matter how many paths notice the death (timeout, peer close,
    // framing error, write-buffer overflow, egress overflow).
    if (!c.session.mark_dead()) {
        return;
    }
    if (cancel_all_) {
        cancel_all_(c.session.id(), c.session.live_order_count());
    }
    if (inbound_ == nullptr) {
        if (!cancel_all_) {
            std::fprintf(stderr, "[session %llu] disconnect: cancel-all for %zu resting orders\n",
                         static_cast<unsigned long long>(c.session.id()),
                         c.session.live_order_count());
        }
        return;
    }
    // The real thing: cancel-on-disconnect as a command on the same queue as
    // everything else. The network thread cannot cancel anything itself — only
    // the matching thread may touch the book.
    if (submit(c, OrderCommand::cancel_all(c.session.id()))) {
        // Hold the Connection open, socket closed, until SessionRetired comes
        // back. That tombstone is what makes freeing the egress queue safe.
        c.retiring = true;
    }
}

void TcpServer::service_timers(Nanos now) {
    for (auto& cp : conns_) {
        Connection& c = *cp;
        if (c.session.timed_out(now)) {
            // Silent for longer than the timeout. Close rather than probe
            // further: three heartbeat intervals have already gone unanswered,
            // so there is no reason to believe a fourth would be read.
            c.close_now();
            continue;
        }
        if (c.session.heartbeat_due(now)) {
            queue(c, encode_frame(protocol::MessageType::Heartbeat, protocol::Heartbeat{now}));
            c.session.on_heartbeat_sent(now);
        }
    }
}

void TcpServer::close_connection(std::size_t index) {
    if (index >= conns_.size()) {
        return;
    }
    Connection& c = *conns_[index];
    // Every close path funnels through here, so cancel-on-disconnect cannot be
    // missed by a route that forgot to call it.
    kill_session(c);

    // The socket goes immediately — no reason to hold a descriptor for a peer
    // that is gone.
    if (c.fd >= 0) {
        ::close(c.fd);
        c.fd = -1;
    }

    // But the Connection itself may not be destroyed yet: it owns the egress
    // queue, and the matching thread may still be pushing into it. Destruction
    // waits for the SessionRetired tombstone, which deliver() turns into an
    // Immediate close with retiring cleared.
    if (c.retiring) {
        c.close_mode = CloseMode::None;  // re-armed when the tombstone arrives
        return;
    }
    conns_.erase(conns_.begin() + static_cast<std::ptrdiff_t>(index));
}

}  // namespace ome
