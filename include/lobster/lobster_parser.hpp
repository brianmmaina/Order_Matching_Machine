#pragma once

#include <istream>
// abstract input: parse can read from ifstream, stringstream, or any std::istream implementation.

#include <string>
#include <vector>

#include "lobster/lobster_message.hpp"
#include "order.h"

// csv row shape: time,type,order_id,size,price,direction — lobster ticks scaled by /10000 for double price.
class LobsterParser {
public:
    // static methods: no parser state; pure functions on input.
    [[nodiscard]] static std::vector<Order> parse(std::istream& input);
    [[nodiscard]] static std::vector<Order> parse_file(const std::string& path);

    // preserves lobster integer type (1..5) for replay / validation (partial vs full cancel etc.).
    [[nodiscard]] static std::vector<LobsterMessage> parse_messages(std::istream& input);
    [[nodiscard]] static std::vector<LobsterMessage> parse_messages_file(const std::string& path);
};
