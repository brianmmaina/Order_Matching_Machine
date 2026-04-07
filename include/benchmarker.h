#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <vector>

#include "matching_engine/matching_engine.hpp"
#include "order.h"

// wraps MatchingEngine for micro-benchmarking: per-call latency percentiles or burst throughput.
class Benchmarker {
public:
    explicit Benchmarker(MatchingEngine& engine) : engine_(engine) {}

    // times each processOrder; fills last_latency_ns_ (nanoseconds). prints p50/p99/p999 after all N orders.
    void run_latency(const std::vector<Order>& orders) {
        last_latency_ns_.clear();
        last_latency_ns_.reserve(orders.size());

        for (const Order& o : orders) {
            Order copy = o;
            const auto t0 = std::chrono::high_resolution_clock::now();
            engine_.processOrder(std::move(copy));
            const auto t1 = std::chrono::high_resolution_clock::now();
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
            last_latency_ns_.push_back(static_cast<std::int64_t>(ns));
            optimizer_sink_ ^= static_cast<std::uint64_t>(ns);
            optimizer_sink_ += static_cast<std::uint32_t>(last_latency_ns_.size());
        }

        std::vector<std::int64_t> sorted = last_latency_ns_;
        std::sort(sorted.begin(), sorted.end());
        const std::int64_t p50 = percentile_sorted(sorted, 0.50);
        const std::int64_t p99 = percentile_sorted(sorted, 0.99);
        const std::int64_t p999 = percentile_sorted(sorted, 0.999);

        std::cout << "[benchmarker latency] N=" << orders.size() << " p50_ns=" << p50 << " p99_ns=" << p99
                  << " p999_ns=" << p999 << "\n";
    }

    // runs N pre-generated orders back-to-back; prints wall time and orders/sec.
    void run_throughput(const std::vector<Order>& orders) {
        const auto t0 = std::chrono::high_resolution_clock::now();
        for (const Order& o : orders) {
            engine_.processOrder(o);
            optimizer_sink_ ^= o.id;
            optimizer_sink_ += static_cast<std::uint64_t>(o.quantity);
            optimizer_sink_ ^= static_cast<std::uint64_t>(static_cast<unsigned>(o.side) << 16U);
        }
        const auto t1 = std::chrono::high_resolution_clock::now();
        const double sec =
            std::chrono::duration_cast<std::chrono::duration<double>>(t1 - t0).count();
        const double ops = sec > 0.0 ? static_cast<double>(orders.size()) / sec : 0.0;
        std::cout << "[benchmarker throughput] N=" << orders.size() << " total_s=" << sec
                  << " orders_per_sec=" << ops << "\n";
        (void)optimizer_sink_;
    }

    [[nodiscard]] const std::vector<std::int64_t>& last_latency_ns() const noexcept { return last_latency_ns_; }

private:
    static std::int64_t percentile_sorted(const std::vector<std::int64_t>& sorted, double q) {
        if (sorted.empty()) {
            return 0;
        }
        const std::size_t n = sorted.size();
        const std::size_t idx = static_cast<std::size_t>((n - 1U) * q);
        return sorted[idx];
    }

    MatchingEngine& engine_;
    std::vector<std::int64_t> last_latency_ns_;
    volatile std::uint64_t optimizer_sink_{0};
};
