#pragma once

#include "mms/flow.hpp"
#include "mms/market_maker.hpp"
#include "mms/order_book.hpp"
#include "mms/types.hpp"
#include "mms/value_process.hpp"

#include <cstddef>
#include <deque>
#include <vector>

namespace mms {

    struct Markout {
        double horizon = 0.0;

        // Measured against the MID - the decomposition a real desk can compute.
        double spread_capture = 0.0;
        double adverse_selection = 0.0;

        // Measured against the LATENT VALUE - the truth only a simulator has.
        // Kept so the observable measure can be checked against the unobservable one.
        double true_edge = 0.0;
        double value_drift = 0.0;

        Quantity volume = 0;
        std::size_t fills = 0;

        double total() const noexcept { return spread_capture + adverse_selection; }
        double true_total() const noexcept { return true_edge + value_drift; }

        double spread_per_lot() const noexcept {
            return volume ? spread_capture / static_cast<double>(volume) : 0.0;
        }
        double adverse_per_lot() const noexcept {
            return volume ? adverse_selection / static_cast<double>(volume) : 0.0;
        }
        double total_per_lot() const noexcept {
            return volume ? total() / static_cast<double>(volume) : 0.0;
        }
        double true_edge_per_lot() const noexcept {
            return volume ? true_edge / static_cast<double>(volume) : 0.0;
        }
        double value_drift_per_lot() const noexcept {
            return volume ? value_drift / static_cast<double>(volume) : 0.0;
        }
    };

    // Markout-based attribution of the market maker's P&L.
    //
    // This is a separate component from MarketMaker, and the split is the honesty of
    // the whole project:
    //
    //   MarketMaker      sees the book                 -> trades blind, like a real one
    //   PnlAttribution   sees the book, V, the clock   -> measures, like an analyst
    //                                                     with hindsight
    //
    // Merging them would mean handing the maker the latent value, and the moment it
    // can see V it can quote off V, and every number becomes an artefact.

    class PnlAttribution {
    public:
        PnlAttribution(OrderBook& book, const ValueProcess& value,
            const FlowGenerator& flow, const std::vector<double>& horizons);

        // Same reason as FlowGenerator and MarketMaker: a this-capturing listener.
        PnlAttribution(const PnlAttribution&) = delete;
        PnlAttribution& operator=(const PnlAttribution&) = delete;
        PnlAttribution(PnlAttribution&&) = delete;
        PnlAttribution& operator=(PnlAttribution&&) = delete;

        // Called after each event. Retires every fill that has reached its horizon.
        void settle();

        const std::vector<Markout>& markouts() const noexcept { return markouts_; }
        const Markout& at(std::size_t i) const { return markouts_.at(i); }

        // Fills still waiting for their horizon. Dropped at the end of a run rather
        // than flushed - see settle().
        std::size_t unsettled() const noexcept;

    private:
        struct PendingFill {
            double time = 0.0;
            double price = 0.0;
            double mid_at_trade = 0.0;
            double fair_at_trade = 0.0;
            Quantity signed_size = 0;
        };

        // One deque per horizon. Fills are appended at the back in time order and
        // retired from the front once matured — the same access pattern, and the
        // same argument, as the FIFO queue in a price level.
        //
        // Not one shared list with per-horizon cursors: that cannot pop until *every*
        // horizon has consumed an element, which turns an obvious loop into
        // bookkeeping. Copying a small struct three times is cheaper than being clever.
        struct Bucket { std::deque<PendingFill> pending; };

        void on_trade(const Trade& t);

        OrderBook* book_;
        const ValueProcess* value_;
        const FlowGenerator* flow_;

        std::vector<Bucket>  buckets_;
        std::vector<Markout> markouts_;
        double               last_mid_ = 0.0;
        bool                 have_mid_ = false;
    };
} // namespace mms
