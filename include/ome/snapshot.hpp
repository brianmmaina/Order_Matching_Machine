#pragma once

// ---------------------------------------------------------------------------
// Book snapshots, so recovery time stops being proportional to the whole log.
//
// Without one, restarting means replaying every command since the exchange
// first started. With one: load the newest snapshot, replay only the log tail
// after it.
//
// FILE FORMAT (little-endian, same discipline as the WAL):
//
//   header
//     u32 magic       'O','M','E','S'
//     u32 version
//     u64 last_seq    the WAL sequence this snapshot reflects
//     u64 order_count
//     i64 last_trade_ticks
//     u64 next_exchange_id
//     u32 crc32       of everything AFTER the header
//   then order_count records, in book order:
//     u64 exchange_id, u64 session, u64 client_order_id,
//     i64 price_ticks, u32 quantity, u8 side, u64 timestamp
//
// Orders are written in book order — bids best-first then asks best-first, and
// within a level in time priority — so replaying them through addOrder rebuilds
// the identical book. Nothing about the book's internal layout is serialized,
// only the orders and the order they arrived in, which is the only thing that
// actually determines the result.
//
// WHY temp + fsync + rename
//
// Writing in place means a crash mid-write leaves a half-written snapshot that
// looks like a real one. Instead: write to a temp file, fsync it, then rename
// over the target. rename(2) is atomic within a filesystem — any reader sees
// either the old complete file or the new complete file, never a mixture. The
// fsync must come BEFORE the rename, or the rename can land while the contents
// are still only in the page cache, and a power cut then leaves a file that
// exists, is named correctly, and contains garbage.
//
// The CRC is belt and braces on top: it catches a snapshot that was corrupted
// after being written, which atomicity says nothing about.
//
// PAUSE-THE-WORLD, deliberately, for now
//
// The matching thread writes the snapshot itself and stops matching while it
// does. The plan's alternative — copy the book quickly and serialize the copy
// on a background thread — is strictly better for tail latency and strictly
// more code. Measure the simple version first; only get clever if the stall is
// actually a problem at realistic book sizes. The measured pause is recorded in
// docs/BENCHMARK.md.
// ---------------------------------------------------------------------------

#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "ome/crc32.hpp"
#include "ome/protocol.hpp"
#include "order.h"

