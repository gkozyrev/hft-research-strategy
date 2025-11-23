#include "strategy/orderbook_display.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>

namespace strategy {

OrderBookDisplay::OrderBookDisplay(const std::string& symbol, int levels)
    : symbol_(symbol),
      levels_(levels) {
}

void OrderBookDisplay::clear() {
    // Clear screen and move cursor to top
    std::cout << "\033[2J\033[H";
    first_render_ = true;
}

std::string OrderBookDisplay::format_price(double price, int precision) const {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(precision) << price;
    return oss.str();
}

std::string OrderBookDisplay::format_quantity(double qty, int precision) const {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(precision) << qty;
    return oss.str();
}

std::string OrderBookDisplay::format_volume(double volume) const {
    std::ostringstream oss;
    if (volume >= 1000000.0) {
        oss << std::fixed << std::setprecision(2) << (volume / 1000000.0) << "M";
    } else if (volume >= 1000.0) {
        oss << std::fixed << std::setprecision(2) << (volume / 1000.0) << "K";
    } else {
        oss << std::fixed << std::setprecision(2) << volume;
    }
    return oss.str();
}

void OrderBookDisplay::print_header() {
    std::cout << BOLD << CYAN << "\n╔═══════════════════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║" << std::setw(50) << std::right << ("ORDER BOOK: " + symbol_) << std::setw(35) << "║\n";
    std::cout << "╚═══════════════════════════════════════════════════════════════════════════════╝" << RESET << "\n\n";
    
    std::cout << BOLD << std::setw(20) << std::left << "  PRICE" 
              << std::setw(20) << "QUANTITY"
              << std::setw(20) << "VOLUME"
              << " │ "
              << std::setw(20) << std::right << "PRICE"
              << std::setw(20) << "QUANTITY"
              << std::setw(20) << "VOLUME" << RESET << "\n";
    std::cout << BOLD << std::setw(20) << std::left << "  ASK (SELL)" 
              << std::setw(20) << ""
              << std::setw(20) << ""
              << " │ "
              << std::setw(20) << std::right << "BID (BUY)"
              << std::setw(20) << ""
              << std::setw(20) << "" << RESET << "\n";
    std::cout << std::string(60, '-') << "|" << std::string(60, '-') << "\n";
}

void OrderBookDisplay::print_separator() {
    std::cout << std::string(60, '-') << "|" << std::string(60, '-') << "\n";
}

void OrderBookDisplay::print_asks(const std::vector<PriceLevel>& asks, double best_ask) {
    // Print asks in reverse order (highest first, going down)
    std::vector<PriceLevel> reversed_asks = asks;
    std::reverse(reversed_asks.begin(), reversed_asks.end());
    
    int count = 0;
    for (const auto& level : reversed_asks) {
        if (count >= levels_) break;
        
        bool is_best = (std::fabs(level.price - best_ask) < 1e-6);
        std::string price_color = is_best ? RED : RESET;
        std::string price_style = is_best ? BOLD : "";
        
        std::cout << "  " << price_style << price_color 
                  << std::setw(20) << std::right << format_price(level.price)
                  << std::setw(20) << std::right << format_quantity(level.quantity)
                  << std::setw(20) << std::right << format_volume(level.price * level.quantity)
                  << RESET;
        
        std::cout << " │ ";
        
        // Empty space for bids side
        std::cout << std::setw(20) << "" << std::setw(20) << "" << std::setw(20) << "";
        
        std::cout << "\n";
        count++;
    }
    
    // Fill remaining ask rows if needed
    for (int i = count; i < levels_; ++i) {
        std::cout << "  " << GRAY << std::setw(20) << "" << std::setw(20) << "" << std::setw(20) << "" << RESET;
        std::cout << " │ ";
        std::cout << std::setw(20) << "" << std::setw(20) << "" << std::setw(20) << "";
        std::cout << "\n";
    }
}

void OrderBookDisplay::print_spread(double best_bid, double best_ask, double spread) {
    double spread_bps = (best_bid > 0.0 && best_ask > 0.0) 
        ? (spread / best_bid) * 10000.0 
        : 0.0;
    
    std::cout << BOLD << YELLOW;
    std::cout << "  " << std::setw(20) << std::right << format_price(best_ask)
              << std::setw(20) << std::right << "SPREAD"
              << std::setw(20) << std::right << (format_price(spread) + " (" + format_price(spread_bps) + " bps)");
    std::cout << RESET;
    std::cout << " │ ";
    std::cout << BOLD << GREEN << std::setw(20) << std::right << format_price(best_bid) << RESET;
    std::cout << std::setw(20) << "" << std::setw(20) << "";
    std::cout << "\n";
    print_separator();
}

void OrderBookDisplay::print_bids(const std::vector<PriceLevel>& bids, double best_bid) {
    int count = 0;
    for (const auto& level : bids) {
        if (count >= levels_) break;
        
        bool is_best = (std::fabs(level.price - best_bid) < 1e-6);
        std::string price_color = is_best ? GREEN : RESET;
        std::string price_style = is_best ? BOLD : "";
        
        // Empty space for asks side
        std::cout << "  " << std::setw(20) << "" << std::setw(20) << "" << std::setw(20) << "";
        std::cout << " │ ";
        
        std::cout << price_style << price_color
                  << std::setw(20) << std::right << format_price(level.price)
                  << std::setw(20) << std::right << format_quantity(level.quantity)
                  << std::setw(20) << std::right << format_volume(level.price * level.quantity)
                  << RESET;
        
        std::cout << "\n";
        count++;
    }
    
    // Fill remaining bid rows if needed
    for (int i = count; i < levels_; ++i) {
        std::cout << "  " << std::setw(20) << "" << std::setw(20) << "" << std::setw(20) << "";
        std::cout << " │ ";
        std::cout << GRAY << std::setw(20) << "" << std::setw(20) << "" << std::setw(20) << "" << RESET;
        std::cout << "\n";
    }
}

void OrderBookDisplay::print_stats(const OrderBookSnapshot& snapshot) {
    std::cout << "\n" << BOLD << BLUE << "Stats:" << RESET << "\n";
    std::cout << "  Best Bid: " << GREEN << BOLD << format_price(snapshot.best_bid) << RESET
              << "  Best Ask: " << RED << BOLD << format_price(snapshot.best_ask) << RESET
              << "  Spread: " << YELLOW << format_price(snapshot.spread) << RESET
              << "  Microprice: " << CYAN << format_price(snapshot.microprice) << RESET << "\n";
    std::cout << "  Bid Volume (" << levels_ << " levels): " << GREEN << format_volume(snapshot.bid_volume) << RESET
              << "  Ask Volume (" << levels_ << " levels): " << RED << format_volume(snapshot.ask_volume) << RESET << "\n";
    std::cout << "  Last Update ID: " << GRAY << snapshot.last_update_id << RESET << "\n";
}

void OrderBookDisplay::render(const OrderBook& orderbook) {
    if (!orderbook.is_valid()) {
        std::cout << RED << "Order book is not valid (missing bids or asks)" << RESET << "\n";
        return;
    }
    
    // Clear screen on first render
    if (first_render_) {
        clear();
        first_render_ = false;
    } else {
        // Move cursor to top
        std::cout << "\033[H";
    }
    
    print_header();
    
    // Get orderbook data
    auto asks = orderbook.get_asks(levels_);
    auto bids = orderbook.get_bids(levels_);
    double best_bid = orderbook.best_bid();
    double best_ask = orderbook.best_ask();
    double spread = orderbook.spread();
    
    // Print asks (top half)
    print_asks(asks, best_ask);
    
    // Print spread separator
    print_spread(best_bid, best_ask, spread);
    
    // Print bids (bottom half)
    print_bids(bids, best_bid);
    
    // Print stats
    auto snapshot = orderbook.get_snapshot(levels_, false);
    print_stats(snapshot);
    
    std::cout << std::flush;
}

void OrderBookDisplay::render_snapshot(const OrderBookSnapshot& snapshot) {
    // Clear screen on first render
    if (first_render_) {
        clear();
        first_render_ = false;
    } else {
        // Move cursor to top
        std::cout << "\033[H";
    }
    
    print_header();
    
    // Print asks
    print_asks(snapshot.asks, snapshot.best_ask);
    
    // Print spread separator
    print_spread(snapshot.best_bid, snapshot.best_ask, snapshot.spread);
    
    // Print bids
    print_bids(snapshot.bids, snapshot.best_bid);
    
    // Print stats
    print_stats(snapshot);
    
    std::cout << std::flush;
}

} // namespace strategy

