#pragma once
// include-once for headers; portable alternative is #ifndef/#define include guards.

#include <cstdint>
// fixed-width ints from <cstdint> so sizes (e.g. uint64_t) are explicit across platforms.

struct Order {
    // unscoped enum: BID/ASK names leak into surrounding scope; they implicitly convert to int (unlike enum class).
    enum Side { BID, ASK };
    enum Type { MARKET, LIMIT, CANCEL };

    uint64_t id;
    double price;
    uint32_t quantity;
    Side side;
    Type type;
    uint64_t timestamp;

    // static member function: no "this"; call as Order::make(...) to build test orders with auto ids.
    static Order make(double price, uint32_t quantity, Side side, Type type, uint64_t timestamp) {
        // function-local static: single next_id for all calls; c++11+ guarantees thread-safe one-time init.
        static uint64_t next_id = 1;
        Order o;
        o.id = next_id++;
        o.price = price;
        o.quantity = quantity;
        o.side = side;
        o.type = type;
        o.timestamp = timestamp;
        return o;  // return by value; often no copy thanks to rvo/elision.
    }
};
