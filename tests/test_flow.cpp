// Tests for the flow generator.
//
// The first two decide whether any of this is worth anything. If informed flow
// does not make money against the latent value and uninformed flow does not
// lose it, then "adverse selection" in this simulator is a label rather than a
// mechanism, and every P&L number in Phase 4 would be measuring nothing.

#include "mms/flow.hpp"

#include "check.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>

using namespace mms;

namespace {

constexpr std::uint64_t kSeed = 0xC0FFEEull;

ValueParams moving_value() {
    ValueParams p;
    p.volatility     = 0.5;
    p.jump_intensity = 0.05;
    p.jump_size      = 15.0;
    return p;
}

// Member order matters: `flow` is constructed last because it takes references
// to the other three. Members are initialised in *declaration* order, not in
// the order they appear in the initialiser list — get this wrong and the
// generator binds to objects that have not been constructed yet.
struct Market {
    OrderBook    book;
    Rng          rng;
    ValueProcess value;
    FlowGenerator flow;

    Market(std::uint64_t seed, const ValueParams& vp, const FlowParams& fp)
        : book(), rng(seed), value(vp, rng), flow(book, value, rng, fp) {}
};

void informed_win_and_noise_lose() {
    Market m(kSeed, moving_value(), FlowParams{});
    m.flow.run(20000);
    const FlowStats& s = m.flow.stats();
    const double per_lot_informed = s.informed_edge / static_cast<double>(s.volume);
    const double per_lot_noise    = s.noise_edge    / static_cast<double>(s.volume);
    std::printf("  [edge] informed %+.3f/lot   noise %+.3f/lot   trades=%zu\n",
                per_lot_informed, per_lot_noise, s.trades);
    CHECK(s.trades > 1000);
    CHECK(per_lot_informed > 1.0);    // observed +3.4 to +5.1 across seeds
    CHECK(per_lot_noise < -0.3);      // observed -0.73 to -0.92 — about half the spread
}

void a_static_value_leaves_the_informed_with_no_edge() {
    // The control. Freeze the latent value and the informed edge must collapse:
    // there is nothing to be informed *about*. The noise traders keep paying
    // the same half-spread, because their loss was never about information.
    //
    // Without this test, an informed edge produced by some artifact of the
    // mechanics — order type, timing, sizing — would look exactly like an
    // informed edge produced by information.
    ValueParams still;
    still.volatility     = 0.0;
    still.jump_intensity = 0.0;
    Market m(kSeed, still, FlowParams{});
    m.flow.run(20000);
    const FlowStats& s = m.flow.stats();
    const double per_lot_informed = s.informed_edge / static_cast<double>(s.volume);
    const double per_lot_noise    = s.noise_edge    / static_cast<double>(s.volume);
    std::printf("  [control] informed %+.3f/lot   noise %+.3f/lot\n",
                per_lot_informed, per_lot_noise);
    CHECK(std::fabs(per_lot_informed) < 0.5);   // observed +0.04 to +0.09
    CHECK(per_lot_noise < -0.3);
}

void the_mid_tracks_the_latent_value() {
    // Price discovery. The book has no access to the latent value; the mid only
    // moves because informed participants trade against stale quotes. This is
    // the test that caught the constant-cancel-rate bug — the mid was sitting
    // 140 ticks away from fair value and everything else still looked fine.
    Market m(kSeed, moving_value(), FlowParams{});
    m.flow.run(20000);
    const FlowStats& s = m.flow.stats();
    const double err = s.abs_mid_error / static_cast<double>(s.two_sided_samples);
    const double two_sided = 100.0 * static_cast<double>(s.two_sided_samples)
                                   / static_cast<double>(s.events);
    std::printf("  [discovery] mean |mid - value| = %.2f ticks, two-sided %.1f%% of the time\n",
                err, two_sided);
    CHECK(err < 12.0);          // observed 3.4 to 5.1
    CHECK(two_sided > 90.0);
}

void the_book_reaches_a_steady_state() {
    // Resting depth must not grow without bound. With a per-order cancel rate
    // it settles; with a per-market rate it does not.
    Market m(kSeed, moving_value(), FlowParams{});
    m.flow.run(2000);
    const std::size_t early = m.book.order_count();
    m.flow.run(60000);
    const std::size_t late = m.book.order_count();
    std::printf("  [steady state] resting orders: %zu after 2k events, %zu after 62k\n",
                early, late);
    CHECK(late < 400);
    CHECK(m.flow.resting_ids().size() < 1000);
}

void only_background_liquidity_ever_rests() {
    // Noise orders are Market and informed orders are IOC, so neither should
    // ever rest. Proven directly: withdraw every background order and the book
    // must be empty. Anything left behind was something that should not have
    // rested.
    Market m(kSeed, moving_value(), FlowParams{});
    m.flow.run(20000);
    CHECK(m.book.order_count() > 0);
    for (const OrderId id : m.flow.resting_ids()) m.book.cancel(id);
    CHECK(m.book.order_count() == 0);
}

void generated_flow_never_crosses_the_book() {
    // The Phase 1 invariant, now under realistic flow rather than random orders.
    Market m(kSeed, moving_value(), FlowParams{});
    bool ok = true;
    for (int i = 0; i < 20000; ++i) {
        m.flow.step();
        const TopOfBook t = m.book.top_of_book();
        if (t.is_two_sided() && t.bid_price >= t.ask_price) ok = false;
    }
    CHECK(ok);
}

void quantity_is_conserved_under_generated_flow() {
    // An inequality rather than an equality, unlike the Phase 1 version,
    // because this run has cancellations in it. The gap is exactly the
    // cancelled quantity. Making it an equality means tracking that too — a
    // real improvement, worth doing when there is a reason to care.
    Market m(kSeed, moving_value(), FlowParams{});
    m.flow.run(20000);
    const FlowStats& s = m.flow.stats();
    const Quantity resting = m.book.total_quantity(Side::Buy)
                           + m.book.total_quantity(Side::Sell);
    std::printf("  [conservation] submitted=%lld resting=%lld traded=%lld cancelled=%lld\n",
                static_cast<long long>(s.submitted),
                static_cast<long long>(resting),
                static_cast<long long>(s.volume),
                static_cast<long long>(s.submitted - resting - 2 * s.volume));
    CHECK(s.submitted >= resting + 2 * s.volume);
}

void the_same_seed_reproduces_the_same_market() {
    // Reproducibility, now across the whole stack. Exact float comparison for
    // the same reason as in Stage 4: this asks whether two runs executed the
    // identical sequence of operations, not whether two numbers are close.
    Market a(kSeed, moving_value(), FlowParams{});
    Market b(kSeed, moving_value(), FlowParams{});
    a.flow.run(20000);
    b.flow.run(20000);
    CHECK(a.flow.stats().trades == b.flow.stats().trades);
    CHECK(a.flow.stats().volume == b.flow.stats().volume);
    CHECK(a.flow.stats().informed_edge == b.flow.stats().informed_edge);
    CHECK(a.value.value() == b.value.value());
    CHECK(a.book.order_count() == b.book.order_count());
    CHECK(a.book.top_of_book().bid_price == b.book.top_of_book().bid_price);
}

void different_seeds_produce_different_markets() {
    Market a(1, moving_value(), FlowParams{});
    Market b(2, moving_value(), FlowParams{});
    a.flow.run(20000);
    b.flow.run(20000);
    CHECK(a.flow.stats().volume != b.flow.stats().volume);
}

void no_informed_flow_means_no_adverse_selection() {
    // Phase 3 will need to switch informed flow off to establish a baseline for
    // the market maker. Check now that the switch works.
    FlowParams fp;
    fp.rate_informed = 0.0;
    Market m(kSeed, moving_value(), fp);
    m.flow.run(20000);
    const FlowStats& s = m.flow.stats();
    CHECK(s.informed_orders == 0);
    CHECK(s.informed_edge == 0.0);
    CHECK(s.noise_orders > 0);
}

}  // anonymous namespace

int main() {
    informed_win_and_noise_lose();
    a_static_value_leaves_the_informed_with_no_edge();
    the_mid_tracks_the_latent_value();
    the_book_reaches_a_steady_state();
    only_background_liquidity_ever_rests();
    generated_flow_never_crosses_the_book();
    quantity_is_conserved_under_generated_flow();
    the_same_seed_reproduces_the_same_market();
    different_seeds_produce_different_markets();
    no_informed_flow_means_no_adverse_selection();

    return check_harness::report();
}
