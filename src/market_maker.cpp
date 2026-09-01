
#include "mms/market_maker.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace mms {
    MarketMaker::MarketMaker(OrderBook& book, const MarketMakerParams& params)
        : book_(&book), params_(params) {
        book_->add_trade_listener([this](const Trade& t) { on_trade(t); });
    }

    // The best price on `side` *excluding our own quote*.
    //
    // This is the piece that is easy to omit and fails silently. Our quotes are in
    // the book, so once we are quoting, the mid we read is largely our own — we are
    // looking in a mirror. The estimate stops responding to the market, the quotes
    // stop moving, and we sit at a stale price being picked off.
    //
    // So: walk the depth from the touch inward, subtract our own resting size at our
    // own price, and take the first level with anything left. That covers both
    // cases. If we *joined* the market touch, subtracting our size leaves the rest
    // of that level and the price is unchanged. If we *improved* on it, subtracting
    // leaves nothing and we step to the next level, which is the market's real touch.
    //
    // A market maker must always be able to tell its own liquidity from the
    // market's. That is an operational requirement, not a simulation artefact.
    bool MarketMaker::market_touch(Side side, Price& out) const {
        const Price own_price = (side == Side::Buy) ? bid_price_ : ask_price_;
        const Quantity own_size = (side == Side::Buy) ? bid_resting_ : ask_resting_;

        const auto levels = book_->depth(side, params_.depth_scan);
        for (const auto& lvl : levels) {
            Quantity qty = lvl.quantity;
            if (quoting_ && lvl.price == own_price) qty -= own_size;
            if (qty > 0) {out = lvl.price; return true; }
        }
        return false;
    }

    bool MarketMaker::market_fair_x2(Price& out) const {
        Price bid, ask;
        if (!market_touch(Side::Buy, bid)) return false;
        if (!market_touch(Side::Sell, ask)) return false;
        out = bid + ask;
        return true;
    }

    void MarketMaker::on_market_update() {
        ++stats_.updates;

        Price fair_x2;
        if (!market_fair_x2(fair_x2)) return; //one-side market, nothing to price off

        stats_.abs_position_sum += std::fabs(static_cast<double>(stats_.position));
        ++stats_.position_samples;

        if (quoting_) {
            ++stats_.updates_quoted;
            const Price moved = static_cast<Price>(std::llabs(fair_x2 - quoted_fair_x2_));
            // Requote when fair value has moved far enough — or when either side has
            // been fully filled, which would otherwise leave us quoting one-sided
            // forever. (A side that is suppressed by the position limit counts as
            // alive; it is absent on purpose.)
            const bool both_alive = (bid_resting_ > 0 || stats_.position >= params_.max_position)
            && (ask_resting_ > 0 || stats_.position <= -params_.max_position);

            // Leaving the quotes alone preserves queue position. Time priority means
            // a re-posted order goes to the back and rarely fills, so requoting
            // trades currency for queue position — this threshold is the knob on it.
            if (moved < 2 * params_.requote_ticks && both_alive) return;
        }
        requote(fair_x2);

    }

    void MarketMaker::requote(Price fair_x2) {
    cancel_quotes();

    // Skew. Long inventory shifts *both* quotes down: the bid becomes less
    // competitive and the ask more so, so arriving flow pushes the position back
    // toward flat. This does nearly all the inventory control — mean |position|
    // falls from 636 to 12.9 with skew_per_lot = 0.03.
    const Price skew_x2 = static_cast<Price>(
        std::llround(2.0 * params_.skew_per_lot * static_cast<double>(stats_.position)));
    const Price centre_x2 = fair_x2 - skew_x2;
    const Price half_x2   = params_.half_spread_x2;

    // Round the bid DOWN and the ask UP, from an unrounded mid.
    //
    // Rounding the mid to a whole tick first and then quoting symmetrically
    // around it is wrong, and wrong in a way that hides. When the market spread
    // is odd — about 70% of the time here — the true mid sits at a half tick,
    // and rounding it moves *both* quotes half a tick in the same direction.
    // The bid becomes half a tick more competitive and the ask half a tick less,
    // so the maker buys systematically more than it sells. It produced a
    // position of +724 on 934 lots of volume before this was fixed.
    //
    // Half a tick is larger than the edge being measured. Same reason
    // TopOfBook::mid_x2() exists.
    const Price bid = (centre_x2 - half_x2) / 2;        // floor, for positive prices
    const Price ask = (centre_x2 + half_x2 + 1) / 2;    // ceil

    // Size each quote against the remaining room, rather than just suppressing
    // it at the limit. Merely suppressing is not enough: at a position of 19
    // against a cap of 20, a full 10-lot quote is already resting and filling it
    // takes the position to 29. Sizing makes the cap hard instead of approximate.
    const Quantity buy_room  = params_.max_position - stats_.position;
    const Quantity sell_room = params_.max_position + stats_.position;

    const Quantity bid_size = std::min(params_.quote_size, buy_room);
    const Quantity ask_size = std::min(params_.quote_size, sell_room);

    if (bid_size > 0) post(Side::Buy, bid, bid_size);
    else { bid_resting_ = 0; ++stats_.bid_suppressed; }

    if (ask_size > 0) post(Side::Sell, ask, ask_size);
    else { ask_resting_ = 0; ++stats_.ask_suppressed; }

    quoted_fair_x2_ = fair_x2;
    quoting_ = true;
    ++stats_.requotes;
    }

    void MarketMaker::cancel_quotes() {
        // cancel() returns false harmlessly for an id the book has never heard of,
        // so there is no need to track which quotes are still live before trying.
        if (quoting_) {
            if (book_-> cancel(bid_id_)) ++stats_.quotes_cancelled;
            if (book_->cancel(ask_id_)) ++stats_.quotes_cancelled;
        }
        bid_resting_ = 0;
        ask_resting_ = 0;
    }

    void MarketMaker::post(Side side, Price price, Quantity size) {
        Order o;
        o.id        = next_id();
        o.side      = side;
        o.type      = OrderType::Limit;
        o.price     = price;
        o.quantity  = size;
        o.remaining = o.quantity;
        o.timestamp = 0;
        o.owner     = kMarketMaker;

        // Quoting symmetrically around the market mid can never be marketable — the
        // mid is strictly between the touches — but read `executed` rather than
        // assume it. If the fair value estimate ever picks up our own quotes, this
        // is where it would show.
        const Quantity executed = book_->submit(o);
        const Quantity rest     = o.quantity - executed;

        if (side == Side::Buy) { bid_id_ = o.id; bid_price_ = price; bid_resting_ = rest; }
        else                   { ask_id_ = o.id; ask_price_ = price; ask_resting_ = rest; }
        ++stats_.quotes_posted;
    }

    void MarketMaker::on_trade(const Trade& t) {
        // We only ever rest; we never aggress. So our fills always arrive as the
        // resting side of somebody else's trade.
        if (t.resting_owner != kMarketMaker) return;

        // The aggressor bought, so we sold — and vice versa.
        const Quantity signed_qty = (t.aggressor_side == Side::Buy) ? -t.quantity : t.quantity;

        // Buying spends cash and adds inventory; selling does the reverse.
        stats_.position += signed_qty;
        stats_.cash     -= static_cast<double>(signed_qty) * static_cast<double>(t.price);
        stats_.volume   += t.quantity;
        ++stats_.fills;
        if (signed_qty > 0) ++stats_.buys; else ++stats_.sells;

        // Keep our own resting size current — market_touch() depends on it.
        if      (t.resting_id == bid_id_) bid_resting_ -= t.quantity;
        else if (t.resting_id == ask_id_) ask_resting_ -= t.quantity;
    }

    double MarketMaker::mark_to_market(double fair) const noexcept {
        return stats_.cash + static_cast<double>(stats_.position) * fair;
    }

}// namespace mms
