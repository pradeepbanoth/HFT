#include <iostream>
#include "orderbook.hpp"

int main() {
    hft::OrderBook book("BTCUSDT");

    book.apply_l2({
        "BTCUSDT",
        hft::BookSide::Bid,
        100.0,
        5.0,
        1,
        1
    });

    book.apply_l2({
        "BTCUSDT",
        hft::BookSide::Ask,
        101.0,
        3.0,
        2,
        2
    });

    auto bid = book.best_bid();
    auto ask = book.best_ask();
    auto mid = book.mid_price();

    if (bid && ask && mid) {
        std::cout << "Best bid: " << *bid << "\n";
        std::cout << "Best ask: " << *ask << "\n";
        std::cout << "Mid price: " << *mid << "\n";
        std::cout << "Spread: " << *book.spread() << "\n";
    }

    return 0;
}