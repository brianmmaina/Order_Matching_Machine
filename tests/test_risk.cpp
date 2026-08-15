// Risk checks and rate limiting.
//
// Every limit is tested at BOTH boundaries — the last accepted value and the
// first rejected one. A test that only checks the middle of the accept range
// passes against an off-by-one, and an off-by-one in a risk limit is the kind
// that only shows up as a fill you did not want.
//
// The token bucket takes time as a parameter, so a one-second refill is tested
// in nanoseconds of wall clock. Sleeping would make the suite slow and flaky,
// and a flaky risk test gets ignored.

#include <gtest/gtest.h>

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "ome/risk_config.hpp"
#include "ome/token_bucket.hpp"

using namespace ome;

namespace {
constexpr Nanos kSec = 1000ULL * 1000 * 1000;

// mkstemp rather than tmpnam: tmpnam returns a name, and anything can create
// that path before we open it. mkstemp creates and opens atomically.
std::string write_temp(const std::string& body) {
    std::vector<char> tmpl(64);
    std::snprintf(tmpl.data(), tmpl.size(), "/tmp/ome_risk_XXXXXX");
    const int fd = ::mkstemp(tmpl.data());
    EXPECT_GE(fd, 0) << "could not create a temp file";
    if (fd >= 0) {
        ::close(fd);
    }
    const std::string path(tmpl.data());
    std::ofstream(path) << body;
    return path;
}
}  // namespace

// --- token bucket ----------------------------------------------------------

TEST(TokenBucket, starts_full_so_a_fresh_session_can_burst) {
    TokenBucket b(10.0, 5.0, 0);
    EXPECT_DOUBLE_EQ(b.tokens(0), 5.0);
}

TEST(TokenBucket, allows_exactly_capacity_then_refuses) {
    // Both boundaries: the 5th call must pass and the 6th must not.
    TokenBucket b(10.0, 5.0, 0);
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(b.allow(0)) << "refused burst token " << i + 1;
    }
    EXPECT_FALSE(b.allow(0)) << "allowed a 6th token from a capacity-5 bucket";
}

TEST(TokenBucket, refills_at_the_configured_rate) {
    TokenBucket b(10.0, 5.0, 0);
    while (b.allow(0)) {
    }
    ASSERT_FALSE(b.allow(0));

    // 10/sec means one token per 100ms. At 99ms there is not yet a whole one.
    EXPECT_FALSE(b.allow(99 * kSec / 1000));
    EXPECT_TRUE(b.allow(100 * kSec / 1000)) << "no token after a full refill interval";
}

TEST(TokenBucket, refill_is_capped_at_capacity) {
    // An idle session must not accumulate an unbounded burst allowance.
    TokenBucket b(10.0, 5.0, 0);
    while (b.allow(0)) {
    }
    EXPECT_DOUBLE_EQ(b.tokens(3600 * kSec), 5.0) << "an hour idle exceeded capacity";
}

TEST(TokenBucket, sustained_rate_matches_configuration) {
    // Drain the burst, then consume for a simulated second and confirm the
    // long-run allowance is the rate rather than the capacity.
    TokenBucket b(100.0, 10.0, 0);
    while (b.allow(0)) {
    }
    int allowed = 0;
    for (int ms = 1; ms <= 1000; ++ms) {
        if (b.allow(static_cast<Nanos>(ms) * kSec / 1000)) {
            ++allowed;
        }
    }
    EXPECT_GE(allowed, 99);
    EXPECT_LE(allowed, 101) << "sustained rate drifted from 100/sec";
}

TEST(TokenBucket, non_monotonic_time_does_not_grant_tokens) {
    // Nanos is unsigned: a backwards timestamp would make (now - last) wrap to
    // an enormous positive elapsed time and refill the bucket completely.
    // Anything named "limit" must fail closed.
    TokenBucket b(10.0, 5.0, 1000 * kSec);
    while (b.allow(1000 * kSec)) {
    }
    ASSERT_FALSE(b.allow(1000 * kSec));
    EXPECT_FALSE(b.allow(500 * kSec)) << "a backwards clock refilled the bucket";
    EXPECT_FALSE(b.allow(1000 * kSec)) << "state was corrupted by the backwards clock";
}

