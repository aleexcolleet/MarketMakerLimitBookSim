#pragma once

#include "mms/types.hpp"

#include <cstddef>
#include <functional>
#include <vector>


namespace mms {

    class OrderBook {
    public:
        using TradeCallback = std::function<void(const Trade&)>;

        OrderBook();
        ~OrderBook();

        OrderBook(const OrderBook&) = delete;
        OrderBook& operator=(const OrderBook&) = delete;
        OrderBook(OrderBook&&) noexcept;
        OrderBook& operator=(OrderBook&&) noexcept;

        //callback sink
        void set_trade_callback(TradeCallback cb);

        //Order Entry
        Quantity submit(const Order& order);
        bool cancel(OrderId id);

        //Queries
        TopOfBook top_of_book() const;
        Price best_bid() const;
        Price best_ask() const;
        Quantity size_at(Side side, Price price) const;
        Quantity total_quantity(Side side) const;
        std::size_t order_count() const;
        bool contains(OrderId id) const;


        struct Level {
            Price price;
            Quantity quantity;
            std::size_t order_count;
        };

        std::vector<Level> depth(Side side, std::size_t levels) const;

        void clear();

    private:
        struct Impl;
        Impl* impl_;

    };
}