namespace ome {

inline constexpr std::uint32_t kSnapshotMagic = 0x53454D4F;  // 'OMES' little-endian
inline constexpr std::uint32_t kSnapshotVersion = 1;
// magic 4 + version 4 + last_seq 8 + count 8 + last_trade 8 + next_id 8 + crc 4
inline constexpr std::size_t kSnapshotHeaderSize = 44;

// One resting order, with the ownership the matching thread needs to route a
// later fill or honor a later cancel.
struct SnapshotOrder {
    std::uint64_t exchange_id{};
    std::uint64_t session{};
    std::uint64_t client_order_id{};
    std::int64_t price_ticks{};
    std::uint32_t quantity{};
    std::uint8_t side{};  // 0 = BID, 1 = ASK
    std::uint64_t timestamp{};
};

struct SnapshotData {
    std::uint64_t last_seq{};
    std::int64_t last_trade_ticks{};
    std::uint64_t next_exchange_id{1};
    std::vector<SnapshotOrder> orders;
};

[[nodiscard]] inline bool write_snapshot(const std::string& path, const SnapshotData& d,
                                         std::string& error) {
    std::vector<std::uint8_t> body;
    body.reserve(d.orders.size() * 45);
    for (const auto& o : d.orders) {
        protocol::detail::put_u64(body, o.exchange_id);
        protocol::detail::put_u64(body, o.session);
        protocol::detail::put_u64(body, o.client_order_id);
        protocol::detail::put_i64(body, o.price_ticks);
        protocol::detail::put_u32(body, o.quantity);
        protocol::detail::put_u8(body, o.side);
        protocol::detail::put_u64(body, o.timestamp);
    }

    std::vector<std::uint8_t> out;
    protocol::detail::put_u32(out, kSnapshotMagic);
    protocol::detail::put_u32(out, kSnapshotVersion);
    protocol::detail::put_u64(out, d.last_seq);
    protocol::detail::put_u64(out, d.orders.size());
    protocol::detail::put_i64(out, d.last_trade_ticks);
    protocol::detail::put_u64(out, d.next_exchange_id);
    protocol::detail::put_u32(out, crc32(body.data(), body.size()));
    out.insert(out.end(), body.begin(), body.end());

    const std::string tmp = path + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        error = "cannot create " + tmp + ": " + std::strerror(errno);
        return false;
    }
    std::size_t off = 0;
    while (off < out.size()) {
        const ssize_t n = ::write(fd, out.data() + off, out.size() - off);
        if (n > 0) {
            off += static_cast<std::size_t>(n);
        } else if (!(n < 0 && errno == EINTR)) {
            error = std::string("snapshot write failed: ") + std::strerror(errno);
            ::close(fd);
            ::unlink(tmp.c_str());
            return false;
        }
    }
    // Durable BEFORE the rename. The other order leaves a correctly named file
    // whose contents are still only in the page cache.
#if defined(__APPLE__)
    static_cast<void>(::fcntl(fd, F_FULLFSYNC));
#else
    static_cast<void>(::fsync(fd));
#endif
    ::close(fd);

    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        error = std::string("snapshot rename failed: ") + std::strerror(errno);
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

// Returns false if the file is missing, malformed, or fails its checksum. A
// bad snapshot is not fatal — the caller falls back to replaying the whole log.
[[nodiscard]] inline bool read_snapshot(const std::string& path, SnapshotData& out,
                                        std::string& error) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        error = "cannot open " + path;
        return false;
    }
    std::vector<std::uint8_t> buf;
    std::uint8_t chunk[64 * 1024];
    for (;;) {
        const ssize_t n = ::read(fd, chunk, sizeof(chunk));
        if (n > 0) {
            buf.insert(buf.end(), chunk, chunk + n);
        } else if (!(n < 0 && errno == EINTR)) {
            break;
        }
    }
    ::close(fd);

    if (buf.size() < kSnapshotHeaderSize) {
        error = "snapshot too short";
        return false;
    }
    protocol::detail::Reader r(buf.data(), kSnapshotHeaderSize);
    if (r.u32() != kSnapshotMagic) {
        error = "not a snapshot file";
        return false;
    }
    if (r.u32() != kSnapshotVersion) {
        error = "unsupported snapshot version";
        return false;
    }
    out.last_seq = r.u64();
    const std::uint64_t count = r.u64();
    out.last_trade_ticks = r.i64();
    out.next_exchange_id = r.u64();
    const std::uint32_t want_crc = r.u32();
    if (!r.ok()) {
        error = "snapshot header malformed";
        return false;
    }

    const std::size_t body_size = buf.size() - kSnapshotHeaderSize;
    constexpr std::size_t kOrderSize = 45;
    if (count > body_size / kOrderSize) {
        error = "snapshot order count exceeds file size";
        return false;
    }
    const std::uint8_t* body = buf.data() + kSnapshotHeaderSize;
    if (crc32(body, body_size) != want_crc) {
        error = "snapshot checksum mismatch";
        return false;
    }

    protocol::detail::Reader br(body, body_size);
    out.orders.clear();
    out.orders.reserve(count);
    for (std::uint64_t i = 0; i < count; ++i) {
        SnapshotOrder o{};
        o.exchange_id = br.u64();
        o.session = br.u64();
        o.client_order_id = br.u64();
        o.price_ticks = br.i64();
        o.quantity = br.u32();
        o.side = br.u8();
        o.timestamp = br.u64();
        if (!br.ok() || o.side > 1) {
            error = "snapshot order record malformed";
            return false;
        }
        out.orders.push_back(o);
    }
    return true;
}

}  // namespace ome
