#pragma once

// ---------------------------------------------------------------------------
// Order gateway wire protocol v1. Specification: docs/PROTOCOL.md.
//
// Length-prefixed binary over TCP. Little-endian on the wire regardless of host
// byte order. Prices are int64 ticks; there is no floating-point field in this
// protocol at all.
//
// WHY FIELD-BY-FIELD SERIALIZATION AND NEVER memcpy OF A STRUCT:
//
//   Padding.     The compiler inserts padding between members for alignment.
//                sizeof(NewOrder) is not the sum of the field sizes, the pad
//                bytes are uninitialized, and their placement can differ
//                between compilers, versions, and even build flags. memcpy'ing
//                a struct puts uninitialized memory on the wire and makes the
//                message length an implementation detail of the compiler.
//
//   Endianness.  A struct copy writes host byte order. Two peers that disagree
//                then misread every integer, silently and consistently, which
//                is the worst kind of bug to find.
//
//   Versioning.  Explicit per-field encoding lets v2 append a field and still
//                decode a v1 message by checking the length. A struct copy pins
//                the format to one exact memory layout forever.
//
// The codec is header-only and dependency-free so the WAL (session 2.1) can
// reuse it for record payloads without dragging in the network layer.
// ---------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "ome/reject_reason.hpp"

namespace ome::protocol {

inline constexpr std::uint16_t kVersion = 1;

// A hostile or corrupt length field must not be able to make us allocate an
// arbitrary buffer. 64 KiB is far above the largest legitimate message (a
// full-depth BookUpdate is ~340 bytes).
inline constexpr std::uint32_t kMaxPayloadSize = 64 * 1024;

inline constexpr std::size_t kHeaderSize = 8;

// Append-only. Client->server occupies 1-9, server->client 10+.
enum class MessageType : std::uint16_t {
    NewOrder = 1,
    Cancel = 2,
    Modify = 3,
    Subscribe = 4,

    Ack = 10,
    Reject = 11,
    Fill = 12,
    BookUpdate = 13,
    Heartbeat = 14,
};

enum class Side : std::uint8_t { Bid = 0, Ask = 1 };
enum class OrderType : std::uint8_t { Market = 0, Limit = 1 };

struct MessageHeader {
    std::uint32_t length{};   // payload bytes following this header
    std::uint16_t type{};     // MessageType
    std::uint16_t version{};  // kVersion
};

// --- messages --------------------------------------------------------------

struct NewOrder {
    std::uint64_t client_order_id{};
    std::int64_t price_ticks{};  // ignored for Market
    std::uint32_t quantity{};
    Side side{Side::Bid};
    OrderType order_type{OrderType::Limit};
};

struct Cancel {
    std::uint64_t client_order_id{};
};

struct Modify {
    std::uint64_t client_order_id{};
    std::int64_t new_price_ticks{};
    std::uint32_t new_quantity{};
};

struct Subscribe {
    std::uint8_t depth{};  // clamped to 10 by the server
};

struct Ack {
    std::uint64_t client_order_id{};
    std::uint64_t exchange_order_id{};
};

struct Reject {
    std::uint64_t client_order_id{};
    RejectReason reason{RejectReason::NONE};
};

struct Fill {
    std::uint64_t exchange_order_id{};
    std::int64_t price_ticks{};
    std::uint32_t quantity{};
    std::uint32_t remaining_quantity{};
};

struct BookLevel {
    std::int64_t price_ticks{};
    std::uint64_t quantity{};
};

struct BookUpdate {
    std::uint64_t seq{};
    std::vector<BookLevel> bids;  // descending, <= 10
    std::vector<BookLevel> asks;  // ascending, <= 10
};

struct Heartbeat {
    std::uint64_t timestamp_ns{};
};

inline constexpr std::size_t kMaxBookDepth = 10;

// --- primitive encoding ----------------------------------------------------
//
// Explicit little-endian byte assembly. Not a reinterpret_cast, not a union,
// not htole64: this compiles to the same shifts on a little-endian machine and
// is correct without modification on a big-endian one.

namespace detail {

inline void put_u8(std::vector<std::uint8_t>& out, std::uint8_t v) { out.push_back(v); }

inline void put_u16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}

inline void put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
    }
}

inline void put_u64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
    }
}

// int64 -> uint64 via a bit copy, so the encoding is two's complement and does
// not depend on implementation-defined signed conversion.
inline void put_i64(std::vector<std::uint8_t>& out, std::int64_t v) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    put_u64(out, bits);
}

