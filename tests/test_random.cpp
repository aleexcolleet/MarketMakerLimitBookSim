// Tests for the random source and the latent value process.
//
// Every test here is seeded. An unseeded statistical test fails one run in a
// hundred, and a suite that fails occasionally is a suite people stop reading.
// Seeded, these are regression tests: any change to the generator that moves
// these numbers is a change someone has to justify. The tolerances are set from
// the sampling distribution — roughly five standard errors — so they are wide
// enough not to be brittle and narrow enough to catch a real error.

#include "mms/random.hpp"
#include "mms/value_process.hpp"

#include "check.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

using namespace mms;

namespace {

constexpr std::uint64_t kSeed = 0xC0FFEEull;

bool near(double a, double b, double tol) { return std::fabs(a - b) < tol; }

// ---------------------------------------------------------------------------
//  Determinism — the property everything downstream depends on
// ---------------------------------------------------------------------------

void same_seed_gives_the_same_stream() {
    Rng a(kSeed), b(kSeed);
    bool identical = true;
    for (int i = 0; i < 10000; ++i) if (a.next_u64() != b.next_u64()) identical = false;
    CHECK(identical);
}

void different_seeds_diverge() {
    Rng a(1), b(2);
    int collisions = 0;
    for (int i = 0; i < 10000; ++i) if (a.next_u64() == b.next_u64()) ++collisions;
    CHECK(collisions == 0);
}

void a_zero_seed_is_not_degenerate() {
    // splitmix64 advances its state by an odd constant, so every seed sits on
    // the same full-period cycle. A large-state generator like mt19937 takes
    // thousands of draws to recover from a mostly-zero seed.
    Rng z(0);
    double sum = 0.0;
    for (int i = 0; i < 10000; ++i) sum += z.uniform01();
    CHECK(near(sum / 10000.0, 0.5, 0.02));
}

// ---------------------------------------------------------------------------
//  Uniforms
// ---------------------------------------------------------------------------

void uniform01_stays_in_range() {
    // Accumulate, then assert once. A CHECK inside a 200,000-iteration loop
    // inflates the assertion count by 200,000 and, on failure, prints 200,000
    // nearly identical lines instead of one.
    Rng r(kSeed);
    bool in_range = true;
    for (int i = 0; i < 200000; ++i) {
        const double u = r.uniform01();
        if (!(u >= 0.0 && u < 1.0)) in_range = false;
    }
    CHECK(in_range);
}

void uniform01_is_flat() {
    Rng r(kSeed);
    constexpr int kBuckets = 10;
    constexpr int kDraws = 200000;
    std::vector<int> hits(kBuckets, 0);
    for (int i = 0; i < kDraws; ++i) {
        ++hits[static_cast<std::size_t>(r.uniform01() * kBuckets)];
    }
    // 20,000 expected per bucket, s.d. ~134. A 600 band is about 4.5 s.d.
    bool flat = true;
    for (int h : hits) if (h < 19400 || h > 20600) flat = false;
    CHECK(flat);
}

void uniform_int_covers_its_range() {
    // Worth being clear about what this catches: gross errors — an off-by-one
    // in the inclusive range, a wrong shift. It does *not* catch modulo bias.
    // At 64 bits that bias is about 1 part in 2^62, and no test at any feasible
    // sample size could see it. Its absence is established by the rejection
    // argument in random.hpp, not by this test. Some correctness properties are
    // only reachable by reasoning.
    Rng r(kSeed);
    constexpr std::int64_t lo = -3, hi = 3;
    constexpr int kDraws = 140000;
    std::vector<int> hits(7, 0);
    bool in_range = true;
    for (int i = 0; i < kDraws; ++i) {
        const std::int64_t v = r.uniform_int(lo, hi);
        if (v < lo || v > hi) { in_range = false; break; }
        ++hits[static_cast<std::size_t>(v - lo)];
    }
    CHECK(in_range);
    bool flat = true;
    for (int h : hits) if (h < 19400 || h > 20600) flat = false;
    CHECK(flat);
}

void uniform_int_reaches_both_endpoints() {
    // Off-by-one in an inclusive range is the classic bug and it is silent:
    // the distribution still looks fine, it is just missing one value.
    Rng r(kSeed);
    bool saw_lo = false, saw_hi = false;
    for (int i = 0; i < 5000; ++i) {
        const std::int64_t v = r.uniform_int(0, 99);
        if (v == 0)  saw_lo = true;
        if (v == 99) saw_hi = true;
    }
    CHECK(saw_lo);
    CHECK(saw_hi);
}

void uniform_int_with_an_empty_range_returns_lo() {
    Rng r(kSeed);
    CHECK(r.uniform_int(7, 7) == 7);
    CHECK(r.uniform_int(7, 3) == 7);
}

void uniform_int_handles_the_full_signed_range() {
    // This test found a real bug — see the note below. The count of values in
    // [INT64_MIN, INT64_MAX] is 2^64, which does not fit in a uint64_t.
    Rng r(kSeed);
    constexpr std::int64_t lo = std::numeric_limits<std::int64_t>::min();
    constexpr std::int64_t hi = std::numeric_limits<std::int64_t>::max();
    bool seen_negative = false, seen_positive = false;
    for (int i = 0; i < 1000; ++i) {
        const std::int64_t v = r.uniform_int(lo, hi);
        if (v < 0) seen_negative = true;
        if (v > 0) seen_positive = true;
    }
    CHECK(seen_negative);
    CHECK(seen_positive);

    // And the range just below it, which takes the rejection path with a
    // divisor of 2^63 + 1.
    bool half_ok = true;
    for (int i = 0; i < 1000; ++i) {
        const std::int64_t v = r.uniform_int(lo, 0);
        if (v > 0) half_ok = false;
    }
    CHECK(half_ok);
}

// ---------------------------------------------------------------------------
//  Bernoulli
// ---------------------------------------------------------------------------

void bernoulli_respects_its_extremes() {
    // Only true because uniform01() is half-open: p = 0 never fires because
    // u >= 0, and p = 1 always fires because u < 1.
    Rng r(kSeed);
    bool any_true = false, any_false = false;
    for (int i = 0; i < 10000; ++i) {
        if (r.bernoulli(0.0))  any_true = true;
        if (!r.bernoulli(1.0)) any_false = true;
    }
    CHECK(!any_true);
    CHECK(!any_false);
}

void bernoulli_matches_its_probability() {
    Rng r(kSeed);
    int hits = 0;
    for (int i = 0; i < 100000; ++i) if (r.bernoulli(0.3)) ++hits;
    // s.d. of the count is sqrt(n p (1-p)) ~ 145. A 750 band is about 5 s.d.
    CHECK(near(static_cast<double>(hits), 30000.0, 750.0));
}

// ---------------------------------------------------------------------------
//  Exponential
// ---------------------------------------------------------------------------

void exponential_is_non_negative_and_finite() {
    Rng r(kSeed);
    bool ok = true;
    for (int i = 0; i < 200000; ++i) {
        const double x = r.exponential(3.0);
        if (!(x >= 0.0) || !std::isfinite(x)) ok = false;
    }
    CHECK(ok);
}

void exponential_has_the_right_shape() {
    // Mean and s.d. of an exponential are both 1/rate. Checking the second
    // moment as well as the first is what distinguishes "exponential" from
    // "something with the right average".
    Rng r(kSeed);
    constexpr double rate = 2.0;
    constexpr int n = 400000;
    double s1 = 0.0, s2 = 0.0;
    for (int i = 0; i < n; ++i) {
        const double x = r.exponential(rate);
        s1 += x;
        s2 += x * x;
    }
    const double mean = s1 / static_cast<double>(n);
    const double var  = s2 / static_cast<double>(n) - mean * mean;
    CHECK(near(mean, 0.5, 0.01));
    CHECK(near(var, 0.25, 0.01));
}

// ---------------------------------------------------------------------------
//  Normal
// ---------------------------------------------------------------------------

void normal_has_mean_zero_and_unit_variance() {
    Rng r(kSeed);
    constexpr int n = 400000;
    double s1 = 0.0, s2 = 0.0;
    for (int i = 0; i < n; ++i) {
        const double x = r.normal();
        s1 += x;
        s2 += x * x;
    }
    const double mean = s1 / static_cast<double>(n);
    CHECK(near(mean, 0.0, 0.02));
    CHECK(near(s2 / static_cast<double>(n) - mean * mean, 1.0, 0.02));
}

void normal_has_normal_tails() {
    // The test above would also pass for a *uniform* on [-sqrt(3), sqrt(3)]:
    // mean 0, variance 1, not remotely normal. The tail fractions are what say
    // "normal". General lesson — a test that checks a summary statistic checks
    // the summary statistic, not the thing. Ask what else would pass.
    Rng r(kSeed);
    constexpr int n = 400000;
    int w1 = 0, w2 = 0, w3 = 0;
    for (int i = 0; i < n; ++i) {
        const double x = std::fabs(r.normal());
        if (x < 1.0) ++w1;
        if (x < 2.0) ++w2;
        if (x < 3.0) ++w3;
    }
    const double d = static_cast<double>(n);
    CHECK(near(static_cast<double>(w1) / d, 0.6827, 0.005));
    CHECK(near(static_cast<double>(w2) / d, 0.9545, 0.003));
    CHECK(near(static_cast<double>(w3) / d, 0.9973, 0.001));
}

// ---------------------------------------------------------------------------
//  The latent value
// ---------------------------------------------------------------------------

void value_starts_where_it_was_told_to() {
    Rng r(kSeed);
    ValueParams p;
    p.initial = 12345.0;
    ValueProcess v(p, r);
    CHECK(v.value() == 12345.0);
    CHECK(v.value_ticks() == 12345);
}

void a_still_value_does_not_move() {
    Rng r(kSeed);
    ValueParams p;
    p.volatility = 0.0;
    p.jump_intensity = 0.0;
    ValueProcess v(p, r);
    for (int i = 0; i < 10000; ++i) v.step();
    CHECK(v.value() == p.initial);
}

void the_same_seed_reproduces_the_same_path() {
    // Exact float comparison, and it is the right call here.
    //
    // The rule "never compare floats for equality" is about *computed* values —
    // arithmetic that should agree often doesn't. This is asking something
    // different: did these two runs execute the identical sequence of
    // operations? IEEE-754 is deterministic, so if they differ by one bit the
    // runs diverged — and approximate equality would hide exactly the failure
    // this test exists to catch.
    Rng ra(kSeed), rb(kSeed);
    ValueParams p;
    ValueProcess a(p, ra), b(p, rb);
    bool identical = true;
    for (int i = 0; i < 20000; ++i) {
        a.step();
        b.step();
        if (a.value() != b.value()) identical = false;
    }
    CHECK(identical);
}

void the_value_is_a_martingale() {
    // No drift: over many independent paths the average end point is the start
    // point. If this drifts, something in the increment is asymmetric — which
    // would hand one side of the book a free edge and quietly invalidate every
    // P&L number the simulator ever produces.
    ValueParams p;
    p.volatility = 0.5;
    p.jump_intensity = 0.01;
    p.jump_size = 20.0;

    constexpr int paths = 4000, steps = 500;
    double sum_end = 0.0;
    for (int k = 0; k < paths; ++k) {
        Rng r(kSeed + static_cast<std::uint64_t>(k));
        ValueProcess v(p, r);
        for (int s = 0; s < steps; ++s) v.step();
        sum_end += v.value() - p.initial;
    }
    // Per-step variance is vol^2 + P(jump)*jump_size^2 = 0.25 + 4 = 4.25.
    // After 500 steps, s.d. ~46; over 4,000 paths the s.e. of the mean is ~0.73.
    // A band of 4 is about 5.5 s.e.
    std::printf("  [martingale] mean end drift = %.4f\n", sum_end / paths);
    CHECK(near(sum_end / static_cast<double>(paths), 0.0, 4.0));
}

void the_variance_grows_linearly_in_time() {
    // Independent increments: variance after n steps is n times the variance of
    // one. This is what makes it a random walk rather than something
    // mean-reverting, and it is the assumption every markout horizon later in
    // the project depends on.
    ValueParams p;
    p.volatility = 0.5;
    p.jump_intensity = 0.0;

    constexpr int paths = 4000;
    auto variance_after = [&](int steps) {
        double s1 = 0.0, s2 = 0.0;
        for (int k = 0; k < paths; ++k) {
            Rng r(0xABCDEFull + static_cast<std::uint64_t>(k));
            ValueProcess v(p, r);
            for (int s = 0; s < steps; ++s) v.step();
            const double d = v.value() - p.initial;
            s1 += d;
            s2 += d * d;
        }
        const double m = s1 / static_cast<double>(paths);
        return s2 / static_cast<double>(paths) - m * m;
    };

    const double v100 = variance_after(100);
    const double v400 = variance_after(400);
    std::printf("  [variance] v100 = %.4f  v400 = %.4f  ratio = %.4f\n",
                v100, v400, v400 / v100);
    CHECK(near(v100, 25.0, 2.5));         // 100 * 0.5^2
    CHECK(near(v400 / v100, 4.0, 0.4));   // four times the steps, four times the variance
}

void jumps_fatten_the_tails() {
    // The whole reason the jump component exists. Same volatility parameter,
    // two configurations, and the largest single-event move differs by roughly
    // thirty times. That difference is what separates a simulation that
    // flatters a quoting strategy from one that tests it.
    ValueParams calm;
    calm.volatility = 0.5;
    calm.jump_intensity = 0.0;

    ValueParams jumpy = calm;
    jumpy.jump_intensity = 0.01;
    jumpy.jump_size = 20.0;

    constexpr int paths = 4000, steps = 200;
    auto max_abs_move = [&](const ValueParams& p) {
        double worst = 0.0;
        for (int k = 0; k < paths; ++k) {
            Rng r(0x5EEDull + static_cast<std::uint64_t>(k));
            ValueProcess v(p, r);
            double prev = v.value();
            for (int s = 0; s < steps; ++s) {
                v.step();
                worst = std::fmax(worst, std::fabs(v.value() - prev));
                prev = v.value();
            }
        }
        return worst;
    };
    const double a = max_abs_move(calm);
    const double b = max_abs_move(jumpy);
    std::printf("  [jumps] largest single-event move: calm = %.2f  jumpy = %.2f\n", a, b);
    CHECK(a < 4.0);
    CHECK(b > 20.0);
}

}  // anonymous namespace

int main() {
    same_seed_gives_the_same_stream();
    different_seeds_diverge();
    a_zero_seed_is_not_degenerate();

    uniform01_stays_in_range();
    uniform01_is_flat();
    uniform_int_covers_its_range();
    uniform_int_reaches_both_endpoints();
    uniform_int_with_an_empty_range_returns_lo();
    uniform_int_handles_the_full_signed_range();

    bernoulli_respects_its_extremes();
    bernoulli_matches_its_probability();

    exponential_is_non_negative_and_finite();
    exponential_has_the_right_shape();

    normal_has_mean_zero_and_unit_variance();
    normal_has_normal_tails();

    value_starts_where_it_was_told_to();
    a_still_value_does_not_move();
    the_same_seed_reproduces_the_same_path();
    the_value_is_a_martingale();
    the_variance_grows_linearly_in_time();
    jumps_fatten_the_tails();

    return check_harness::report();
}