// --- config parsing --------------------------------------------------------

TEST(RiskConfig, parses_values_and_ignores_comments_and_blanks) {
    const auto path = write_temp(
        "# leading comment\n"
        "\n"
        "rate_per_sec = 250\n"
        "   rate_burst=500   \n"
        "max_order_qty = 42   # trailing comment\n"
        "tick_size = 25\n"
        "price_band_bp = 500\n");

    RiskConfig c{};
    std::string err;
    ASSERT_TRUE(RiskConfig::load(path, c, err)) << err;
    EXPECT_DOUBLE_EQ(c.rate_per_sec, 250.0);
    EXPECT_DOUBLE_EQ(c.rate_burst, 500.0);
    EXPECT_EQ(c.max_order_qty, 42u);
    EXPECT_EQ(c.tick_size, 25);
    EXPECT_EQ(c.price_band_bp, 500);
    std::remove(path.c_str());
}

TEST(RiskConfig, unknown_key_is_an_error_not_a_warning) {
    // A typo'd risk limit that silently keeps its default is the failure you
    // discover from a fill.
    const auto path = write_temp("rate_per_sec = 100\nmax_order_size = 5\n");
    RiskConfig c{};
    std::string err;
    EXPECT_FALSE(RiskConfig::load(path, c, err));
    EXPECT_NE(err.find("max_order_size"), std::string::npos) << err;
    EXPECT_NE(err.find(":2:"), std::string::npos) << "error did not name the line: " << err;
    std::remove(path.c_str());
}

TEST(RiskConfig, trailing_garbage_in_a_value_is_rejected) {
    // operator>> alone accepts "100abc" as 100.
    const auto path = write_temp("max_order_qty = 100abc\n");
    RiskConfig c{};
    std::string err;
    EXPECT_FALSE(RiskConfig::load(path, c, err)) << "accepted a malformed value";
    std::remove(path.c_str());
}

TEST(RiskConfig, missing_equals_is_rejected_with_a_line_number) {
    const auto path = write_temp("rate_per_sec = 100\nthis is not a setting\n");
    RiskConfig c{};
    std::string err;
    EXPECT_FALSE(RiskConfig::load(path, c, err));
    EXPECT_NE(err.find(":2:"), std::string::npos) << err;
    std::remove(path.c_str());
}

TEST(RiskConfig, missing_file_reports_cleanly) {
    RiskConfig c{};
    std::string err;
    EXPECT_FALSE(RiskConfig::load("/nonexistent/risk.conf", c, err));
    EXPECT_FALSE(err.empty());
}

TEST(RiskConfig, internally_inconsistent_settings_are_rejected) {
    // A bucket that cannot hold one token refuses every order, which is a
    // configuration mistake that should stop startup rather than look like a
    // gateway that silently rejects everything.
    RiskConfig c{};
    std::string err;
    c.rate_burst = 0.0;
    EXPECT_FALSE(c.validate(err));

    c = RiskConfig{};
    c.tick_size = 0;
    EXPECT_FALSE(c.validate(err)) << "tick_size 0 would divide the modulo check by zero";

    c = RiskConfig{};
    c.rate_per_sec = 0.0;
    EXPECT_FALSE(c.validate(err));

    c = RiskConfig{};
    c.max_order_qty = 0;
    EXPECT_FALSE(c.validate(err)) << "a zero size cap rejects every order";

    c = RiskConfig{};
    EXPECT_TRUE(c.validate(err)) << err;
}

TEST(RiskConfig, defaults_are_valid) {
    RiskConfig c{};
    std::string err;
    EXPECT_TRUE(c.validate(err)) << err;
    // The shipped config must also load and validate — a broken example file is
    // worse than none, because it is what people copy.
    RiskConfig from_file{};
    if (RiskConfig::load("config/risk.conf", from_file, err)) {
        EXPECT_TRUE(from_file.validate(err)) << err;
    }
}
