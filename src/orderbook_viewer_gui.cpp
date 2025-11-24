#include "mexc/spot_client.hpp"
#include "mexc/ws_spot_client.hpp"
#include "strategy/orderbook_manager.hpp"
#include "strategy/latency_tracker.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <deque>
#include <memory>

// ImGui and GLFW
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"

#ifdef __APPLE__
#define GL_SILENCE_DEPRECATION
#endif
#if defined(IMGUI_IMPL_OPENGL_ES2)
#include <GLES2/gl2.h>
#endif
#if defined(IMGUI_IMPL_OPENGL_LOADER_GL3W)
#include <GL/gl3w.h>
#elif defined(IMGUI_IMPL_OPENGL_LOADER_GLEW)
#include <GL/glew.h>
#elif defined(IMGUI_IMPL_OPENGL_LOADER_GLAD)
#include <glad/glad.h>
#elif defined(IMGUI_IMPL_OPENGL_LOADER_GLAD2)
#include <glad/gl.h>
#elif defined(IMGUI_IMPL_OPENGL_LOADER_GLBINDING2)
#define GLFW_INCLUDE_NONE
#include <glbinding/Binding.h>
#include <glbinding/gl/gl.h>
using namespace gl;
#elif defined(IMGUI_IMPL_OPENGL_LOADER_GLBINDING3)
#define GLFW_INCLUDE_NONE
#include <glbinding/glbinding.h>
#include <glbinding/gl/gl.h>
using namespace gl;
#else
#include <GL/gl.h>
#endif

namespace {
volatile std::sig_atomic_t g_running = 1;

void signal_handler(int signal) {
    g_running = 0;
}

// Log capture mechanism
class LogCapture {
public:
    static LogCapture& instance() {
        static LogCapture inst;
        return inst;
    }

    void addLog(const std::string& log) {
        std::lock_guard<std::mutex> lock(mutex_);
        logs_.push_back(log);
        
        // Keep only last 10000 lines using deque's efficient front removal
        if (logs_.size() > 10000) {
            logs_.erase(logs_.begin(), logs_.begin() + 5000);
        }
        
        // Auto-scroll if needed
        if (auto_scroll_) {
            scroll_to_bottom_ = true;
        }
    }

    std::vector<std::string> getLogs() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::vector<std::string>(logs_.begin(), logs_.end());
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        logs_.clear();
    }

    void setAutoScroll(bool auto_scroll) {
        auto_scroll_ = auto_scroll;
    }

    bool shouldScrollToBottom() {
        if (scroll_to_bottom_) {
            scroll_to_bottom_ = false;
            return true;
        }
        return false;
    }

private:
    mutable std::mutex mutex_;
    std::deque<std::string> logs_;  // Use deque for efficient front removal
    bool auto_scroll_ = true;
    bool scroll_to_bottom_ = false;
};