// A cursor over the payload. Every read is bounds-checked; a short buffer sets
// the failure flag rather than reading past the end. This is the class that
// makes a truncated frame a clean MALFORMED instead of a crash.
class Reader {
public:
    Reader(const std::uint8_t* data, std::size_t size) noexcept : data_(data), size_(size) {}

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return ok_ ? size_ - pos_ : 0; }

    std::uint8_t u8() noexcept {
        if (!need(1)) return 0;
        return data_[pos_++];
    }

    std::uint16_t u16() noexcept {
        if (!need(2)) return 0;
        const std::uint16_t v = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(data_[pos_]) |
            static_cast<std::uint16_t>(static_cast<std::uint16_t>(data_[pos_ + 1]) << 8));
        pos_ += 2;
        return v;
    }

    std::uint32_t u32() noexcept {
        if (!need(4)) return 0;
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            v |= static_cast<std::uint32_t>(data_[pos_ + static_cast<std::size_t>(i)])
                 << (8 * i);
        }
        pos_ += 4;
        return v;
    }

    std::uint64_t u64() noexcept {
        if (!need(8)) return 0;
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v |= static_cast<std::uint64_t>(data_[pos_ + static_cast<std::size_t>(i)])
                 << (8 * i);
        }
        pos_ += 8;
        return v;
    }

    std::int64_t i64() noexcept {
        const std::uint64_t bits = u64();
        std::int64_t v = 0;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }

private:
    [[nodiscard]] bool need(std::size_t n) noexcept {
        if (!ok_ || size_ - pos_ < n) {
            ok_ = false;
            return false;
        }
        return true;
    }

    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t pos_{0};
    bool ok_{true};
};

}  // namespace detail

// --- header ----------------------------------------------------------------

inline void encode_header(std::vector<std::uint8_t>& out, const MessageHeader& h) {
    detail::put_u32(out, h.length);
    detail::put_u16(out, h.type);
    detail::put_u16(out, h.version);
}

// Decodes a header from at least kHeaderSize bytes. Returns nullopt on a short
// buffer or an oversized length claim. Does NOT validate the type or version —
// that is the dispatcher's job, so it can reply with the right reason code.
[[nodiscard]] inline std::optional<MessageHeader> decode_header(const std::uint8_t* data,
                                                                std::size_t size) {
    if (size < kHeaderSize) {
        return std::nullopt;
    }
    detail::Reader r(data, size);
    MessageHeader h{};
    h.length = r.u32();
    h.type = r.u16();
    h.version = r.u16();
    if (!r.ok() || h.length > kMaxPayloadSize) {
        return std::nullopt;
    }
    return h;
}

// --- payload encode --------------------------------------------------------

inline void encode(std::vector<std::uint8_t>& out, const NewOrder& m) {
    detail::put_u64(out, m.client_order_id);
    detail::put_i64(out, m.price_ticks);
    detail::put_u32(out, m.quantity);
    detail::put_u8(out, static_cast<std::uint8_t>(m.side));
    detail::put_u8(out, static_cast<std::uint8_t>(m.order_type));
}

inline void encode(std::vector<std::uint8_t>& out, const Cancel& m) {
    detail::put_u64(out, m.client_order_id);
}

inline void encode(std::vector<std::uint8_t>& out, const Modify& m) {
    detail::put_u64(out, m.client_order_id);
    detail::put_i64(out, m.new_price_ticks);
    detail::put_u32(out, m.new_quantity);
}

inline void encode(std::vector<std::uint8_t>& out, const Subscribe& m) {
    detail::put_u8(out, m.depth);
}

inline void encode(std::vector<std::uint8_t>& out, const Ack& m) {
    detail::put_u64(out, m.client_order_id);
    detail::put_u64(out, m.exchange_order_id);
}

inline void encode(std::vector<std::uint8_t>& out, const Reject& m) {
    detail::put_u64(out, m.client_order_id);
    detail::put_u16(out, static_cast<std::uint16_t>(m.reason));
}

inline void encode(std::vector<std::uint8_t>& out, const Fill& m) {
    detail::put_u64(out, m.exchange_order_id);
    detail::put_i64(out, m.price_ticks);
    detail::put_u32(out, m.quantity);
    detail::put_u32(out, m.remaining_quantity);
}

inline void encode(std::vector<std::uint8_t>& out, const BookUpdate& m) {
    detail::put_u64(out, m.seq);
    // counts are u8 and depth is capped at 10, so a malicious count cannot
    // describe more levels than the payload could possibly hold.
    detail::put_u8(out, static_cast<std::uint8_t>(m.bids.size()));
    detail::put_u8(out, static_cast<std::uint8_t>(m.asks.size()));
    for (const auto& l : m.bids) {
        detail::put_i64(out, l.price_ticks);
        detail::put_u64(out, l.quantity);
    }
    for (const auto& l : m.asks) {
        detail::put_i64(out, l.price_ticks);
        detail::put_u64(out, l.quantity);
    }
}

inline void encode(std::vector<std::uint8_t>& out, const Heartbeat& m) {
    detail::put_u64(out, m.timestamp_ns);
}

// --- payload decode --------------------------------------------------------
//
// Each returns nullopt when the payload is too short OR has trailing bytes.
// Trailing bytes are rejected deliberately: within one version, a message has
// exactly one valid length, and silently ignoring extra bytes hides a peer
// that is encoding something else entirely.

template <typename T>
[[nodiscard]] std::optional<T> decode(const std::uint8_t* data, std::size_t size);

