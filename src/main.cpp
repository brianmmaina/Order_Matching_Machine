#include <iostream>
// std::cout: character output stream; << is overloaded for built-in types.

#include "order_book/order_book.hpp"

int main() {
    order_book::OrderBook book;
    // Order::make: static factory that assigns monotonic test ids (see order.h).
    book.addOrder(Order::make(100.0, 10, Order::BID, Order::LIMIT, 1));
    book.addOrder(Order::make(101.0, 5, Order::ASK, Order::LIMIT, 2));
    std::cout << "orders added; cancel bid: " << book.cancelOrder(1) << '\n';
    return 0;
}
