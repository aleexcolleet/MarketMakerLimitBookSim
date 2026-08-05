#pragma once

#include <cstdint>
#include <limits>

namespace mms {
    using Price = std::int64_t; //in TICKS, never currency
    using Quantity = std::int64_t; // in lots
    using OrderId = std::uint64_t;
    using Timestamp = std::uint64_t; // simulated nanoseconds


inline constexpr Price kInvalidPrice = std::numeric_limits<Price>::min();

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

constexpr Side opposite(Side s) noexcept {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

constexpr int sign_of(Side s) noexcept {
    return s == Side::Buy ? 1 : -1;
}


enum class OrderType : std::uint8_t {
    Limit, // rests on the book if not immediately marketable
    Market, // takes whatever is available, never rests
    IOC, // immediate-or-cancel: fill what you can, discard the rest
};

struct Order {
    OrderId id = 0;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    Price price = 0; // ignored for Market
    Quantity quantity = 0; // original size
    Quantity remaining = 0; // unfilled size
    Timestamp timestamp = 0; // arrival time - breaks price ties
    int owner = 0; // 0 = market participant, 1 = our MM
};

struct Trade {
    Price price = 0;
    Quantity quantity = 0;
    OrderId aggressor_id = 0;
    OrderId resting_id = 0;
    Side aggressor_side = Side::Buy;
    Timestamp timestamp = 0;
    int aggressor_owner = 0;
    int resting_owner = 0;
};

struct TopOfBook {
    Price bid_price = kInvalidPrice;
    Quantity bid_size = 0;
    Price ask_price = kInvalidPrice;
    Quantity ask_size = 0;

    bool has_bid() const noexcept { return bid_price != kInvalidPrice; }
    bool has_ask() const noexcept { return ask_price != kInvalidPrice; }
    bool is_two_sided() const noexcept { return has_bid() && has_ask(); }

    Price spread() const noexcept {
        return is_two_sided() ? (ask_price - bid_price) : kInvalidPrice;
    }

    Price mid_x2() const noexcept {
        return is_two_sided() ? (bid_price + ask_price) : kInvalidPrice;
    }
};

}

