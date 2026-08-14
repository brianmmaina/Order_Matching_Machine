#include "lobster/lobster_parser.hpp"

#include <cctype>
// cctype: std::isspace for portable ascii/locale-safe-ish checks if cast to unsigned char.

#include <cmath>
// std::isfinite, std::llround for timestamp validation and rounding.

#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
// unnamed namespace: internal linkage — these helpers are visible only in this .cpp (like "static" file scope in c).

// constexpr: compile-time constant; may be used in array sizes etc.; here just a named literal.

void trim_in_place(std::string& s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.erase(s.begin());
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
}

bool split_csv_line(const std::string& line, std::vector<std::string>& fields) {
    fields.clear();
    std::string cell;
    std::stringstream ss(line);
    // std::getline with delimiter splits on commas; does not handle quoted fields (not needed for simple lobster rows).
    while (std::getline(ss, cell, ',')) {
        trim_in_place(cell);
        fields.push_back(cell);
    }
    return true;
}

bool parse_timestamp(const std::string& field, uint64_t& out_ts) {
    try {
        const long double t = std::stold(field);
        if (!std::isfinite(static_cast<long double>(t)) || t < 0.0L) {
            return false;
        }
        constexpr long double kMicro = 1000000.0L;
        const auto micro = static_cast<long double>(std::llround(t * kMicro));
        if (micro < 0.0L ||
            micro > static_cast<long double>(std::numeric_limits<uint64_t>::max())) {
            return false;
        }
        out_ts = static_cast<uint64_t>(micro);
        return true;
    } catch (const std::exception&) {
        // stold throws on bad parse; swallow and let caller skip row (graceful io).
        return false;
    }
}

bool map_message_type(int code, Order::Type& out) {
    switch (code) {
        case 1:
            out = Order::LIMIT;
            return true;
        case 2:
        case 3:
            out = Order::CANCEL;
            return true;
        case 4:
        case 5:
            out = Order::MARKET;
            return true;
        default:
            return false;
    }
}

bool parse_row_message(const std::vector<std::string>& fields, LobsterMessage& out) {
    if (fields.size() != 6) {
        return false;
    }

    uint64_t ts = 0;
    if (!parse_timestamp(fields[0], ts)) {
        return false;
    }

    int msg_type = 0;
    try {
        msg_type = std::stoi(fields[1]);
    } catch (const std::exception&) {
        return false;
    }
    if (msg_type < 1 || (msg_type > 5 && msg_type != 7)) {
        return false;
    }

    if (msg_type == 7) {
        // trading halt / resume — see LOBSTER readme; fields may be zeros or -1.
        out.timestamp_us = ts;
        out.type = 7;
        try {
            out.order_id = std::stoull(fields[2]);
        } catch (const std::exception&) {
            out.order_id = 0;
        }
        try {
            out.size = static_cast<uint32_t>(std::stoull(fields[3]));
        } catch (const std::exception&) {
            out.size = 0;
        }
        try {
            out.price_ticks = std::stoll(fields[4]);
        } catch (const std::exception&) {
            out.price_ticks = -1;
        }
        try {
            out.direction = std::stoi(fields[5]);
        } catch (const std::exception&) {
            out.direction = -1;
        }
        return true;
    }

    uint64_t order_id = 0;
    try {
        order_id = std::stoull(fields[2]);
    } catch (const std::exception&) {
        return false;
    }
    // executions (4/5) often use order id 0 in LOBSTER samples; limits/cancels require a real id.
    if (order_id == 0 && msg_type != 4 && msg_type != 5) {
        return false;
    }

    unsigned long long size_ull = 0;
    try {
        size_ull = std::stoull(fields[3]);
    } catch (const std::exception&) {
        return false;
    }
    if (size_ull > std::numeric_limits<uint32_t>::max()) {
        return false;
    }

    long long price_ticks = 0;
    try {
        price_ticks = std::stoll(fields[4]);
    } catch (const std::exception&) {
        return false;
    }
    if (price_ticks < 0) {
        return false;
    }

    int dir = 0;
    try {
        dir = std::stoi(fields[5]);
    } catch (const std::exception&) {
        return false;
    }
    if (dir != 1 && dir != -1) {
        return false;
    }

    out.timestamp_us = ts;
    out.type = msg_type;
    out.order_id = order_id;
    out.size = static_cast<uint32_t>(size_ull);
    out.price_ticks = price_ticks;
    out.direction = dir;
    return true;
}

bool parse_row(const std::vector<std::string>& fields, Order& out) {
    LobsterMessage m{};
    if (!parse_row_message(fields, m)) {
        return false;
    }

    Order::Type ty{};
    if (!map_message_type(m.type, ty)) {
        return false;
    }

    out.id = m.order_id;
    out.price_ticks = m.price_ticks;
    out.quantity = m.size;
    out.side = (m.direction == 1) ? Order::BID : Order::ASK;
    out.type = ty;
    out.timestamp = m.timestamp_us;
    return true;
}

bool line_all_whitespace(const std::string& line) {
    for (const char ch : line) {
        // std::isspace takes an int that must be representable as unsigned char (or EOF);
        // passing a negative char is UB, so widen through unsigned char explicitly rather
        // than letting the char->unsigned char conversion happen implicitly.
        if (!std::isspace(static_cast<unsigned char>(ch))) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::vector<Order> LobsterParser::parse(std::istream& input) {
    std::vector<Order> orders;
    std::string line;
    std::vector<std::string> fields;
    while (std::getline(input, line)) {
        if (line.empty() || line_all_whitespace(line)) {
            continue;
        }
        split_csv_line(line, fields);
        Order row{};
        if (!parse_row(fields, row)) {
            continue;
        }
        orders.push_back(row);
    }
    return orders;
}

std::vector<Order> LobsterParser::parse_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        return {};
    }
    return parse(in);
}

std::vector<LobsterMessage> LobsterParser::parse_messages(std::istream& input) {
    std::vector<LobsterMessage> rows;
    std::string line;
    std::vector<std::string> fields;
    while (std::getline(input, line)) {
        if (line.empty() || line_all_whitespace(line)) {
            continue;
        }
        split_csv_line(line, fields);
        LobsterMessage m{};
        if (!parse_row_message(fields, m)) {
            continue;
        }
        rows.push_back(m);
    }
    return rows;
}

std::vector<LobsterMessage> LobsterParser::parse_messages_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        return {};
    }
    return parse_messages(in);
}
