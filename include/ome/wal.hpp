#pragma once

// ---------------------------------------------------------------------------
// Write-ahead log.
//
// RECORD FORMAT (little-endian, like the wire protocol):
//
//   +--------+--------+--------+------------------+
//   | u32    | u32    | u64    | payload          |
//   | length | crc32  | seq    | (length bytes)   |
//   +--------+--------+--------+------------------+
//
//   length   payload bytes following the 16-byte header
//   crc32    checksum of the PAYLOAD ONLY (see below)
//   seq      strictly increasing, starting at 1
//   payload  a serialized OrderCommand, field by field
//
// The checksum covers the payload rather than the whole record. That is a
// deliberate limitation and worth stating: a corrupt `length` field cannot be
// detected by the checksum, only bounded by a sanity cap. It is caught in
// practice because a wrong length makes the payload fail its own CRC. Covering
// the header too would be stricter; it would also mean the header could not be
// read and validated before deciding how many bytes to read, which is the
// property that lets recovery stop cleanly at a torn tail.
//
// WHY APPEND BEFORE APPLY
//
// The log is written BEFORE the command touches the book, and that ordering is
// the entire meaning of "write-ahead". It chooses which of two failure modes
// you get when the process dies mid-command:
//
//   append first   the record may be on disk for a command that never reached
//                  the book. Recovery replays it — the command is applied
//                  exactly once, just later than the client thought.
//
//   apply first    the book may be mutated for a command whose record never
//                  landed. Recovery has no idea it happened, and the rebuilt
//                  book silently differs from the one that existed. There is no
//                  way to detect this after the fact.
//
// The first is recoverable and the second is not, so the ordering is not a
// preference. It relies on the apply being deterministic given the command,
// which is why the engine has no clock and no randomness in its apply path.
//
// DURABILITY POLICY: GROUP COMMIT
//
// fsync per record is maximal durability and brutal latency — an fsync costs
// hundreds of microseconds to milliseconds, against an order path measured at
// tens of microseconds, so it would dominate everything this gateway does.
//
// Instead: fsync every `sync_interval_ns` or every `sync_every_n` records,
// whichever comes first. The cost is an explicit, bounded loss window — orders
// acknowledged within that window can be lost by a power failure or kernel
// panic. State the window; do not pretend it is zero.
//
// Note the distinction that makes this defensible: a process crash (kill -9,
// segfault) loses NOTHING, because the data has been handed to the kernel with
// write() and the page cache survives the process. Only a machine-level failure
// loses the window. Session 2.4's kill testing exercises exactly the case that
// is fully recoverable.
// ---------------------------------------------------------------------------

#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "ome/commands.hpp"
#include "ome/crc32.hpp"
#include "ome/protocol.hpp"

namespace ome {

inline constexpr std::size_t kWalHeaderSize = 16;

// A corrupt length field must not make recovery allocate arbitrarily. Far above
// any real command, which is a few dozen bytes.
inline constexpr std::uint32_t kWalMaxPayload = 64 * 1024;

struct WalConfig {
    // Group commit knobs. Both are upper bounds on the loss window: whichever
    // is reached first triggers the fsync.
    std::uint64_t sync_interval_ns = 10ULL * 1000 * 1000;  // 10ms
    std::uint64_t sync_every_n = 100;

