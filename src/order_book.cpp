#include "mms/order_book.hpp"

#include <deque>
#include <functional>
#include <map>
#include <unordered_map>
#include <utility>
#include <algorithm>

namespace mms {
    namespace {

        struct RestingOrder {
            OrderId id = 0;
            Quantity remaining = 0;
            Timestamp timestamp = 0;
            int owner = 0;
        };

        struct PriceLevel {
            std::deque<RestingOrder> queue;
            Quantity total = 0;

            bool empty() const { return queue.empty(); }
        };

        struct Locator {
            Side side = Side::Buy;
            Price price = 0;
        };

        //Block A

        template <typename SideMap>
        Price best_price(const SideMap& m) {
            return m.empty() ? kInvalidPrice : m.begin()->first;
        }

        template <typename SideMap>
        void rest_order(SideMap& m, const Order& o, Quantity qty) {
            PriceLevel& lvl = m[o.price];
            lvl.queue.push_back(RestingOrder{o.id, qty, o.timestamp, o.owner});
            lvl.total += qty;
        }

        template <typename SideMap>
        bool remove_order(SideMap& m, Price price, OrderId id) {
            auto lit = m.find(price);
            if (lit == m.end()) return false;

            PriceLevel& lvl = lit->second;
            for (auto it = lvl.queue.begin(); it != lvl.queue.end(); ++it) {
                if (it->id == id) {
                    lvl.total -= it->remaining;
                    lvl.queue.erase(it);
                    if (lvl.queue.empty()) m.erase(lit);
                    return true;
                }
            }
            return false;
        }

        template <typename SideMap>
        Quantity quantity_at(const SideMap& m, Price price) {
            const auto it = m.find(price);
            return it == m.end() ? 0 : it->second.total;
        }

        template <typename SideMap>
        Quantity sum_quantity(const SideMap& m) {
            Quantity total = 0;
            for (const auto& [price, lvl] : m) { (void)price; total += lvl.total; }
            return total;
        }

        template <typename SideMap>
        std::vector<OrderBook::Level> collect_depth(const SideMap& m, std::size_t levels) {
            std::vector<OrderBook::Level> out;
            for (const auto& [price, lvl] : m) {
                if (out.size() >= levels) break;
                out.push_back(OrderBook::Level{price, lvl.total, lvl.queue.size()});
            }
            return out;
        }

        bool is_marketable(const Order& incoming, Price resting_price) {
            if (incoming.type == OrderType::Market) return true;
            return incoming.side == Side::Buy ? incoming.price >= resting_price
                                            : incoming.price <= resting_price;
        }


    }// anonymous namespace

    //Block 3
    struct OrderBook::Impl {
        std::map<Price, PriceLevel, std::greater<Price>> bids;
        std::map<Price, PriceLevel, std::less<Price>>    asks;
        std::unordered_map<OrderId, Locator> index;
        std::size_t live_orders = 0;
        std::vector<TradeCallback> listeners;

        template <typename SideMap>
        Quantity match(SideMap& opposite, const Order& incoming, Quantity& remaining);
    };


    //Block 4
    OrderBook::OrderBook() : impl_(new Impl()) {}
    OrderBook::~OrderBook() { delete impl_; }

    OrderBook::OrderBook(OrderBook&& other) noexcept : impl_(other.impl_) {
        other.impl_ = nullptr;
    }

    OrderBook& OrderBook::operator=(OrderBook&& other) noexcept {
        if (this != &other) {
            delete impl_;
            impl_ = other.impl_;
            other.impl_ = nullptr;
        }
        return *this;
    }

    //Block 5
    void OrderBook::add_trade_listener(TradeCallback cb) {
        impl_->listeners.push_back(std::move(cb));
    }

    void OrderBook::clear() {
        impl_->bids.clear();
        impl_->asks.clear();
        impl_->index.clear();
        impl_->live_orders = 0;
    }

    std::size_t OrderBook::order_count() const {
        return impl_->live_orders;
    }

    bool OrderBook::contains(OrderId id) const {
        return impl_->index.find(id) != impl_->index.end();
    }

