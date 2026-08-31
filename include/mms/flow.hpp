#pragma once

#include "mms/order_book.hpp"
#include "mms/random.hpp"
#include "mms/types.hpp"
#include "mms/value_process.hpp"

#include <cstddef>
#include <vector>

namespace mms {
    // Order::owner tags. The field has existed on Order since Stage 1, and Trade
    // has carried aggressor_owner and resting_owner just as long, doing nothing.
    // This is what they were for: attribution is a matter of reading them back.

    enum Owner : int {
        kLiquidity = 0,
        kNoise = 1,
        kInformed = 2,
        kMarketMaker = 3, //Phase 3
    };

    struct FlowParams {
        // Arrival intensities, in events per unit of time. Only the ratios shape
        // the market; the absolute scale sets how much latent value movement
        // happens between events.
        double rate_liquidity = 10.0; //background limit orders posted
        double cancel_rate = 0.10; //per *resting order*, not per market - see step()
        double rate_noise = 2.0; //uninformed marketable orders
        double rate_informed = 3.0; //informed arrivals (most of which do nothing)

        // Background liquidity
        Price depth_ticks = 6; // how far beyond the opposite touch orders are posted
        Quantity max_size = 10;

        // Informed Participants
        double informed_noise = 1.0; // s.d of their observation error, in ticks
        Price informed_edge = 2;
        Quantity informed_max_size = 20;

        Price initial_price = 10000;
    };

    // Diagnostics. Cheap to maintain, and the only way to tell whether the market
    // you generated resembles a market at all.
    struct FlowStats {
        std::size_t events           = 0;
        std::size_t orders_posted    = 0;
        std::size_t orders_cancelled = 0;
        std::size_t noise_orders     = 0;
        std::size_t informed_orders  = 0;
        std::size_t informed_passed  = 0;   // arrived, saw no edge, did nothing
        std::size_t trades           = 0;
        Quantity    volume           = 0;
        Quantity    submitted        = 0;

        // Realised edge against the latent value at the instant of the trade, in
        // tick-lots. Positive means the aggressor bought below fair value or sold
        // above it.
        double noise_edge    = 0.0;
        double informed_edge = 0.0;

        // Price discovery: how far the mid sits from the latent value, sampled
        // every event the book is two-sided.
        std::size_t two_sided_samples = 0;
        double abs_mid_error = 0.0;
    };

    class FlowGenerator {
        public:
        FlowGenerator(OrderBook& book, ValueProcess& value, Rng& rng, const FlowParams& params);
        // Not copyable and not movable. The constructor installs a trade callback
        // that captures `this`, so moving the object would leave the book calling
        // into the husk left behind. The alternative — reinstalling the callback in
        // a move constructor — is hidden coupling that will be forgotten the first
        // time someone adds a member. The deleted declarations are documentation:
        // this class has an identity, not just a value.
        //
        // General rule: any class that hands out a pointer or reference to itself
        // has, by that act, given up being movable for free.
        FlowGenerator(const FlowGenerator&) = delete;
        FlowGenerator& operator=(const FlowGenerator&) = delete;
        FlowGenerator(FlowGenerator&&) = delete;
        FlowGenerator& operator=(FlowGenerator&&) = delete;

        void step();
        void run(std::size_t n);

        double clock() const noexcept { return clock_; }
        const FlowStats& stats() const noexcept { return stats_; }
        const std::vector<OrderId>& resting_ids() const noexcept { return resting_; }

    private:
        void post_liquidity();
        void cancel_liquidity();
        void submit_noise();
        void submit_informed();
        void on_trade(const Trade& t);

        Price reference_for(Side side) const;
        OrderId next_id() noexcept { return ++last_id_; }
        Timestamp timestamp() const noexcept;
        void sample_mid();

        OrderBook* book_;
        ValueProcess* value_;
        Rng* rng_;
        FlowParams params_;

        double clock_ = 0.0;
        OrderId last_id_ = 0;
        Price last_price_;
        std::vector<OrderId> resting_; // candidate list for cancellation — see cancel_liquidity()
        FlowStats stats_;
    };

}// namespace mms
