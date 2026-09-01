#include "mms/flow.hpp"

#include <cmath>
#include <cstdint>

namespace mms {

FlowGenerator::FlowGenerator(OrderBook& book, ValueProcess& value, Rng& rng,
                             const FlowParams& params)
    : book_(&book), value_(&value), rng_(&rng), params_(params),
      last_price_(params.initial_price) {
    book_->add_trade_listener([this](const Trade& t) { on_trade(t); });
}

void FlowGenerator::run(std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) step();
}

void FlowGenerator::step() {
    // Cancellation is a property of each *resting order*, not of the market.
    //
    // The first version of this had a constant cancel rate, and the book grew
    // without bound: 11,766 resting orders after 200,000 events, a wall a
    // hundred ticks deep, informed traders unable to move price through it, and
    // a mid that ended up 140 ticks from fair value. Arrivals at a fixed rate
    // against departures at a fixed rate have no equilibrium — the difference
    // accumulates forever.
    //
    // With intensity delta*N the system has a fixed point, where posting equals
    // delta*N + fills, and it is self-correcting: too many orders means more
    // cancellations. Same argument as birth-death processes in queueing theory.
    // Steady state is now ~35 resting orders. Closes D3.
    const double rate_cancel =
        params_.cancel_rate * static_cast<double>(resting_.size());

    // One clock, not four.
    //
    // Independent Poisson processes with intensities l_i superpose into a
    // single Poisson process with intensity L = sum(l_i), in which each event
    // is of type i with probability l_i / L, independently of the timing. So
    // the whole arrival structure is one exponential draw for *when* and one
    // categorical draw for *what*. Four separate clocks would give the identical
    // distribution and cost four times as much.
    const double total = params_.rate_liquidity + rate_cancel
                       + params_.rate_noise + params_.rate_informed;

    const double dt = rng_->exponential(total);
    clock_ += dt;

    // The latent value moves in continuous time, so it advances by the elapsed
    // interval — not one step per event. Under a Poisson clock those are very
    // different: dt is exponentially distributed, so a fixed step would
    // understate the volatility of long gaps and overstate it for short ones.
    value_->step(dt);
    ++stats_.events;

    double u = rng_->uniform01() * total;
    if      ((u -= params_.rate_liquidity) < 0.0) post_liquidity();
    else if ((u -= rate_cancel)            < 0.0) cancel_liquidity();
    else if ((u -= params_.rate_noise)     < 0.0) submit_noise();
    else                                          submit_informed();

    sample_mid();
}

// The book breaks price ties by queue position rather than by reading this
// field, so the timestamp is informational. An event counter is monotonic by
// construction; converting the floating-point clock would mean choosing a scale
// factor and would create ties whenever two events landed in the same tick of it.
Timestamp FlowGenerator::timestamp() const noexcept {
    return static_cast<Timestamp>(stats_.events);
}

// The price a background provider quotes around.
//
// A buy is posted strictly below the best ask, a sell strictly above the best
// bid — relative to the *opposite* touch. That is the standard zero-intelligence
// formulation (Farmer, Patelli & Zovko), and it buys a property worth having:
// such an order cannot be marketable, so background liquidity never crosses the
// spread. Every aggressive order in this simulation comes from a participant
// that chose to be aggressive.
//
// With that side empty there is no touch to reference, so the last traded price
// stands in.
Price FlowGenerator::reference_for(Side side) const {
    const Price opposite = (side == Side::Buy) ? book_->best_ask() : book_->best_bid();
    return opposite == kInvalidPrice ? last_price_ : opposite;
}

void FlowGenerator::post_liquidity() {
    const Side  side   = rng_->bernoulli(0.5) ? Side::Buy : Side::Sell;
    const Price offset = 1 + static_cast<Price>(rng_->uniform_int(0, params_.depth_ticks));
    const Price ref    = reference_for(side);

    Order o;
    o.id        = next_id();
    o.side      = side;
    o.type      = OrderType::Limit;
    o.price     = (side == Side::Buy) ? ref - offset : ref + offset;
    o.quantity  = rng_->uniform_int(1, params_.max_size);
    o.remaining = o.quantity;
    o.timestamp = timestamp();
    o.owner     = kLiquidity;

    stats_.submitted += o.quantity;
    book_->submit(o);
    resting_.push_back(o.id);
    ++stats_.orders_posted;
}

