// Tests for the market maker.
//
// Three of these together are the point of the whole project: the maker is
// profitable against uninformed flow, loses against informed flow, and becomes
// profitable again when the latent value is frozen so there is nothing to be
// informed about.

#include "mms/market_maker.hpp"

#include "check.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

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
ValueParams frozen_value() {
    ValueParams p;
    p.volatility     = 0.0;
    p.jump_intensity = 0.0;
    return p;
}

// Declaration order matters again: flow and maker both take references to
// members declared above them.
struct Sim {
    OrderBook     book;
    Rng           rng;
    ValueProcess  value;
    FlowGenerator flow;
    MarketMaker   maker;

    Sim(std::uint64_t seed, const ValueParams& vp, const FlowParams& fp,
        const MarketMakerParams& mp)
        : book(), rng(seed), value(vp, rng), flow(book, value, rng, fp), maker(book, mp) {}

    void run(std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) { flow.step(); maker.on_market_update(); }
    }
    double pnl() const { return maker.mark_to_market(value.value()); }
    double pnl_per_lot() const {
        const Quantity v = maker.stats().volume;
        return v ? pnl() / static_cast<double>(v) : 0.0;
    }
};

FlowParams with_informed(double rate) { FlowParams f; f.rate_informed = rate; return f; }

void the_maker_quotes_two_sided_and_gets_filled() {
    Sim s(kSeed, moving_value(), with_informed(2.0), MarketMakerParams{});
    s.run(20000);
    const auto& m = s.maker.stats();
    std::printf("  [quoting] requotes=%zu posted=%zu fills=%zu buys=%zu sells=%zu\n",
                m.requotes, m.quotes_posted, m.fills, m.buys, m.sells);
    CHECK(m.requotes > 100);
    CHECK(m.fills > 100);
    CHECK(m.buys > 0);
    CHECK(m.sells > 0);
    CHECK(s.maker.quoting());
    CHECK(s.maker.quoted_bid() < s.maker.quoted_ask());
}

void the_maker_never_crosses_the_book() {
    // Quoting symmetrically around the market mid can never be marketable: the
    // mid is strictly between the touches, so bid = mid - h <= best bid and
    // ask = mid + h >= best ask for any h >= 0. If this fires, the fair value
    // estimate is picking up the maker's own quotes.
    Sim s(kSeed, moving_value(), with_informed(2.0), MarketMakerParams{});
    bool ok = true;
    for (int i = 0; i < 20000; ++i) {
        s.flow.step();
        s.maker.on_market_update();
        const TopOfBook t = s.book.top_of_book();
        if (t.is_two_sided() && t.bid_price >= t.ask_price) ok = false;
    }
    CHECK(ok);
}

void with_no_informed_flow_the_maker_is_profitable() {
    // The test that decides whether the quoting agent works at all. No informed
    // flow means no adverse selection, so spread capture is all that is left.
    Sim s(kSeed, moving_value(), with_informed(0.0), MarketMakerParams{});
    s.run(60000);
    std::printf("  [baseline] no informed flow: %+.4f/lot over %lld lots\n",
                s.pnl_per_lot(), static_cast<long long>(s.maker.stats().volume));
    CHECK(s.maker.stats().volume > 2000);
    CHECK(s.pnl_per_lot() > 0.15);   // observed +0.39 to +1.15 across seeds
}

void informed_flow_costs_the_maker_money() {
    Sim a(kSeed, moving_value(), with_informed(0.0), MarketMakerParams{});
    Sim b(kSeed, moving_value(), with_informed(4.0), MarketMakerParams{});
    a.run(60000);
    b.run(60000);
    std::printf("  [adverse selection] informed 0: %+.4f/lot   informed 4: %+.4f/lot\n",
                a.pnl_per_lot(), b.pnl_per_lot());
    CHECK(b.pnl_per_lot() < a.pnl_per_lot());
    CHECK(b.pnl_per_lot() < -1.0);   // observed -2.5 to -3.5 across seeds
}

void a_frozen_value_leaves_the_maker_profitable_even_against_informed_flow() {
    // The control, and the strongest result in the project. Same participants,
    // same order types, same arrival rates as the test above — the only change
    // is that there is nothing to be informed *about*. If the maker's losses
    // came from the mechanics of how informed traders trade rather than from
    // what they know, this would still be negative.
    Sim s(kSeed, frozen_value(), with_informed(4.0), MarketMakerParams{});
    s.run(60000);
    std::printf("  [control] frozen value, informed 4: %+.4f/lot\n", s.pnl_per_lot());
    CHECK(s.pnl_per_lot() > 0.15);   // observed +0.49 to +0.55, very tight
}