// Custom ostream to capture logs
class LogStreamBuf : public std::streambuf {
public:
    LogStreamBuf(std::streambuf* original, const std::string& prefix) 
        : original_(original), prefix_(prefix) {}

protected:
    int overflow(int c) override {
        if (c != EOF) {
            buffer_ += static_cast<char>(c);
            if (c == '\n') {
                LogCapture::instance().addLog(prefix_ + buffer_);
                buffer_.clear();
            }
        }
        return original_ ? original_->sputc(c) : c;
    }

private:
    std::streambuf* original_;
    std::string prefix_;
    std::string buffer_;
};

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
    std::string symbol = "BTCUSDT";
    if (argc > 1) {
        symbol = argv[1];
    }
    
    // Transform to uppercase
    std::transform(symbol.begin(), symbol.end(), symbol.begin(), ::toupper);
    
    // Set up signal handler
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    
    // Capture logs from stderr
    LogStreamBuf cerr_buf(std::cerr.rdbuf(), "");
    std::cerr.rdbuf(&cerr_buf);
    
    // Initialize GLFW
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW" << std::endl;
        return 1;
    }
    
    // GLFW window setup
    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    
    GLFWwindow* window = glfwCreateWindow(1600, 900, 
                                         (std::string("OrderBook Viewer - ") + symbol).c_str(),
                                         nullptr, nullptr);
    if (!window) {
        std::cerr << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return 1;
    }
    
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // Enable vsync
    
    // Initialize ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    
    // Dark theme
    ImGui::StyleColorsDark();
    
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);
    
    // Load credentials
    load_env_file(".env");
    auto credentials = load_credentials_from_env();
    
    // Orderbook manager setup (in background thread)
    std::shared_ptr<strategy::OrderBookManager> ob_manager;
    strategy::OrderBookSnapshot latest_snapshot;
    std::mutex snapshot_mutex;
    bool connected = false;
    bool connecting = false;
    std::thread connection_thread;
    
    std::string status_message = "Initializing...";
    
    // Start connection in background
    connection_thread = std::thread([&]() {
        try {
            connecting = true;
            status_message = "Creating clients...";
            
            // Create REST client for initial snapshot
            mexc::SpotClient rest_client(credentials);
            
            // Create WebSocket client
            mexc::WsSpotClient ws_client(credentials);
            
            // Create orderbook manager (use shared_ptr for thread safety)
            ob_manager = std::make_shared<strategy::OrderBookManager>(symbol);
            
            status_message = "Connecting to WebSocket...";
            
            // Connect WebSocket
            if (!ws_client.connect()) {
                status_message = "Failed to connect to WebSocket";
                connecting = false;
                return;
            }
            
            // Wait for connection
            int wait_count = 0;
            while (!ws_client.is_connected() && wait_count < 50 && g_running) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                wait_count++;
            }
            
            if (!ws_client.is_connected()) {
                status_message = "Connection timeout";
                connecting = false;
                return;
            }
            
            status_message = "Subscribing to depth stream...";
            
            // Subscribe to depth stream
            if (!ob_manager->subscribe(ws_client, &rest_client)) {
                status_message = "Failed to subscribe";
                connecting = false;
                return;
            }
            
            connected = true;
            connecting = false;
            status_message = "Connected!";
            
            // Set up update callback - use provided snapshot directly to minimize latency
            // The snapshot is already computed in handle_depth_message, so we don't need to re-fetch it
            // Use weak_ptr to avoid use-after-free if ob_manager is deleted
            std::weak_ptr<strategy::OrderBookManager> ob_manager_weak = ob_manager;
            ob_manager->set_update_callback([&, ob_manager_weak](const strategy::OrderBookSnapshot& snapshot) {
                // Use provided snapshot directly - it's already computed and safe to use
                // This avoids lock contention and reduces latency significantly
                std::lock_guard<std::mutex> lock(snapshot_mutex);
                latest_snapshot = snapshot;
            });
            
            // Keep running
            while (g_running && connected) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            
            // Cleanup
            if (ob_manager) {
                ob_manager->unsubscribe(ws_client);
            }
            ws_client.disconnect();
            
        } catch (const std::exception& e) {
            status_message = std::string("Error: ") + e.what();
            connecting = false;
        }
    });
    
    // Main GUI loop
    while (!glfwWindowShouldClose(window) && g_running) {
        glfwPollEvents();
        
        // Start ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        
        // Menu bar
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("Options")) {
                if (ImGui::MenuItem("Clear Logs")) {
                    LogCapture::instance().clear();
                }
                bool auto_scroll = true;
                if (ImGui::MenuItem("Auto-scroll Logs", nullptr, &auto_scroll)) {
                    LogCapture::instance().setAutoScroll(auto_scroll);
                }
                ImGui::EndMenu();
            }
            ImGui::Text("Status: %s", status_message.c_str());
            if (connected) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "●");
            } else if (connecting) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "●");
            } else {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "●");
            }
            ImGui::EndMainMenuBar();
        }
        
        // Split view using child windows
        ImVec2 window_size = ImGui::GetIO().DisplaySize;
        float menu_bar_height = ImGui::GetFrameHeight();
        ImVec2 work_pos = ImVec2(0, menu_bar_height);
        ImVec2 work_size = ImVec2(window_size.x, window_size.y - menu_bar_height);
        
        // Split view: Logs (left) and OrderBook (right)
        float split_ratio = 0.4f; // 40% for logs, 60% for orderbook
        float logs_width = work_size.x * split_ratio;
        
        // Logs window (left side)
        ImGui::SetNextWindowPos(work_pos);
        ImGui::SetNextWindowSize(ImVec2(logs_width, work_size.y));
        ImGui::Begin("Logs", nullptr, 
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | 
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
        
        auto logs = LogCapture::instance().getLogs();
        if (ImGui::BeginChild("LogScrolling", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar)) {
            for (const auto& log : logs) {
                // Color code by log type
                ImVec4 color(1.0f, 1.0f, 1.0f, 1.0f);
                if (log.find("[DEBUG]") != std::string::npos) {
                    color = ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
                } else if (log.find("WARNING") != std::string::npos) {
                    color = ImVec4(1.0f, 1.0f, 0.0f, 1.0f);
                } else if (log.find("ERROR") != std::string::npos || log.find("Error") != std::string::npos) {
                    color = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
                }
                ImGui::TextColored(color, "%s", log.c_str());
            }
            
            if (LogCapture::instance().shouldScrollToBottom()) {
                ImGui::SetScrollHereY(1.0f);
            }
        }
        ImGui::EndChild();
        ImGui::End();
        
        // Orderbook window (right side)
        ImGui::SetNextWindowPos(ImVec2(work_pos.x + logs_width, work_pos.y));
        ImGui::SetNextWindowSize(ImVec2(work_size.x - logs_width, work_size.y));
        ImGui::Begin("OrderBook", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | 
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
        
        strategy::OrderBookSnapshot current_snapshot;
        static auto last_snapshot_refresh = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        
        // Periodically refresh snapshot from orderbook directly (every 100ms)
        // This ensures we always have fresh data even if callbacks stop
        // Use shared_ptr to safely check if ob_manager still exists
        if (connected && ob_manager && 
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_snapshot_refresh).count() >= 100) {
            try {
                // Lock ob_manager to ensure it's not deleted during access
                auto ob_mgr = ob_manager; // Copy shared_ptr to extend lifetime
                std::lock_guard<std::mutex> lock(snapshot_mutex);
                current_snapshot = ob_mgr->get_orderbook().get_snapshot(20, true);
                latest_snapshot = current_snapshot; // Update latest snapshot
                last_snapshot_refresh = now;
            } catch (...) {
                // Fallback to cached snapshot
                std::lock_guard<std::mutex> lock(snapshot_mutex);
                current_snapshot = latest_snapshot;
            }
        } else {
            // Use cached snapshot
            std::lock_guard<std::mutex> lock(snapshot_mutex);
            current_snapshot = latest_snapshot;
        }
        
        // Get orderbook validity status (safely with shared_ptr)
        bool orderbook_valid = false;
        if (connected && ob_manager) {
            auto ob_mgr = ob_manager; // Copy shared_ptr to extend lifetime
            orderbook_valid = ob_mgr->get_orderbook().is_valid();
        }
        
        // Show warning if orderbook is invalid
        if (connected && ob_manager) {
            auto ob_mgr = ob_manager; // Copy shared_ptr to extend lifetime
            if (!orderbook_valid) {
                ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "WARNING: Orderbook is invalid!");
                ImGui::Text("Last Update ID: %lld", ob_mgr->get_orderbook().last_update_id());
                double best_bid = ob_mgr->get_orderbook().best_bid();
                double best_ask = ob_mgr->get_orderbook().best_ask();
                ImGui::Text("Best Bid: %.4f, Best Ask: %.4f", best_bid, best_ask);
                if (best_bid > 0 && best_ask > 0) {
                    ImGui::Text("Spread: %.4f (INVALID: bid >= ask)", best_ask - best_bid);
                }
                ImGui::Separator();
            }
        }
        
        if (connected && current_snapshot.best_bid > 0 && current_snapshot.best_ask > 0) {
            // Header with symbol and stats
            ImGui::Text("Symbol: %s", symbol.c_str());
            ImGui::Separator();
            
            // Best bid/ask and spread
            ImGui::Columns(2, "Stats", false);
            
            ImGui::Text("Best Bid:");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "%.4f", current_snapshot.best_bid);
            
            ImGui::NextColumn();
            
            ImGui::Text("Best Ask:");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "%.4f", current_snapshot.best_ask);
            
            ImGui::Columns(1);
            
            double spread = current_snapshot.spread;
            double spread_bps = (spread / current_snapshot.best_bid) * 10000.0;
            
            ImGui::Separator();
            ImGui::Text("Spread: %.4f (%.4f bps)", spread, spread_bps);
            ImGui::Text("Microprice: %.4f", current_snapshot.microprice);
            ImGui::Text("Bid Volume (10 levels): %.2f", current_snapshot.bid_volume);
            ImGui::Text("Ask Volume (10 levels): %.2f", current_snapshot.ask_volume);
            ImGui::Text("Last Update ID: %lld", current_snapshot.last_update_id);
            
            if (ob_manager) {
                auto ob_mgr = ob_manager; // Copy shared_ptr to extend lifetime
                ImGui::Separator();
                ImGui::Text("Latency:");
                auto stats = ob_mgr->get_latency_tracker().format_stats();
                ImGui::Text("%s", stats.c_str());
            }
            
            ImGui::Separator();
            
            // Orderbook table - reverse asks to show highest first
            if (ImGui::BeginTable("OrderBook", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, -1))) {
                ImGui::TableSetupColumn("Ask Price", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                ImGui::TableSetupColumn("Ask Qty", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                ImGui::TableSetupColumn("Ask Vol", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                ImGui::TableSetupColumn("Bid Price", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                ImGui::TableSetupColumn("Bid Qty", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                ImGui::TableSetupColumn("Bid Vol", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();
                
                // Display asks and bids side by side
                size_t max_rows = std::max(current_snapshot.asks.size(), current_snapshot.bids.size());
                for (size_t i = 0; i < max_rows; ++i) {
                    ImGui::TableNextRow();
                    
                    // Ask side (reverse order - highest first)
                    size_t ask_idx = current_snapshot.asks.size() > 0 && i < current_snapshot.asks.size() 
                        ? (current_snapshot.asks.size() - 1 - i) : i;
                    
                    ImGui::TableSetColumnIndex(0);
                    if (i < current_snapshot.asks.size()) {
                        const auto& ask = current_snapshot.asks[ask_idx];
                        bool is_best = (std::fabs(ask.price - current_snapshot.best_ask) < 1e-6);
                        if (is_best) {
                            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "%.4f", ask.price);
                        } else {
                            ImGui::Text("%.4f", ask.price);
                        }
                    }
                    
                    ImGui::TableSetColumnIndex(1);
                    if (i < current_snapshot.asks.size()) {
                        ImGui::Text("%.4f", current_snapshot.asks[ask_idx].quantity);
                    }
                    
                    ImGui::TableSetColumnIndex(2);
                    if (i < current_snapshot.asks.size()) {
                        double vol = current_snapshot.asks[ask_idx].price * current_snapshot.asks[ask_idx].quantity;
                        ImGui::Text("%.2f", vol);
                    }
                    
                    // Bid side
                    ImGui::TableSetColumnIndex(3);
                    if (i < current_snapshot.bids.size()) {
                        const auto& bid = current_snapshot.bids[i];
                        bool is_best = (std::fabs(bid.price - current_snapshot.best_bid) < 1e-6);
                        if (is_best) {
                            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "%.4f", bid.price);
                        } else {
                            ImGui::Text("%.4f", bid.price);
                        }
                    }
                    
                    ImGui::TableSetColumnIndex(4);
                    if (i < current_snapshot.bids.size()) {
                        ImGui::Text("%.4f", current_snapshot.bids[i].quantity);
                    }
                    
                    ImGui::TableSetColumnIndex(5);
                    if (i < current_snapshot.bids.size()) {
                        double vol = current_snapshot.bids[i].price * current_snapshot.bids[i].quantity;
                        ImGui::Text("%.2f", vol);
                    }
                }
                
                ImGui::EndTable();
            }
            
        } else {
            ImGui::Text("Waiting for orderbook data...");
            if (connecting) {
                ImGui::Text("Connecting...");
            }
        }
        
        ImGui::End();
        
        // Rendering
        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        
        glfwSwapBuffers(window);
        
        std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60 FPS
    }
    
    // Cleanup: stop running first, then clear callback, then join thread, then clear ob_manager
    g_running = 0;
    
    // Clear callback FIRST to prevent new callbacks from being invoked
    // This must happen before joining the thread to avoid race conditions
    if (ob_manager) {
        ob_manager->set_update_callback(nullptr);
    }
    
    // Wait for connection thread to finish (this ensures any in-flight callbacks complete)
    if (connection_thread.joinable()) {
        connection_thread.join();
    }
    
    // Now safe to reset ob_manager - all callbacks are cleared and thread is joined
    ob_manager.reset(); // Release shared_ptr (will delete if last reference)
    
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    
    glfwDestroyWindow(window);
    glfwTerminate();
    
    return 0;
}