template <>
[[nodiscard]] inline std::optional<NewOrder> decode<NewOrder>(const std::uint8_t* data,
                                                              std::size_t size) {
    detail::Reader r(data, size);
    NewOrder m{};
    m.client_order_id = r.u64();
    m.price_ticks = r.i64();
    m.quantity = r.u32();
    const std::uint8_t side = r.u8();
    const std::uint8_t type = r.u8();
    if (!r.ok() || r.remaining() != 0) return std::nullopt;
    if (side > 1 || type > 1) return std::nullopt;  // undefined enumerator on the wire
    m.side = static_cast<Side>(side);
    m.order_type = static_cast<OrderType>(type);
    return m;
}

template <>
[[nodiscard]] inline std::optional<Cancel> decode<Cancel>(const std::uint8_t* data,
                                                          std::size_t size) {
    detail::Reader r(data, size);
    Cancel m{};
    m.client_order_id = r.u64();
    if (!r.ok() || r.remaining() != 0) return std::nullopt;
    return m;
}

template <>
[[nodiscard]] inline std::optional<Modify> decode<Modify>(const std::uint8_t* data,
                                                          std::size_t size) {
    detail::Reader r(data, size);
    Modify m{};
    m.client_order_id = r.u64();
    m.new_price_ticks = r.i64();
    m.new_quantity = r.u32();
    if (!r.ok() || r.remaining() != 0) return std::nullopt;
    return m;
}

template <>
[[nodiscard]] inline std::optional<Subscribe> decode<Subscribe>(const std::uint8_t* data,
                                                                std::size_t size) {
    detail::Reader r(data, size);
    Subscribe m{};
    m.depth = r.u8();
    if (!r.ok() || r.remaining() != 0) return std::nullopt;
    return m;
}

template <>
[[nodiscard]] inline std::optional<Ack> decode<Ack>(const std::uint8_t* data, std::size_t size) {
    detail::Reader r(data, size);
    Ack m{};
    m.client_order_id = r.u64();
    m.exchange_order_id = r.u64();
    if (!r.ok() || r.remaining() != 0) return std::nullopt;
    return m;
}

template <>
[[nodiscard]] inline std::optional<Reject> decode<Reject>(const std::uint8_t* data,
                                                          std::size_t size) {
    detail::Reader r(data, size);
    Reject m{};
    m.client_order_id = r.u64();
    const std::uint16_t reason = r.u16();
    if (!r.ok() || r.remaining() != 0) return std::nullopt;
    // An unrecognized reason code is NOT a decode failure: a newer server may
    // send a code this build predates, and the client must still be able to
    // read the message. to_string() renders it as "UNRECOGNIZED".
    m.reason = static_cast<RejectReason>(reason);
    return m;
}

template <>
[[nodiscard]] inline std::optional<Fill> decode<Fill>(const std::uint8_t* data, std::size_t size) {
    detail::Reader r(data, size);
    Fill m{};
    m.exchange_order_id = r.u64();
    m.price_ticks = r.i64();
    m.quantity = r.u32();
    m.remaining_quantity = r.u32();
    if (!r.ok() || r.remaining() != 0) return std::nullopt;
    return m;
}

template <>
[[nodiscard]] inline std::optional<BookUpdate> decode<BookUpdate>(const std::uint8_t* data,
                                                                  std::size_t size) {
    detail::Reader r(data, size);
    BookUpdate m{};
    m.seq = r.u64();
    const std::uint8_t nb = r.u8();
    const std::uint8_t na = r.u8();
    if (!r.ok()) return std::nullopt;
    if (nb > kMaxBookDepth || na > kMaxBookDepth) return std::nullopt;

    // reserve only after the counts are bounds-checked above: reserving on an
    // attacker-supplied count is how a length field becomes an allocation.
    m.bids.reserve(nb);
    m.asks.reserve(na);
    for (std::uint8_t i = 0; i < nb; ++i) {
        BookLevel l{};
        l.price_ticks = r.i64();
        l.quantity = r.u64();
        m.bids.push_back(l);
    }
    for (std::uint8_t i = 0; i < na; ++i) {
        BookLevel l{};
        l.price_ticks = r.i64();
        l.quantity = r.u64();
        m.asks.push_back(l);
    }
    if (!r.ok() || r.remaining() != 0) return std::nullopt;
    return m;
}

template <>
[[nodiscard]] inline std::optional<Heartbeat> decode<Heartbeat>(const std::uint8_t* data,
                                                                std::size_t size) {
    detail::Reader r(data, size);
    Heartbeat m{};
    m.timestamp_ns = r.u64();
    if (!r.ok() || r.remaining() != 0) return std::nullopt;
    return m;
}

// --- framing helper --------------------------------------------------------

// Encodes header + payload into a complete frame. The length is computed from
// the encoded payload rather than sizeof(T), which is exactly the point.
template <typename T>
[[nodiscard]] inline std::vector<std::uint8_t> encode_frame(MessageType type, const T& msg) {
    std::vector<std::uint8_t> payload;
    encode(payload, msg);

    std::vector<std::uint8_t> frame;
    frame.reserve(kHeaderSize + payload.size());
    MessageHeader h{};
    h.length = static_cast<std::uint32_t>(payload.size());
    h.type = static_cast<std::uint16_t>(type);
    h.version = kVersion;
    encode_header(frame, h);
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

}  // namespace ome::protocol
