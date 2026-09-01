#pragma once

#include "mms/random.hpp"
#include "mms/types.hpp"

#include <cmath>

namespace mms {

struct ValueParams {
    double initial          = 10000.0;  // ticks
    double volatility       = 0.30;     // s.d. of the increment over one unit of time, in ticks
    double jump_intensity   = 0.002;    // expected jumps per unit of time
    double jump_size        = 15.0;     // s.d. of that repricing, in ticks
};

// The latent value of the asset: what it is actually worth, at each moment.
//
// Nobody in the simulation observes this directly. Informed participants see it
// with noise; the market maker never sees it at all. It exists so that adverse
// selection can be *measured* rather than asserted.
//
// Two components, because one is not enough:
//
//   diffusion  small, constant drift of opinion. On its own it gives the
//              informed a tiny steady edge and the market maker a smooth,
//              boring loss — which a market maker solves by widening.
//   jumps      rare, large repricings. This is what actually hurts: quotes that
//              were fair a microsecond ago are stale on one side, and the
//              informed take that side before they can be pulled. Inventory
//              risk only looks like the real thing once the value can gap.
class ValueProcess {
public:
    ValueProcess(const ValueParams& params, Rng& rng) noexcept
        : params_(params), rng_(&rng), value_(params.initial) {}

    // Advance the value by an elapsed interval.
    //
    // sqrt(dt), not dt. The increments of a random walk are independent, so
    // *variances* add: over an interval dt the variance is vol^2 * dt and the
    // standard deviation is vol * sqrt(dt). This is the most common error in a
    // simulation driven by an irregular clock, and it is invisible — the paths
    // still look plausible, they just have the wrong volatility term structure.
    // It is the same reason annualised vol is daily vol times sqrt(252).
    //
    // Jumps arrive as a Poisson process with intensity `jump_intensity`, so the
    // probability of at least one arrival in dt is 1 - exp(-intensity*dt).
    // expm1(x) computes exp(x) - 1 accurately for small x, where computing it
    // directly loses almost every significant digit to cancellation: at
    // x = 1e-8, exp(x) is 1.00000001 and subtracting 1 from a 16-digit double
    // leaves you with about eight. expm1 and log1p exist for exactly this.
    void step(double dt) noexcept {
        value_ += params_.volatility * std::sqrt(dt) * rng_->normal();
        if (rng_->bernoulli(-std::expm1(-params_.jump_intensity * dt))) {
            value_ += params_.jump_size * rng_->normal();
        }
    }

    void step() noexcept { step(1.0); }

    double value() const noexcept { return value_; }

    // The latent value rounded to the tick grid, for comparison against book
    // prices. This is the one place a double meets a Price, and it is
    // deliberate.
    //
    // Prices are integers throughout the engine because price *equality*
    // decides whether two orders match, and because rounding error accumulates
    // into P&L (D1). Neither applies here: the latent value never rests on the
    // book, is never compared for equality, and never enters a money figure. It
    // is a modelling parameter, rounded at the point of use.
    //
    // Keeping it a double internally also avoids a modelling artifact — if it
    // were rounded to a tick every step, increments below half a tick would
    // round to zero and the process would sit still.
    //
    // llround, not a cast: casting a float to an integer truncates *toward
    // zero*, a downward bias for positive values and an upward one for
    // negative. Half a tick, applied asymmetrically across the sign, in a
    // simulator whose whole output is measured in fractions of a tick.
    Price value_ticks() const noexcept {
        return static_cast<Price>(std::llround(value_));
    }

private:
    ValueParams params_;
    // A non-owning pointer to a generator owned elsewhere. Not a copy, because
    // every consumer must draw from the *same* stream — two objects seeded
    // alike would draw identical sequences, correlated invisibly. Not a
    // reference, because a reference member implicitly deletes copy-assignment
    // (references cannot be rebound), which makes the class impossible to store
    // in a vector that reallocates and awkward everywhere generic.
    Rng* rng_;
    double value_;
};

}  // namespace mms
