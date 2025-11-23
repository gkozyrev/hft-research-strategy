#include "mexc/spot_client.hpp"
#include "mexc/ws_spot_client.hpp"
#include "strategy/orderbook_manager.hpp"
#include "strategy/orderbook_display.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace {
volatile std::sig_atomic_t g_running = 1;

void signal_handler(int signal) {
    g_running = 0;
}

std::string trim(std::string value) {
    const auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

void load_env_file(const std::string& path) {
    std::ifstream env_file(path);
    if (!env_file.is_open()) {
        return;
    }

    std::string line;
    while (std::getline(env_file, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }

        const auto pos = line.find('=');
        if (pos == std::string::npos) {
            continue;
        }

        auto key = trim(line.substr(0, pos));
        auto value = trim(line.substr(pos + 1));

        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }

        if (!key.empty()) {
            setenv(key.c_str(), value.c_str(), 1);
        }
    }
}

mexc::Credentials load_credentials_from_env() {
    const char* api_key = std::getenv("MEXC_API_KEY");
    const char* api_secret = std::getenv("MEXC_API_SECRET");
    return mexc::Credentials{api_key ? api_key : "", api_secret ? api_secret : ""};
}

} // namespace

int main(int argc, char* argv[]) {
    // Parse symbol from command line or use default
    std::string symbol = "SPYXUSDT";
    if (argc > 1) {
        symbol = argv[1];
    }
    
    // Transform to uppercase
    std::transform(symbol.begin(), symbol.end(), symbol.begin(), ::toupper);
    
    std::cout << "Starting OrderBook Viewer for " << symbol << std::endl;
    std::cout << "Press Ctrl+C to exit\n" << std::endl;
    
    // Set up signal handler
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    
    // Load credentials (optional, only needed for user data streams)
    load_env_file(".env");
    auto credentials = load_credentials_from_env();
    
    try {
        // Create REST client for initial snapshot
        mexc::SpotClient rest_client(credentials);
        
        // Create WebSocket client
        mexc::WsSpotClient ws_client(credentials);
        
        // Create orderbook manager
        strategy::OrderBookManager ob_manager(symbol);
        
        // Create display
        strategy::OrderBookDisplay display(symbol, 10);
        
        // Set up update callback to render orderbook with latency tracking
        ob_manager.set_update_callback([&display, &ob_manager](const strategy::OrderBookSnapshot& snapshot) {
            display.render(ob_manager.get_orderbook(), ob_manager.get_latency_tracker());
        });
        
        // Connect WebSocket
        std::cout << "Connecting to MEXC WebSocket..." << std::endl;
        if (!ws_client.connect()) {
            std::cerr << "Failed to connect to WebSocket" << std::endl;
            return 1;
        }
        
        // Wait for connection to establish
        std::cout << "Waiting for connection..." << std::endl;
        int wait_count = 0;
        while (!ws_client.is_connected() && wait_count < 50) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            wait_count++;
        }
        
        if (!ws_client.is_connected()) {
            std::cerr << "Connection timeout - WebSocket did not connect" << std::endl;
            return 1;
        }
        
        // Subscribe to depth stream (with REST client for initial snapshot)
        std::cout << "Subscribing to depth stream..." << std::endl;
        if (!ob_manager.subscribe(ws_client, &rest_client)) {
            std::cerr << "Failed to subscribe to depth stream" << std::endl;
            return 1;
        }
        
        std::cout << "Connected! Waiting for orderbook updates...\n" << std::endl;
        
        // Main loop - keep connection alive and render updates
        auto last_render_time = std::chrono::steady_clock::now();
        const auto render_interval = std::chrono::milliseconds(100); // Render at most 10 times per second
        
        while (g_running) {
            // Check if we should render (throttle rendering)
            auto now = std::chrono::steady_clock::now();
            if (now - last_render_time >= render_interval) {
                // Render current orderbook state with latency tracking
                if (ob_manager.get_orderbook().is_valid()) {
                    display.render(ob_manager.get_orderbook(), ob_manager.get_latency_tracker());
                }
                last_render_time = now;
            }
            
            // Small sleep to prevent CPU spinning
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        
        // Cleanup
        std::cout << "\n\nDisconnecting..." << std::endl;
        ob_manager.unsubscribe(ws_client);
        ws_client.disconnect();
        
        std::cout << "Goodbye!" << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}