void skew_keeps_inventory_near_flat() {
    MarketMakerParams none; none.skew_per_lot = 0.0;  none.max_position = 1000000;
    MarketMakerParams some; some.skew_per_lot = 0.03; some.max_position = 1000000;

    Sim a(kSeed, moving_value(), with_informed(4.0), none);
    Sim b(kSeed, moving_value(), with_informed(4.0), some);
    a.run(40000);
    b.run(40000);

    const auto mean_abs = [](const MarketMakerStats& m) {
        return m.abs_position_sum / static_cast<double>(m.position_samples);
    };
    std::printf("  [skew] mean |position|: no skew %.1f   skew 0.03 %.1f\n",
                mean_abs(a.maker.stats()), mean_abs(b.maker.stats()));
    CHECK(mean_abs(b.maker.stats()) < 0.25 * mean_abs(a.maker.stats()));
}

void the_position_limit_is_never_breached() {
    // The quote is sized against the remaining room, not merely suppressed once
    // the limit is reached — otherwise a resting quote can carry the position
    // straight through the cap.
    MarketMakerParams mp;
    mp.max_position = 20;
    mp.skew_per_lot = 0.0;   // remove the soft control so the hard cap is what binds
    Sim s(kSeed, moving_value(), with_informed(4.0), mp);

    Quantity worst = 0;
    for (int i = 0; i < 40000; ++i) {
        s.flow.step();
        s.maker.on_market_update();
        worst = std::max(worst, static_cast<Quantity>(std::llabs(s.maker.stats().position)));
    }
    std::printf("  [limit] max |position| reached: %lld of a %lld cap\n",
                static_cast<long long>(worst), static_cast<long long>(mp.max_position));
    CHECK(worst <= mp.max_position);
    CHECK(worst > 5);   // the cap must actually bind, or the test proves nothing
}

void a_wider_quote_trades_less_and_earns_more_per_lot() {
    MarketMakerParams tight; tight.half_spread_x2 = 1;
    MarketMakerParams wide;  wide.half_spread_x2  = 3;
    Sim a(kSeed, moving_value(), with_informed(0.0), tight);
    Sim b(kSeed, moving_value(), with_informed(0.0), wide);
    a.run(60000);
    b.run(60000);
    std::printf("  [spread] half_spread_x2=1: %lld lots at %+.4f/lot   =3: %lld lots at %+.4f/lot\n",
                static_cast<long long>(a.maker.stats().volume), a.pnl_per_lot(),
                static_cast<long long>(b.maker.stats().volume), b.pnl_per_lot());
    CHECK(b.maker.stats().volume < a.maker.stats().volume);
    CHECK(b.pnl_per_lot() > a.pnl_per_lot());
}

void cash_and_position_agree_with_the_fills() {
    Sim s(kSeed, moving_value(), with_informed(2.0), MarketMakerParams{});
    s.run(20000);
    const auto& m = s.maker.stats();
    CHECK(m.fills == m.buys + m.sells);
    CHECK(m.volume > 0);
    // Long inventory was paid for, so cash must be negative; short is the reverse.
    if (m.position > 0) CHECK(m.cash < 0.0);
    if (m.position < 0) CHECK(m.cash > 0.0);
}

void the_same_seed_reproduces_the_same_maker() {
    Sim a(kSeed, moving_value(), with_informed(3.0), MarketMakerParams{});
    Sim b(kSeed, moving_value(), with_informed(3.0), MarketMakerParams{});
    a.run(20000);
    b.run(20000);
    CHECK(a.maker.stats().fills    == b.maker.stats().fills);
    CHECK(a.maker.stats().position == b.maker.stats().position);
    CHECK(a.maker.stats().cash     == b.maker.stats().cash);
    CHECK(a.pnl() == b.pnl());
}

void the_maker_ids_cannot_collide_with_participant_ids() {
    Sim s(kSeed, moving_value(), with_informed(3.0), MarketMakerParams{});
    s.run(20000);
    // Participants allocate upward from 1; the maker from 2^63. Colliding ids
    // would be silently rejected by the book, so the failure would look like a
    // maker that mysteriously stops quoting.
    bool ok = true;
    for (const OrderId id : s.flow.resting_ids()) if (id >= kMarketMakerIdBase) ok = false;
    CHECK(ok);
    CHECK(s.flow.stats().orders_posted < kMarketMakerIdBase);
}

}  // anonymous namespace

int main() {
    the_maker_quotes_two_sided_and_gets_filled();
    the_maker_never_crosses_the_book();
    with_no_informed_flow_the_maker_is_profitable();
    informed_flow_costs_the_maker_money();
    a_frozen_value_leaves_the_maker_profitable_even_against_informed_flow();
    skew_keeps_inventory_near_flat();
    the_position_limit_is_never_breached();
    a_wider_quote_trades_less_and_earns_more_per_lot();
    cash_and_position_agree_with_the_fills();
    the_same_seed_reproduces_the_same_maker();
    the_maker_ids_cannot_collide_with_participant_ids();

    return check_harness::report();
}