    //Block B
    Quantity OrderBook::submit(const Order& order) {
        if (order.quantity <= 0) return 0;
        if (contains(order.id)) return 0;

        Quantity remaining = order.quantity;

        // A buy crosses against the asks; a sell crosses against the bids.
        const Quantity executed = (order.side == Side::Buy)
            ? impl_->match(impl_->asks, order, remaining)
            : impl_->match(impl_->bids, order, remaining);

        // Only limit orders rest. Market and IOC discard the remainder.
        if (remaining > 0 && order.type == OrderType::Limit) {
            if (order.side == Side::Buy) rest_order(impl_->bids, order, remaining);
            else                         rest_order(impl_->asks, order, remaining);

            impl_->index[order.id] = Locator{order.side, order.price};
            ++impl_->live_orders;
        }

        return executed;
    }

    //Block C
    bool OrderBook::cancel(OrderId id) {
        const auto it = impl_->index.find(id);
        if (it == impl_->index.end()) return false;

        const Locator loc = it->second;
        const bool removed = (loc.side == Side::Buy)
            ? remove_order(impl_->bids, loc.price, id)
            : remove_order(impl_->asks, loc.price, id);
        if (removed) {
            impl_->index.erase(it);
            --impl_->live_orders;
        }
        return removed;
    }

    //Block D
    Price OrderBook::best_bid() const { return best_price(impl_->bids); }
    Price OrderBook::best_ask() const { return best_price(impl_->asks); }

    Quantity OrderBook::size_at(Side side, Price price) const {
        return side == Side::Buy ? quantity_at(impl_->bids, price)
                                : quantity_at(impl_->asks, price);
    }

    Quantity OrderBook::total_quantity(Side side) const {
        return side == Side::Buy ? sum_quantity(impl_->bids)
                                : sum_quantity(impl_->asks);
    }

    std::vector<OrderBook::Level> OrderBook::depth(Side side, std::size_t levels) const {
        return side == Side::Buy ? collect_depth(impl_->bids, levels)
                                : collect_depth(impl_->asks, levels);
    }

    TopOfBook OrderBook::top_of_book() const {
        TopOfBook t;
        if (!impl_->bids.empty()) {
            const auto& [p, l] = *impl_->bids.begin();
            t.bid_price = p;
            t.bid_size = l.total;
        }
        if (!impl_->asks.empty()) {
            const auto& [p, l] = *impl_->asks.begin();
            t.ask_price = p;
            t.ask_size = l.total;
        }
        return t;
    }

    template <typename SideMap>
    Quantity OrderBook::Impl::match(SideMap& opposite, const Order& incoming, Quantity& remaining) {
        Quantity executed = 0;

        while (remaining > 0 && !opposite.empty()) {
            const auto level_it = opposite.begin();
            const Price level_price = level_it->first;

            if (!is_marketable(incoming, level_price)) break;

            PriceLevel& lvl = level_it->second;

            while (remaining > 0 && !lvl.queue.empty()) {
                RestingOrder& resting = lvl.queue.front();
                const Quantity traded = std::min(remaining, resting.remaining);

                // Build the trade BEFORE mutating: `resting` dangles after pop_front().
                Trade t;
                t.price           = level_price;   // executes at the RESTING price
                t.quantity        = traded;
                t.aggressor_id    = incoming.id;
                t.resting_id      = resting.id;
                t.aggressor_side  = incoming.side;
                t.timestamp       = incoming.timestamp;
                t.aggressor_owner = incoming.owner;
                t.resting_owner   = resting.owner;

                resting.remaining -= traded;
                lvl.total         -= traded;
                remaining         -= traded;
                executed          += traded;

                if (resting.remaining == 0) {
                    index.erase(resting.id);
                    --live_orders;
                    lvl.queue.pop_front();
                }
                // A partial fill keeps its queue position by construction: we
                // decrement and do not pop.

                for (const auto& l : listeners) l(t);
            }

            // Erase the level only after we have finished with it — `lvl` is a
            // reference into the map and erasing invalidates it.
            if (lvl.queue.empty()) opposite.erase(level_it);
        }

        return executed;
    }

}// namespace mms