    // macOS only. fsync() there returns once the data reaches the DRIVE, not
    // once the drive has committed it to stable media; F_FULLFSYNC waits for
    // the latter and is the only call that actually survives a power cut.
    //
    // It is also far slower — measured at roughly 25x the p99 of the same
    // workload without it, because the flush happens on the matching thread and
    // stalls order processing for its duration. Defaults to true: a write-ahead
    // log whose whole purpose is surviving machine failure should not quietly
    // choose the weaker guarantee. Set false to trade power-cut durability for
    // latency, and say so if you do.
    bool full_sync = true;
};

// --- command payload codec -------------------------------------------------
//
// Field by field, for the same reasons as the wire protocol: struct padding is
// uninitialized and compiler-dependent, and a WAL is read back by a build that
// may not be the one that wrote it. OrderCommand happens to be trivially
// copyable, which makes memcpy tempting and wrong — it also carries raw
// pointers (the egress queues) that are meaningless after a restart and must
// never reach the disk.

[[nodiscard]] inline bool is_known_command_type(std::uint8_t t) noexcept {
    switch (static_cast<CommandType>(t)) {
        case CommandType::SessionOpened:
        case CommandType::NewOrder:
        case CommandType::Cancel:
        case CommandType::Modify:
        case CommandType::Subscribe:
        case CommandType::CancelAllForSession:
            return true;
    }
    // -Wswitch makes a newly added enumerator a compile error here rather than
    // a value silently treated as corrupt on disk.
    return false;
}

inline void encode_command(std::vector<std::uint8_t>& out, const OrderCommand& c) {
    protocol::detail::put_u8(out, static_cast<std::uint8_t>(c.type));
    protocol::detail::put_u64(out, c.session);
    protocol::detail::put_u64(out, c.client_order_id);
    protocol::detail::put_i64(out, c.price_ticks);
    protocol::detail::put_u32(out, c.quantity);
    protocol::detail::put_u8(out, static_cast<std::uint8_t>(c.side));
    protocol::detail::put_u8(out, static_cast<std::uint8_t>(c.order_type));
}

[[nodiscard]] inline bool decode_command(const std::uint8_t* data, std::size_t size,
                                         OrderCommand& out) {
    protocol::detail::Reader r(data, size);
    const std::uint8_t type = r.u8();
    out.session = r.u64();
    out.client_order_id = r.u64();
    out.price_ticks = r.i64();
    out.quantity = r.u32();
    const std::uint8_t side = r.u8();
    const std::uint8_t otype = r.u8();
    if (!r.ok() || r.remaining() != 0) {
        return false;
    }
    // Undefined enumerators on disk are a corrupt record, not a new feature.
    //
    // Enumerated explicitly rather than range-checked against "the last one":
    // the first version of this compared against CommandType::Subscribe, which
    // stopped being the highest value the moment Subscribe was inserted before
    // CancelAllForSession. It then rejected every legitimate cancel-all as
    // corrupt. A bound that depends on declaration order is a bound that breaks
    // silently when the order changes.
    if (!is_known_command_type(type) || side > 1 || otype > 1) {
        return false;
    }
    out.type = static_cast<CommandType>(type);
    out.side = static_cast<protocol::Side>(side);
    out.order_type = static_cast<protocol::OrderType>(otype);
    // Pointers are per-process and were never written; recovery must not
    // resurrect a stale one.
    out.egress = nullptr;
    out.md_queue = nullptr;
    return true;
}

// Only state-mutating commands belong in the log. A rejected order changes
// nothing, and SessionOpened/Subscribe describe connections that will not exist
// after a restart. Logging them would make recovery replay events for sessions
// that are gone.
[[nodiscard]] inline bool is_loggable(CommandType t) noexcept {
    return t == CommandType::NewOrder || t == CommandType::Cancel ||
           t == CommandType::Modify || t == CommandType::CancelAllForSession;
}

// --- writer ----------------------------------------------------------------

class Wal {
public:
    Wal() = default;
    ~Wal() { close(); }

    Wal(const Wal&) = delete;
    Wal& operator=(const Wal&) = delete;

    // Opens for append, creating if absent. `now_ns` seeds the group-commit
    // timer; `start_seq` continues an existing log's numbering.
    [[nodiscard]] bool open(const std::string& path, std::uint64_t now_ns,
                            std::uint64_t start_seq = 0, WalConfig cfg = {}) {
        close();
        cfg_ = cfg;
        seq_ = start_seq;
        last_sync_ns_ = now_ns;
        fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd_ < 0) {
            error_ = "cannot open " + path + ": " + std::strerror(errno);
            return false;
        }
        path_ = path;
        return true;
    }

    void close() {
        if (fd_ >= 0) {
            sync();
            ::close(fd_);
            fd_ = -1;
        }
    }

    [[nodiscard]] bool is_open() const noexcept { return fd_ >= 0; }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    [[nodiscard]] std::uint64_t last_seq() const noexcept { return seq_; }
    [[nodiscard]] std::uint64_t syncs() const noexcept { return syncs_; }

    // Appends one command. Returns false only on a write error, which is
    // unrecoverable: continuing to apply commands whose records did not land
    // would break the append-before-apply guarantee silently.
    [[nodiscard]] bool append(const OrderCommand& c, std::uint64_t now_ns) {
        if (fd_ < 0) {
            return false;
        }
        payload_.clear();
        encode_command(payload_, c);

        record_.clear();
        record_.reserve(kWalHeaderSize + payload_.size());
        protocol::detail::put_u32(record_, static_cast<std::uint32_t>(payload_.size()));
        protocol::detail::put_u32(record_, crc32(payload_.data(), payload_.size()));
        protocol::detail::put_u64(record_, ++seq_);
        record_.insert(record_.end(), payload_.begin(), payload_.end());

        if (!write_all(record_.data(), record_.size())) {
            return false;
        }
        ++since_sync_;
        maybe_sync(now_ns);
        return true;
    }

