#pragma once

#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <queue>
#include <vector>

namespace mexc {

enum class WsConnectionState {
    Disconnected,
    Connecting,
    Connected,
    Reconnecting
};

// Callback types for WebSocket events
using WsMessageCallback = std::function<void(const std::string& message)>;
using WsBinaryCallback = std::function<void(const std::vector<uint8_t>& data)>;
using WsErrorCallback = std::function<void(const std::string& error)>;
using WsStateCallback = std::function<void(WsConnectionState state)>;

class WsClient {
public:
    explicit WsClient(const std::string& url);
    ~WsClient();

    WsClient(const WsClient&) = delete;
    WsClient& operator=(const WsClient&) = delete;
    WsClient(WsClient&&) noexcept = delete;
    WsClient& operator=(WsClient&&) noexcept = delete;

    // Connection management
    bool connect();
    void disconnect();
    bool send(const std::string& message);
    bool is_connected() const noexcept;

    // Callbacks
    void set_message_callback(WsMessageCallback callback);
    void set_binary_callback(WsBinaryCallback callback);
    void set_error_callback(WsErrorCallback callback);
    void set_state_callback(WsStateCallback callback);

    // Configuration
    void set_auto_reconnect(bool enable, int max_reconnect_attempts = -1);
    void set_reconnect_delay_ms(int delay_ms);
    void set_heartbeat_interval_ms(int interval_ms);

    WsConnectionState state() const noexcept;

    // Public for callback access (implementation detail)
    struct Impl;
    
private:
    std::unique_ptr<Impl> pimpl_;
};

} // namespace mexc

