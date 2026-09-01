// Tests for the P&L attribution.
//
// The first two are the point of the entire project: informed flow must degrade
// the adverse-selection term and leave spread capture alone. If it contaminates
// spread capture, the two are mixed and every number downstream means nothing.

#include "mms/attribution.hpp"

#include "check.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace mms;

namespace {

constexpr std::uint64_t kSeed = 0xC0FFEEull;
const std::vector<double> kHorizons = {0.5, 5.0, 50.0};

ValueParams moving_value() {
    ValueParams p; p.volatility = 0.5; p.jump_intensity = 0.05; p.jump_size = 15.0; return p;
}
ValueParams frozen_value() {
    ValueParams p; p.volatility = 0.0; p.jump_intensity = 0.0; return p;
}
FlowParams with_informed(double r) { FlowParams f; f.rate_informed = r; return f; }

struct Sim {
    OrderBook      book;
    Rng            rng;
    ValueProcess   value;
    FlowGenerator  flow;
    MarketMaker    maker;
    PnlAttribution attr;

    Sim(std::uint64_t seed, const ValueParams& vp, const FlowParams& fp)
        : book(), rng(seed), value(vp, rng), flow(book, value, rng, fp),
          maker(book, MarketMakerParams{}), attr(book, value, flow, kHorizons) {}

    void run(std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            flow.step();
            maker.on_market_update();
            attr.settle();
        }
    }
    const Markout& shortest() const { return attr.at(0); }
    const Markout& medium()   const { return attr.at(1); }
    const Markout& longest()  const { return attr.at(2); }
};

void spread_capture_survives_informed_flow() {
    // The test the whole stage exists for. Adding informed participants must not
    // move spread capture — that term is what the maker earns for providing
    // immediacy, and immediacy is worth the same whoever is buying it.
    Sim a(kSeed, moving_value(), with_informed(0.0));
    Sim b(kSeed, moving_value(), with_informed(4.0));
    a.run(40000);
    b.run(40000);
    const double sa = a.medium().spread_per_lot();
    const double sb = b.medium().spread_per_lot();
    std::printf("  [spread] informed 0: %+.4f/lot   informed 4: %+.4f/lot\n", sa, sb);
    CHECK(sa > 0.5);
    CHECK(sb > 0.5);
    CHECK(std::fabs(sb - sa) < 0.5);   // observed +0.98 -> +1.11
}

void informed_flow_lands_in_adverse_selection() {
    Sim a(kSeed, moving_value(), with_informed(0.0));
    Sim b(kSeed, moving_value(), with_informed(4.0));
    a.run(40000);
    b.run(40000);
    const double aa = a.medium().adverse_per_lot();
    const double ab = b.medium().adverse_per_lot();
    std::printf("  [adverse] informed 0: %+.4f/lot   informed 4: %+.4f/lot\n", aa, ab);
    CHECK(aa < 0.0);
    CHECK(ab < aa - 1.5);   // observed -0.29 -> -3.54
}

void adverse_selection_deepens_with_horizon() {
    // The adverse-selection curve: how much of the move happens immediately
    // after your fill versus later. A flat curve would mean the measure is not
    // actually a markout.
    Sim s(kSeed, moving_value(), with_informed(4.0));
    s.run(40000);
    std::printf("  [curve] h=0.5 %+.4f   h=5.0 %+.4f   h=50 %+.4f  (per lot)\n",
                s.shortest().adverse_per_lot(), s.medium().adverse_per_lot(),
                s.longest().adverse_per_lot());
    CHECK(s.medium().adverse_per_lot()  < s.shortest().adverse_per_lot());
    CHECK(s.longest().adverse_per_lot() < s.shortest().adverse_per_lot());
}

void a_frozen_value_leaves_only_price_impact() {
    // Freeze the value — nothing to be informed about — and adverse selection is
    // still about half a tick. That is *price impact*, not information: the
    // aggressive order that hits you consumes liquidity and moves the mid,
    // regardless of whether anyone knew anything.
    //
    // The measure therefore has a floor, and the informational component is what
    // lies below it. Quoting markout numbers without knowing where your impact
    // floor sits is reporting information you do not have.
    Sim s(kSeed, frozen_value(), with_informed(4.0));
    s.run(40000);
    std::printf("  [impact floor] frozen value: spread %+.4f   adverse %+.4f (h=5)\n",
                s.medium().spread_per_lot(), s.medium().adverse_per_lot());
    CHECK(s.medium().spread_per_lot() > 0.5);
    CHECK(s.medium().adverse_per_lot() < 0.0);      // impact is real
    CHECK(s.medium().adverse_per_lot() > -1.0);     // but shallow — observed -0.51
    CHECK(s.medium().total_per_lot() > 0.0);        // and the maker still profits
}