    // Time-based half of the group commit, for callers that go quiet: without
    // it, a burst that ends just short of sync_every_n would sit unsynced until
    // the next command arrived, which could be a long time.
    void poll_sync(std::uint64_t now_ns) { maybe_sync(now_ns); }

    // Forces the group commit early. Called on clean shutdown so a graceful
    // stop has no loss window at all.
    void sync() {
        if (fd_ < 0 || since_sync_ == 0) {
            return;
        }
        // fdatasync where available: the file's size changes on every append, so
        // the metadata must be durable too, but access times need not be.
#if defined(__APPLE__)
        if (cfg_.full_sync) {
            static_cast<void>(::fcntl(fd_, F_FULLFSYNC));
        } else {
            static_cast<void>(::fsync(fd_));
        }
#else
        static_cast<void>(::fdatasync(fd_));
#endif
        since_sync_ = 0;
        ++syncs_;
    }

private:
    void maybe_sync(std::uint64_t now_ns) {
        if (since_sync_ >= cfg_.sync_every_n ||
            (now_ns > last_sync_ns_ && now_ns - last_sync_ns_ >= cfg_.sync_interval_ns)) {
            sync();
            last_sync_ns_ = now_ns;
        }
    }

    [[nodiscard]] bool write_all(const std::uint8_t* data, std::size_t size) {
        std::size_t off = 0;
        while (off < size) {
            const ssize_t n = ::write(fd_, data + off, size - off);
            if (n > 0) {
                off += static_cast<std::size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            error_ = std::string("wal write failed: ") + std::strerror(errno);
            return false;
        }
        return true;
    }

    int fd_{-1};
    std::string path_;
    std::string error_;
    WalConfig cfg_{};
    std::uint64_t seq_{0};
    std::uint64_t since_sync_{0};
    std::uint64_t last_sync_ns_{0};
    std::uint64_t syncs_{0};
    std::vector<std::uint8_t> payload_;
    std::vector<std::uint8_t> record_;
};

// --- reader ----------------------------------------------------------------

struct WalReadResult {
    std::vector<OrderCommand> commands;
    std::uint64_t last_seq{0};
    // Bytes at the end that did not form an intact record. Non-zero after a
    // crash is NORMAL, not an error: the process died mid-write.
    std::size_t truncated_bytes{0};
    bool sequence_gap{false};
    std::uint64_t gap_after{0};
    std::string error;
};

// Reads a log, stopping at the first record that is not intact.
//
// A torn tail is expected and is not a failure — a crash leaves one. A gap in
// the sequence numbers is different: it means a record that WAS written is now
// missing, which implies a lost or truncated file rather than an interrupted
// write. That is reported, and startup should refuse rather than silently
// rebuild a book with a hole in its history.
[[nodiscard]] inline WalReadResult read_wal(const std::string& path) {
    WalReadResult r{};
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        r.error = "cannot open " + path + ": " + std::strerror(errno);
        return r;
    }

    std::vector<std::uint8_t> buf;
    std::uint8_t chunk[64 * 1024];
    for (;;) {
        const ssize_t n = ::read(fd, chunk, sizeof(chunk));
        if (n > 0) {
            buf.insert(buf.end(), chunk, chunk + n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    ::close(fd);

    std::size_t pos = 0;
    while (pos + kWalHeaderSize <= buf.size()) {
        protocol::detail::Reader hr(buf.data() + pos, kWalHeaderSize);
        const std::uint32_t len = hr.u32();
        const std::uint32_t want_crc = hr.u32();
        const std::uint64_t seq = hr.u64();

        if (len > kWalMaxPayload) {
            break;  // a corrupt length; treat everything from here as torn
        }
        if (pos + kWalHeaderSize + len > buf.size()) {
            break;  // payload incomplete: the write was interrupted
        }
        const std::uint8_t* payload = buf.data() + pos + kWalHeaderSize;
        if (crc32(payload, len) != want_crc) {
            break;  // record is not intact
        }

        OrderCommand c{};
        if (!decode_command(payload, len, c)) {
            break;
        }
        if (r.last_seq != 0 && seq != r.last_seq + 1) {
            r.sequence_gap = true;
            r.gap_after = r.last_seq;
            break;
        }
        r.commands.push_back(c);
        r.last_seq = seq;
        pos += kWalHeaderSize + len;
    }

    r.truncated_bytes = buf.size() - pos;
    return r;
}

}  // namespace ome
