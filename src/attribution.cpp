#include "mms/attribution.hpp"

namespace mms {
    PnlAttribution::PnlAttribution(OrderBook& book, const ValueProcess& value,
        const FlowGenerator& flow, const std::vector<double>& horizons)
            : book_(&book), value_(&value), flow_(&flow) {
        buckets_.resize(horizons.size());
        markouts_.reserve(horizons.size());
        for (const double h : horizons) {
            Markout m;
            m.horizon = h;
            markouts_.push_back(m);
        }
        // The third listener on the book, after the flow generator and the maker.
        // This is what add_trade_listener was built for in Stage 6.
        book_->add_trade_listener([this](const Trade& t) { on_trade(t); });
    }

    void PnlAttribution::on_trade(const Trade& t) {
        // The maker only ever rests, so its fills arrive as the resting side.
        if (t.resting_owner != kMarketMaker) return;

        PendingFill f;
        f.time = flow_->clock();
        f.price = static_cast<double>(t.price);
        // The mid sampled at the end of the *previous* event. It cannot be read now:
        // this callback fires mid-match, when the book is half-updated and still
        // holds the maker's own quotes. One event of staleness (~0.1 ticks) buys a
        // reference that is genuinely pre-trade rather than contaminated by the
        // trade itself.
        f.mid_at_trade = last_mid_;
        f.fair_at_trade = value_->value();
        f.signed_size = (t.aggressor_side == Side::Buy) ? -t.quantity : t.quantity;

        if (!have_mid_) return; // no reference price yet - early in the run
        for (auto& b : buckets_) b.pending.push_back(f);
    }

    void PnlAttribution::settle() {
        const double now = flow_->clock();
        const double fair = value_->value();

        const TopOfBook top = book_->top_of_book();
        if (top.is_two_sided()) {
            last_mid_ = static_cast<double>(top.mid_x2()) / 2.0;
            have_mid_ = true;
        }
        const double mid = last_mid_;

        for (std::size_t i = 0; i < buckets_.size(); ++i) {
            Markout& m = markouts_[i];
            auto& q = buckets_[i].pending;

            // Retire everything that has reached its horizon. Settled at the first
            // event at or after t + horizon — real markouts are computed at the
            // nearest observation after the horizon too.
            while (!q.empty() && q.front().time + m.horizon <= now) {
                const PendingFill f = q.front();
                q.pop_front();

                const double size = static_cast<double>(f.signed_size);

                // The two mid-based terms telescope: the intermediate M(t) cancels,
                // so they sum to (M(t+tau) - P) * Q by construction. Which reference
                // you use decides which bucket the loss lands in, and the mid is the
                // right one — adverse selection is the market's *correction*, and
                // the mid is the thing that corrects. Measured against the latent
                // value instead, the whole cost of informed flow lands in spread
                // capture, because the mid is already stale at trade time and V is a
                // martingale afterwards.
                m.spread_capture += (f.mid_at_trade - f.price) * size;
                m.adverse_selection += (mid - f.mid_at_trade) * size;

                // The same decomposition against the truth. Only a simulator can do
                // this, and it is what validates the mid-based pair.
                m.true_edge += (f.fair_at_trade - f.price) * size;
                m.value_drift += (fair - f.fair_at_trade) * size;

                m.volume += (f.signed_size >= 0 ? f.signed_size : -f.signed_size);
                ++m.fills;
            }
        }
    }

    std::size_t PnlAttribution::unsettled() const noexcept {
        std::size_t n = 0;
        for (const auto& b : buckets_) n += b.pending.size();
        return n;
    }
}
