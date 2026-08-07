#include "mms/order_book.hpp"

#include <deque>
#include <functional>
#include <map>
#include <unordered_map>
#include <utility>


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
    }// anonymous namespace

    //Block 3
    struct OrderBook::Impl {
        std::map<Price, PriceLevel, std::greater<Price>> bids;
        std::map<Price, PriceLevel, std::less<Price>>    asks;
        std::unordered_map<OrderId, Locator> index;
        std::size_t live_orders = 0;
        TradeCallback on_trade;
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
    void OrderBook::set_trade_callback(TradeCallback cb) {
        impl_->on_trade = std::move(cb);
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


    //Block 6
    Quantity OrderBook::submit(const Order&) { return 0; }
    bool OrderBook::cancel(OrderId) { return false; }
    TopOfBook OrderBook::top_of_book() const { return TopOfBook{}; }
    Price OrderBook::best_bid() const { return kInvalidPrice; }
    Price OrderBook::best_ask() const { return kInvalidPrice; }
    Quantity OrderBook::size_at(Side, Price) const { return 0; }
    Quantity OrderBook::total_quantity(Side) const { return 0; }
    std::vector<OrderBook::Level>OrderBook::depth(Side, std::size_t) const {return {}; }
}// namespace mms