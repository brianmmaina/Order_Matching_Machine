#include "lobster/lobster_orderbook_converter.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

void trim(std::string& s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.erase(s.begin());
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
}

void split_csv(const std::string& line, std::vector<std::string>& fields) {
    fields.clear();
    std::string cell;
    std::istringstream ss(line);
    while (std::getline(ss, cell, ',')) {
        trim(cell);
        fields.push_back(cell);
    }
}

}  // namespace

bool write_validator_snapshot_from_lobster_orderbook_row(const std::string& csv_line, std::ostream& out,
                                                         std::size_t levels) {
    std::vector<std::string> f;
    split_csv(csv_line, f);
    if (f.size() < 4 || (f.size() % 4) != 0) {
        return false;
    }
    const std::size_t avail = f.size() / 4;
    const std::size_t L = std::min(levels, avail);

    for (std::size_t i = 0; i < levels; ++i) {
        if (i >= L) {
            out << "0,0\n";
            continue;
        }
        const std::string& bid_p = f[4 * i + 2];
        const std::string& bid_s = f[4 * i + 3];
        long long px = 0;
        unsigned long long sz = 0;
        try {
            px = std::stoll(bid_p);
            sz = std::stoull(bid_s);
        } catch (const std::exception&) {
            return false;
        }
        if (px <= 0 || sz == 0) {
            out << "0,0\n";
        } else {
            out << px << ',' << sz << '\n';
        }
    }

    for (std::size_t i = 0; i < levels; ++i) {
        if (i >= L) {
            out << "0,0\n";
            continue;
        }
        const std::string& ask_p = f[4 * i + 0];
        const std::string& ask_s = f[4 * i + 1];
        long long px = 0;
        unsigned long long sz = 0;
        try {
            px = std::stoll(ask_p);
            sz = std::stoull(ask_s);
        } catch (const std::exception&) {
            return false;
        }
        if (px <= 0 || sz == 0) {
            out << "0,0\n";
        } else {
            out << px << ',' << sz << '\n';
        }
    }
    return true;
}

bool read_orderbook_line_at_index(const std::string& path, std::size_t line_index, std::string& out_line) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::string line;
    std::size_t logical = 0;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        if (logical == line_index) {
            out_line = line;
            return true;
        }
        ++logical;
    }
    return false;
}
