#pragma once

#include <cstdint>
// stdint fixed-width aliases for ids and sizes in trade prints.

struct Trade {
    // aggregate-type brace-or-equal initializers: members default to zero if omitted (value initialization).
    uint64_t buyer_id{};
    uint64_t seller_id{};
    double price{};
    uint32_t quantity{};
    uint64_t timestamp{};
};
