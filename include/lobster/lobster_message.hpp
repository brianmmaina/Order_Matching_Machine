#pragma once

#include <cstdint>

// one raw lobster csv row: time,type,order_id,size,price_ticks,direction
// price kept as integer ticks (same as file column 5); time stored in microseconds like LobsterParser.
struct LobsterMessage {
    uint64_t timestamp_us{};
    int type{};
    uint64_t order_id{};
    uint32_t size{};
    int64_t price_ticks{};
    int direction{};  // 1 buy/bid, -1 sell/ask
};
