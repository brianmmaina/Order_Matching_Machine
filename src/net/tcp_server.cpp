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

        for (const auto& c : conns_) {
            pollfd p{};
            p.fd = c->fd;
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

        const int n = ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), cfg_.poll_timeout_ms);
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

        // Timer work runs every iteration regardless of socket activity, which
        // is why poll() has a bounded timeout: a completely silent set of
        // connections still needs heartbeats sent and dead ones swept.
        service_timers(monotonic_now());

        // Iterate backwards so erasing a closed connection does not disturb
        // the indices of the ones not yet visited.
        for (std::size_t i = conns_.size(); i-- > 0;) {
            // conns_ may have grown via accept_new() above; those new entries
            // have no matching pollfd this iteration and are simply skipped.
            if (i + 1 >= pfds.size()) {
                continue;
            }
            const short re = pfds[i + 1].revents;
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
                                                      cfg_.session));
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

    switch (static_cast<MessageType>(f.header.type)) {
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
            // SESSION 1.3 STUB: still a hardcoded Ack. There is no book yet, so
            // the exchange_order_id is fabricated. Session 1.4 replaces this
            // with a command onto the SPSC queue and an Ack produced by the
            // matching thread.
            queue(c, encode_frame(MessageType::Ack,
                                  Ack{m->client_order_id, c.id() * 1000000 + ++acks_}));
            return;
        }
        case MessageType::Cancel: {
            const auto m = decode<Cancel>(f.payload.data(), f.payload.size());
            if (!m.has_value()) {
                queue(c, encode_frame(MessageType::Reject, Reject{0, RejectReason::MALFORMED}));
                return;
            }
            // Cancelling something this session never had is knowable without
            // the book, so it is answered here.
            if (!c.session.forget_order(m->client_order_id)) {
                queue(c, encode_frame(MessageType::Reject,
                                      Reject{m->client_order_id, RejectReason::UNKNOWN_ORDER}));
                return;
            }
            queue(c, encode_frame(MessageType::Ack, Ack{m->client_order_id, 0}));
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
            // Echo the client_order_id: a client with several orders in flight
            // cannot act on a rejection that does not say which one it concerns.
            queue(c, encode_frame(MessageType::Reject,
                                  Reject{m->client_order_id, RejectReason::NOT_IMPLEMENTED}));
            return;
        }
        case MessageType::Subscribe: {
            const auto m = decode<Subscribe>(f.payload.data(), f.payload.size());
            if (!m.has_value()) {
                queue(c, encode_frame(MessageType::Reject, Reject{0, RejectReason::MALFORMED}));
                return;
            }
            // Subscribe carries no client_order_id, so 0 is honest here.
            queue(c, encode_frame(MessageType::Reject, Reject{0, RejectReason::NOT_IMPLEMENTED}));
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
    // mark_dead() returns true only on the transition, so the cancel-all fires
    // exactly once no matter how many paths notice the death (timeout, peer
    // close, framing error, write-buffer overflow).
    if (!c.session.mark_dead()) {
        return;
    }
    if (cancel_all_) {
        cancel_all_(c.session.id(), c.session.live_order_count());
    } else {
        std::fprintf(stderr, "[session %llu] disconnect: cancel-all for %zu resting orders\n",
                     static_cast<unsigned long long>(c.session.id()),
                     c.session.live_order_count());
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
    // Every close path funnels through here, so cancel-on-disconnect cannot be
    // missed by a route that forgot to call it.
    kill_session(*conns_[index]);
    if (conns_[index]->fd >= 0) {
        ::close(conns_[index]->fd);
        conns_[index]->fd = -1;
    }
    conns_.erase(conns_.begin() + static_cast<std::ptrdiff_t>(index));
}

}  // namespace ome
