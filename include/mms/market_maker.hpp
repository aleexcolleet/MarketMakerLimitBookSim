#pragma once

#include "mms/flow.hpp"        // for the Owner tags
#include "mms/order_book.hpp"
#include "mms/types.hpp"

#include <cstddef>

namespace mms {
    // Order ids are partitioned rather than allocated from a shared counter:
    // participants count upward from 1, the maker from 2^63. A shared allocator
    // would couple the two components for no benefit, and a collision would be
    // *silently rejected* by the book — presenting as a maker that mysteriously
    // stops quoting, which is a horrible thing to debug.
    inline constexpr  OrderId kMarketMakerIdBase = 1ull << 63;

    struct MarketMakerParams {
        // The half-spread, in HALF-ticks — the same unit as TopOfBook::mid_x2().
        // 1 is half a tick either side, which exactly joins a one-tick market;
        // 2 is a full tick. Whole ticks are too coarse: the market's own spread
        // here is one or two ticks, so a whole-tick half-spread quotes outside it
        // and never trades.
        Price half_spread_x2 = 2;
        Quantity quote_size = 10;

        // How far fair value must move before cancelling and re-posting. Requoting
        // keeps the quotes current but sends them to the back of the queue, and
        // time priority means the back of the queue rarely fills.
        Price requote_ticks = 1;

        // Inventory control. Skew is the soft mechanism and does nearly all the
        // work; the cap is a hard backstop
        Quantity max_position = 100;
        double skew_per_lot = 0.03;

        std::size_t depth_scan = 4; // levels to walk when excluding own quotes
    };

    struct MarketMakerStats {
        Quantity position = 0;
        double cash = 0.0;
        Quantity volume = 0;
        std::size_t fills = 0;
        std::size_t buys = 0;
        std::size_t sells = 0;

        std::size_t quotes_posted    = 0;
        std::size_t quotes_cancelled = 0;
        std::size_t requotes         = 0;
        std::size_t updates          = 0;
        std::size_t updates_quoted   = 0;
        std::size_t bid_suppressed   = 0;
        std::size_t ask_suppressed   = 0;

        double      abs_position_sum = 0.0;   // diagnostics for the skew test
        std::size_t position_samples = 0;
    };

    // A quoting market maker.
    //
    // The constraint that makes every number this produces meaningful: it is
    // constructed with an OrderBook and nothing else. It has no ValueProcess and no
    // way to reach one, so it never sees the latent value. Everything it believes
    // about fair value comes from the book — which is the information a real market
    // maker has. Give it the latent value and it becomes an oracle, and every
    // result the project produces becomes an artefact of that.

    class MarketMaker {
    public:
        MarketMaker(OrderBook& book, const MarketMakerParams& params);

        // Same reason as FlowGenerator: the constructor installs a trade listener
        // capturing 'this'
        MarketMaker(const MarketMaker&) = delete;
        MarketMaker& operator=(const MarketMaker&) = delete;
        MarketMaker(MarketMaker&&) = delete;
        MarketMaker& operator=(MarketMaker&&) = delete;

        // Called after each market event. Decides whether to requote.
        void on_market_update();

        const MarketMakerStats& stats() const noexcept { return stats_; }

        // cash + inventory marked at `fair`. Marking against the latent value says
        // what the maker really made; marking against the mid says what it could
        // realise now. The tests use the former.
        double mark_to_market(double fair) const noexcept;

        bool quoting() const noexcept { return quoting_; }
        Price quoted_bid() const noexcept { return bid_price_; }
        Price quoted_ask() const noexcept { return ask_price_; }

    private:
        void on_trade(const Trade& t);
        bool market_touch(Side side, Price& out) const;
        bool market_fair_x2(Price& out) const;
        void requote(Price fair_x2);
        void cancel_quotes();
        void post(Side side, Price price, Quantity size);
        OrderId next_id() noexcept { return ++last_id_; }

        OrderBook* book_;
        MarketMakerParams params_;

        OrderId  last_id_        = kMarketMakerIdBase;
        bool     quoting_        = false;
        Price    quoted_fair_x2_ = 0;

        OrderId  bid_id_      = 0;
        OrderId  ask_id_      = 0;
        Price    bid_price_   = kInvalidPrice;
        Price    ask_price_   = kInvalidPrice;
        Quantity bid_resting_ = 0;
        Quantity ask_resting_ = 0;

        MarketMakerStats stats_;
    };

}// namespace mms
