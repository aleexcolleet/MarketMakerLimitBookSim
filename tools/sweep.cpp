// Sweeps informed-flow intensity and prints the P&L decomposition as CSV.
//
// This is the data behind docs/attribution.png. It is a tool, not a test: it
// prints numbers rather than asserting them, and lives outside tests/ for that
// reason.
//
//   ./build/mms_sweep > docs/attribution.csv

#include "mms/attribution.hpp"

#include <cstdio>
#include <cstdint>
#include <vector>

using namespace mms;

namespace {

const std::vector<double> kHorizons = {0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 25.0, 50.0};
constexpr std::size_t kEvents = 120000;
constexpr int         kSeeds  = 8;

ValueParams moving_value() {
    ValueParams p;
    p.volatility     = 0.5;
    p.jump_intensity = 0.05;
    p.jump_size      = 15.0;
    return p;
}

struct Row {
    std::vector<double> spread, adverse, total;
    double volume = 0.0;
};

// Averaged over seeds, because a single path is noisy and the shape is the point.
Row measure(double informed, bool frozen) {
    Row r;
    r.spread.assign(kHorizons.size(), 0.0);
    r.adverse.assign(kHorizons.size(), 0.0);
    r.total.assign(kHorizons.size(), 0.0);

    for (int k = 0; k < kSeeds; ++k) {
        ValueParams vp = moving_value();
        if (frozen) { vp.volatility = 0.0; vp.jump_intensity = 0.0; }

        FlowParams fp;
        fp.rate_informed = informed;

        OrderBook      book;
        Rng            rng(0xC0FFEEull + static_cast<std::uint64_t>(k) * 7919ull);
        ValueProcess   value(vp, rng);
        FlowGenerator  flow(book, value, rng, fp);
        MarketMaker    maker(book, MarketMakerParams{});
        PnlAttribution attr(book, value, flow, kHorizons);

        for (std::size_t i = 0; i < kEvents; ++i) {
            flow.step();
            maker.on_market_update();
            attr.settle();
        }
        for (std::size_t h = 0; h < kHorizons.size(); ++h) {
            const Markout& m = attr.at(h);
            r.spread[h]  += m.spread_per_lot();
            r.adverse[h] += m.adverse_per_lot();
            r.total[h]   += m.total_per_lot();
        }
        r.volume += static_cast<double>(attr.at(0).volume);
    }
    const double n = static_cast<double>(kSeeds);
    for (std::size_t h = 0; h < kHorizons.size(); ++h) {
        r.spread[h] /= n; r.adverse[h] /= n; r.total[h] /= n;
    }
    r.volume /= n;
    return r;
}

}  // namespace

int main() {
    std::printf("informed,frozen,horizon,spread_capture,adverse_selection,total,volume\n");

    for (const double informed : {0.0, 0.5, 1.0, 1.5, 2.0, 3.0, 4.0, 6.0, 8.0}) {
        const Row r = measure(informed, false);
        for (std::size_t h = 0; h < kHorizons.size(); ++h)
            std::printf("%.2f,0,%.2f,%.6f,%.6f,%.6f,%.0f\n",
                        informed, kHorizons[h], r.spread[h], r.adverse[h], r.total[h], r.volume);
    }
    // The control: no information in the system at all. Whatever adverse
    // selection reads here is price impact, not information.
    for (const double informed : {2.0, 4.0}) {
        const Row r = measure(informed, true);
        for (std::size_t h = 0; h < kHorizons.size(); ++h)
            std::printf("%.2f,1,%.2f,%.6f,%.6f,%.6f,%.0f\n",
                        informed, kHorizons[h], r.spread[h], r.adverse[h], r.total[h], r.volume);
    }
    return 0;
}
