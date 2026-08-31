#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

namespace mms {

// A deterministic pseudo-random source.
//
// Why not <random>'s distributions? The *engines* in <random> are specified
// bit-for-bit by the standard; the *distributions* are not. libstdc++ and
// libc++ consume a different number of engine outputs per variate, so the same
// seed produces different simulations on different machines. That would
// destroy the one property this project rests on — a run reproduces exactly.
// The distributions here are therefore ours, and specified by their code.
class Rng {
public:
    explicit Rng(std::uint64_t seed) noexcept : state_(seed) {}

    // splitmix64.
    //
    // `state_ += <odd constant>` is a Weyl sequence: because the constant is
    // odd it is coprime with 2^64, so repeated addition visits all 2^64 states
    // before repeating. Full period from *every* seed — there are no bad seeds.
    // (mt19937 holds 2.5 KB of state and takes thousands of draws to recover
    // from a seed that is mostly zero bits.)
    //
    // The two multiply-xorshift rounds are the mixing function. A counter on
    // its own is terrible randomness — it counts. These rounds scramble it so
    // that flipping one input bit flips about half the output bits. The
    // multipliers are odd, so the multiplication is invertible mod 2^64 and no
    // information is lost; the xorshifts push the high bits, which
    // multiplication contaminates well, back down into the low bits, which it
    // contaminates poorly.
    std::uint64_t next_u64() noexcept {
        std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    // Uniform on [0, 1).
    //
    // A double has a 53-bit significand, so more than 53 random bits is wasted:
    // the extra bits round away and introduce a small bias. Take exactly 53
    // (>> 11 drops the low eleven of sixty-four) and scale by 2^-53.
    //
    // 0x1.0p-53 is a hex float literal — significand 1.0, binary exponent -53.
    // Exact, and resolved at compile time.
    //
    // The *top* bits, not the bottom: in many generators the low bits have much
    // shorter periods. splitmix64 mixes well enough that it does not matter
    // here, but the code should not depend on that.
    double uniform01() noexcept {
        return static_cast<double>(next_u64() >> 11) * 0x1.0p-53;
    }

    // Note the interval is half-open, [0, 1): bernoulli(0.0) is therefore never
    // true and bernoulli(1.0) is always true, which is what you want and is
    // not free — it depends on that half-openness.
    bool bernoulli(double p) noexcept { return uniform01() < p; }

    // Uniform integer on [lo, hi], inclusive.
    std::int64_t uniform_int(std::int64_t lo, std::int64_t hi) noexcept {
        if (hi <= lo) return lo;

        // Computed in unsigned. hi - lo overflows a signed 64-bit integer for
        // wide ranges, and signed overflow is *undefined behaviour*, not
        // wraparound — the compiler may assume it never happens and delete
        // whatever follows. Unsigned wraparound is defined.
        const std::uint64_t span =
            static_cast<std::uint64_t>(hi) - static_cast<std::uint64_t>(lo);

        std::uint64_t offset;
        if (span == std::numeric_limits<std::uint64_t>::max()) {
            // The caller asked for all 2^64 values. That count does not fit in
            // a uint64_t: span + 1 wraps to zero and every modulo below becomes
            // a division by zero. It also needs no rejection — a raw draw is
            // already uniform over the whole range.
            offset = next_u64();
        } else {
            const std::uint64_t range = span + 1ull;

            // Rejection sampling, to remove modulo bias. 2^64 is not in general
            // a multiple of `range`, so `next_u64() % range` returns the low
            // residues slightly more often. Write kMax = q*range + r; then
            // limit = kMax - r = q*range, so the accepted set [0, limit) is an
            // exact multiple of range and every residue is equally likely.
            constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
            const std::uint64_t limit = kMax - (kMax % range);
            do { offset = next_u64(); } while (offset >= limit);
            offset %= range;
        }

        // Added in unsigned (wraparound is defined) and converted once at the
        // end. That conversion is implementation-defined before C++20 and
        // defined as two's complement from C++20 on. Implementation-defined is
        // not undefined: the compiler must pick a behaviour and document it,
        // and every compiler picks the wrap. Signed overflow would not be a
        // justifiable dependency; this is.
        return static_cast<std::int64_t>(static_cast<std::uint64_t>(lo) + offset);
    }

    // Exponential with the given rate (mean 1/rate).
    //
    // Inverse transform sampling: if U is uniform on (0,1) and F is a
    // continuous, strictly increasing CDF, then F^-1(U) has distribution F —
    // because P(F^-1(U) <= x) = P(U <= F(x)) = F(x). For the exponential
    // F(x) = 1 - e^(-rate*x), so F^-1(u) = -ln(1-u)/rate.
    //
    // 1 - uniform01() rather than uniform01(), because uniform01() can return
    // exactly 0 and log(0) is -infinity. Flipping gives (0, 1].
    //
    // This is how Poisson inter-arrival times get drawn in Stage 5.
    double exponential(double rate) noexcept {
        return -std::log(1.0 - uniform01()) / rate;
    }

    // Standard normal, Box-Muller: read two uniforms as a radius and an angle,
    // and the Cartesian pair r*cos(t), r*sin(t) is two independent standard
    // normals.
    //
    // This throws the sine away, on purpose. Caching it saves a sqrt and a log
    // every second call, but then the generator's behaviour depends on how many
    // times it has been called and its state is no longer one integer. As
    // written, every call consumes exactly two draws, the state is a single
    // uint64_t, and a simulation can be snapshotted, resumed or forked by
    // copying one number. Two Rng objects with equal state are interchangeable.
    // One transcendental pair is nothing next to the order book work each
    // event triggers.
    double normal() noexcept {
        constexpr double kTwoPi = 6.283185307179586476925286766559;
        const double u1 = 1.0 - uniform01();   // (0, 1], keeps the log finite
        const double u2 = uniform01();
        return std::sqrt(-2.0 * std::log(u1)) * std::cos(kTwoPi * u2);
    }

    std::uint64_t state() const noexcept { return state_; }

private:
    std::uint64_t state_;
};

}  // namespace mms