void the_mid_based_markout_recovers_the_true_cost() {
    // The validation, and the thing only a simulator can produce: the observable
    // measure agrees with the unobservable truth. On real data there is no latent
    // value to check against.
    Sim s(kSeed, moving_value(), with_informed(4.0));
    s.run(40000);
    const Markout& m = s.longest();
    const double truth = m.true_edge_per_lot() + m.value_drift_per_lot();
    std::printf("  [validation] mid-based %+.4f/lot vs latent-value truth %+.4f/lot\n",
                m.total_per_lot(), truth);
    CHECK(std::fabs(m.total_per_lot() - truth) < 1.0);   // observed within 0.35
}

void the_attribution_agrees_with_the_makers_own_pnl() {
    // Two completely different code paths: the maker tracks cash and inventory
    // and marks at the latent value; the attribution sums markouts over
    // individual fills. Agreeing is a much stronger statement than either alone.
    Sim s(kSeed, moving_value(), with_informed(4.0));
    s.run(40000);
    const double maker_per_lot = s.maker.mark_to_market(s.value.value())
                               / static_cast<double>(s.maker.stats().volume);
    std::printf("  [cross-check] attribution %+.4f/lot   maker's own P&L %+.4f/lot\n",
                s.longest().total_per_lot(), maker_per_lot);
    CHECK(std::fabs(s.longest().total_per_lot() - maker_per_lot) < 1.0);
}

void almost_every_fill_settles() {
    Sim s(kSeed, moving_value(), with_informed(4.0));
    s.run(40000);
    std::printf("  [coverage] settled %zu of %zu maker fills, %zu still open\n",
                s.longest().fills, s.maker.stats().fills, s.attr.unsettled());
    CHECK(s.longest().fills > 0);
    // The longest horizon necessarily loses the tail of the run — a fill in the
    // last 50 time units has nowhere to mark out to. Expect ~95%, require 90%.
    CHECK(s.longest().fills * 10 > s.maker.stats().fills * 9);
    CHECK(s.shortest().fills * 100 > s.maker.stats().fills * 99);
}

void no_market_maker_means_nothing_to_attribute() {
    // The owner filter must pick up only the maker's fills. Run the market with
    // no maker at all: thousands of trades happen and none are attributed.
    OrderBook book;
    Rng rng(kSeed);
    ValueProcess value(moving_value(), rng);
    FlowGenerator flow(book, value, rng, with_informed(4.0));
    PnlAttribution attr(book, value, flow, kHorizons);

    for (int i = 0; i < 20000; ++i) { flow.step(); attr.settle(); }

    CHECK(flow.stats().trades > 1000);
    for (const auto& m : attr.markouts()) {
        CHECK(m.fills == 0);
        CHECK(m.volume == 0);
        CHECK(m.spread_capture == 0.0);
        CHECK(m.adverse_selection == 0.0);
    }
}

void the_same_seed_reproduces_the_same_attribution() {
    Sim a(kSeed, moving_value(), with_informed(3.0));
    Sim b(kSeed, moving_value(), with_informed(3.0));
    a.run(20000);
    b.run(20000);
    for (std::size_t i = 0; i < kHorizons.size(); ++i) {
        CHECK(a.attr.at(i).spread_capture    == b.attr.at(i).spread_capture);
        CHECK(a.attr.at(i).adverse_selection == b.attr.at(i).adverse_selection);
        CHECK(a.attr.at(i).volume            == b.attr.at(i).volume);
    }
}

}  // anonymous namespace

int main() {
    spread_capture_survives_informed_flow();
    informed_flow_lands_in_adverse_selection();
    adverse_selection_deepens_with_horizon();
    a_frozen_value_leaves_only_price_impact();
    the_mid_based_markout_recovers_the_true_cost();
    the_attribution_agrees_with_the_makers_own_pnl();
    almost_every_fill_settles();
    no_market_maker_means_nothing_to_attribute();
    the_same_seed_reproduces_the_same_attribution();

    return check_harness::report();
}