void FlowGenerator::cancel_liquidity() {
    // `resting_` holds background orders posted and not yet cancelled — but an
    // order can also leave by being *filled*, and nothing tells the generator
    // when that happens. So the list drifts above the true live count and is a
    // list of *candidates*, not a truth source.
    //
    // Handled lazily: pick an id, attempt the cancel, and if the book says it
    // is unknown, discard it and draw again. Every cancel event therefore
    // cancels a live order or exhausts the list, and dead ids get pruned as
    // they are met. The exact fix — an unordered_map<OrderId, size_t> beside
    // the vector so the trade callback can remove filled ids in O(1) — is
    // deferred: all it changes is that delta*N slightly overstates the true
    // cancel intensity, which shifts the steady-state book size by a constant
    // that is already a free parameter.
    while (!resting_.empty()) {
        // Swap-and-pop: O(1), where erase(begin() + i) is O(n). It destroys
        // ordering, which is free here because the element was chosen uniformly
        // at random and the order carries no meaning.
        const std::size_t i = static_cast<std::size_t>(
            rng_->uniform_int(0, static_cast<std::int64_t>(resting_.size()) - 1));
        const OrderId id = resting_[i];
        resting_[i] = resting_.back();
        resting_.pop_back();

        if (book_->cancel(id)) { ++stats_.orders_cancelled; return; }
    }
}

void FlowGenerator::submit_noise() {
    // Uninformed flow: buys and sells with equal probability, ignoring the
    // latent value entirely. This is the market maker's revenue — the
    // counterparty who is not systematically right.
    Order o;
    o.id        = next_id();
    o.side      = rng_->bernoulli(0.5) ? Side::Buy : Side::Sell;
    o.type      = OrderType::Market;
    o.price     = 0;                       // ignored for Market
    o.quantity  = rng_->uniform_int(1, params_.max_size);
    o.remaining = o.quantity;
    o.timestamp = timestamp();
    o.owner     = kNoise;

    stats_.submitted += o.quantity;
    book_->submit(o);
    ++stats_.noise_orders;
}

void FlowGenerator::submit_informed() {
    // Informed flow: observes the latent value with noise and takes whichever
    // side of the book is on the wrong side of it. This is the market maker's
    // cost, and the thing the whole project exists to measure.
    const double perceived = value_->value() + params_.informed_noise * rng_->normal();
    const Price  fair = static_cast<Price>(std::llround(perceived));
    const Price  edge = params_.informed_edge;

    const Price ask = book_->best_ask();
    const Price bid = book_->best_bid();

    Side side;
    if (ask != kInvalidPrice && ask <= fair - edge) {
        side = Side::Buy;            // the offer is cheap
    } else if (bid != kInvalidPrice && bid >= fair + edge) {
        side = Side::Sell;           // the bid is rich
    } else {
        // No edge worth taking. An informed trader who acts on an infinitesimal
        // mispricing is a noise trader with extra steps, and would make the
        // market maker unprofitable at any spread. Real informed traders have a
        // hurdle — costs, capital, the risk the signal is wrong. The threshold
        // is what makes this model produce a market a market maker can survive
        // in. Most informed arrivals end here.
        ++stats_.informed_passed;
        return;
    }

    Order o;
    o.id        = next_id();
    o.side      = side;
    // IOC, not Market. A market order sweeps the book at any price; an informed
    // participant takes everything mispriced and stops. Limiting at the
    // perceived fair value is what makes them informed rather than impatient.
    o.type      = OrderType::IOC;
    o.price     = fair;
    o.quantity  = rng_->uniform_int(1, params_.informed_max_size);
    o.remaining = o.quantity;
    o.timestamp = timestamp();
    o.owner     = kInformed;

    stats_.submitted += o.quantity;
    book_->submit(o);
    ++stats_.informed_orders;
}

void FlowGenerator::on_trade(const Trade& t) {
    ++stats_.trades;
    stats_.volume += t.quantity;
    last_price_ = t.price;

    // Realised edge, measured against the latent value at the instant of the
    // trade. A buyer gains when the price paid is below fair value; a seller
    // gains when the price received is above it. Sign by the aggressor's side,
    // weight by size.
    //
    // Note this is the *instantaneous* mispricing, not a markout. True adverse
    // selection is measured against the value some horizon *after* the trade —
    // that is a Phase 4 refinement, and the distinction matters.
    const double fair = value_->value();
    const double per_lot = (t.aggressor_side == Side::Buy)
        ? fair - static_cast<double>(t.price)
        : static_cast<double>(t.price) - fair;
    const double edge = per_lot * static_cast<double>(t.quantity);

    if      (t.aggressor_owner == kNoise)    stats_.noise_edge    += edge;
    else if (t.aggressor_owner == kInformed) stats_.informed_edge += edge;
}

void FlowGenerator::sample_mid() {
    const TopOfBook top = book_->top_of_book();
    if (!top.is_two_sided()) return;
    ++stats_.two_sided_samples;
    stats_.abs_mid_error +=
        std::fabs(static_cast<double>(top.mid_x2()) / 2.0 - value_->value());
}

}  // namespace mms
