#include "mexc/ws_client.hpp"

#include <libwebsockets.h>
#include <openssl/ssl.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <thread>

namespace mexc {

// Define the Impl structure (alias for WsClient::Impl)
using WsClientImpl = WsClient::Impl;

// Define the Impl structure
struct WsClient::Impl {
    std::string url;
    ::lws_context* context = nullptr;
    ::lws* wsi = nullptr;
    std::thread worker_thread;
    std::atomic<bool> should_stop{false};
    std::atomic<bool> connected{false};
    std::atomic<WsConnectionState> state{WsConnectionState::Disconnected};
    
    WsMessageCallback message_callback;
    WsBinaryCallback binary_callback;
    WsErrorCallback error_callback;
    WsStateCallback state_callback;
    
    std::mutex callbacks_mutex;
    std::mutex send_mutex;
    std::queue<std::string> send_queue;
    std::string message_buffer; // For fragmented text messages
    std::vector<uint8_t> binary_buffer; // For fragmented binary messages
    
    bool auto_reconnect = true;
    int max_reconnect_attempts = -1; // -1 means infinite
    int reconnect_attempts = 0;
    int reconnect_delay_ms = 1000;
    int heartbeat_interval_ms = 30000;
    
    std::chrono::steady_clock::time_point last_ping_time;
    std::condition_variable cv;
    std::mutex cv_mutex;
};

} // namespace mexc

namespace {

// WebSocket protocol callbacks
int callback_ws_client(struct lws* wsi, enum lws_callback_reasons reason,
                       void* user, void* in, size_t len) {
    auto* impl = static_cast<mexc::WsClient::Impl*>(lws_get_opaque_user_data(wsi));
    if (!impl) {
        // Try to get from context user data
        impl = static_cast<mexc::WsClient::Impl*>(lws_context_user(lws_get_context(wsi)));
        if (impl) {
            lws_set_opaque_user_data(wsi, impl);
        }
        if (!impl) {
            return 0;
        }
    }

    switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED: {
            impl->connected = true;
            impl->state = mexc::WsConnectionState::Connected;
            impl->reconnect_attempts = 0;
            impl->last_ping_time = std::chrono::steady_clock::now();
            
            {
                std::lock_guard<std::mutex> lock(impl->callbacks_mutex);
                if (impl->state_callback) {
                    impl->state_callback(mexc::WsConnectionState::Connected);
                }
            }
            break;
        }

        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR: {
            impl->connected = false;
            impl->state = mexc::WsConnectionState::Disconnected;
            
            {
                std::lock_guard<std::mutex> lock(impl->callbacks_mutex);
                if (impl->error_callback) {
                    const char* error_msg = in ? static_cast<const char*>(in) : "Connection error";
                    impl->error_callback(std::string(error_msg, len));
                }
                if (impl->state_callback) {
                    impl->state_callback(mexc::WsConnectionState::Disconnected);
                }
            }
            break;
        }

        case LWS_CALLBACK_CLOSED: {
            impl->connected = false;
            impl->state = mexc::WsConnectionState::Disconnected;
            
            {
                std::lock_guard<std::mutex> lock(impl->callbacks_mutex);
                if (impl->state_callback) {
                    impl->state_callback(mexc::WsConnectionState::Disconnected);
                }
            }
            break;
        }

        case LWS_CALLBACK_CLIENT_RECEIVE: {
            if (in && len > 0) {
                // Check if this is a binary frame (Protobuf) or text frame (JSON)
                int is_binary = lws_frame_is_binary(wsi);
                
                if (is_binary) {
                    // Binary frame (Protobuf) - accumulate binary data
                    const uint8_t* data = static_cast<const uint8_t*>(in);
                    impl->binary_buffer.insert(impl->binary_buffer.end(), data, data + len);
                    
                    if (lws_is_final_fragment(wsi)) {
                        // Complete binary message received
                        std::lock_guard<std::mutex> lock(impl->callbacks_mutex);
                        if (impl->binary_callback) {
                            impl->binary_callback(impl->binary_buffer);
                        }
                        impl->binary_buffer.clear();
                    }
                } else {
                    // Text frame (JSON) - handle normally
                    impl->message_buffer.append(static_cast<const char*>(in), len);
                    
                    if (lws_is_final_fragment(wsi)) {
                        std::lock_guard<std::mutex> lock(impl->callbacks_mutex);
                        if (impl->message_callback) {
                            impl->message_callback(impl->message_buffer);
                        }
                        impl->message_buffer.clear();
                    }
                }
            }
            break;
        }

        case LWS_CALLBACK_CLIENT_WRITEABLE: {
            std::lock_guard<std::mutex> lock(impl->send_mutex);
            if (!impl->send_queue.empty()) {
                std::string message = impl->send_queue.front();
                impl->send_queue.pop();
                
                unsigned char* buf = new unsigned char[LWS_PRE + message.size()];
                std::memcpy(buf + LWS_PRE, message.data(), message.size());
                
                int n = lws_write(wsi, buf + LWS_PRE, message.size(), LWS_WRITE_TEXT);
                delete[] buf;
                
                if (n < 0) {
                    std::lock_guard<std::mutex> cb_lock(impl->callbacks_mutex);
                    if (impl->error_callback) {
                        impl->error_callback("Failed to send WebSocket message");
                    }
                } else {
                    lws_callback_on_writable(wsi);
                }
            }
            break;
        }

        case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER: {
            // Add custom headers if needed
            break;
        }

        default:
            break;
    }

    return 0;
}

const struct lws_protocols protocols[] = {
    {
        "ws-client",
        callback_ws_client,
        0,
        4096, // rx_buffer_size
    },
    {nullptr, nullptr, 0, 0}
};

} // anonymous namespace

namespace mexc {

WsClient::WsClient(const std::string& url)
    : pimpl_(std::make_unique<Impl>()) {
    pimpl_->url = url;
}

WsClient::~WsClient() {
    disconnect();
}

bool WsClient::connect() {
    if (pimpl_->connected) {
        return true;
    }
    
    // Prevent multiple simultaneous connection attempts
    if (pimpl_->state == WsConnectionState::Connecting || 
        pimpl_->state == WsConnectionState::Reconnecting) {
        return false;
    }

    pimpl_->state = WsConnectionState::Connecting;
    {
        std::lock_guard<std::mutex> lock(pimpl_->callbacks_mutex);
        if (pimpl_->state_callback) {
            pimpl_->state_callback(WsConnectionState::Connecting);
        }
    }

    struct lws_context_creation_info info;
    std::memset(&info, 0, sizeof(info));
    
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;
    info.gid = -1;
    info.uid = -1;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    info.user = pimpl_.get(); // Store impl in context user data

    pimpl_->context = lws_create_context(&info);
    if (!pimpl_->context) {
        pimpl_->state = WsConnectionState::Disconnected;
        return false;
    }

    // Parse URL
    std::string host, path;
    int port;
    bool ssl = false;
    
    if (pimpl_->url.find("wss://") == 0) {
        ssl = true;
        size_t start = 6; // "wss://"
        size_t slash = pimpl_->url.find('/', start);
        if (slash == std::string::npos) {
            host = pimpl_->url.substr(start);
            path = "/";
        } else {
            host = pimpl_->url.substr(start, slash - start);
            path = pimpl_->url.substr(slash);
        }
        
        size_t colon = host.find(':');
        if (colon != std::string::npos) {
            port = std::stoi(host.substr(colon + 1));
            host = host.substr(0, colon);
        } else {
            port = 443;
        }
    } else if (pimpl_->url.find("ws://") == 0) {
        size_t start = 5; // "ws://"
        size_t slash = pimpl_->url.find('/', start);
        if (slash == std::string::npos) {
            host = pimpl_->url.substr(start);
            path = "/";
        } else {
            host = pimpl_->url.substr(start, slash - start);
            path = pimpl_->url.substr(slash);
        }
        
        size_t colon = host.find(':');
        if (colon != std::string::npos) {
            port = std::stoi(host.substr(colon + 1));
            host = host.substr(0, colon);
        } else {
            port = 80;
        }
    } else {
        pimpl_->state = WsConnectionState::Disconnected;
        return false;
    }

    struct lws_client_connect_info ccinfo;
    std::memset(&ccinfo, 0, sizeof(ccinfo));
    ccinfo.context = pimpl_->context;
    ccinfo.address = host.c_str();
    ccinfo.port = port;
    ccinfo.path = path.c_str();
    ccinfo.host = host.c_str();
    ccinfo.origin = host.c_str();
    ccinfo.protocol = protocols[0].name;
    ccinfo.ssl_connection = ssl ? LCCSCF_USE_SSL : 0;

    pimpl_->wsi = lws_client_connect_via_info(&ccinfo);
    if (!pimpl_->wsi) {
        pimpl_->state = WsConnectionState::Disconnected;
        return false;
    }
    
    // Store impl pointer in wsi for callbacks
    lws_set_opaque_user_data(pimpl_->wsi, pimpl_.get());

    // Start service thread
    pimpl_->should_stop = false;
    pimpl_->worker_thread = std::thread([this]() {
        while (!pimpl_->should_stop) {
            if (pimpl_->context) {
                lws_service(pimpl_->context, 0);
            }
            
            // Handle heartbeat
            auto now = std::chrono::steady_clock::now();
            if (pimpl_->connected && 
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - pimpl_->last_ping_time).count() >= pimpl_->heartbeat_interval_ms) {
                if (pimpl_->wsi) {
                    lws_callback_on_writable(pimpl_->wsi);
                }
                pimpl_->last_ping_time = now;
            }
            
            // Handle reconnection (don't call connect() recursively - just set flag)
            // The reconnection will be handled by checking connection state externally
            // or by destroying and recreating the connection
            
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    return true;
}

void WsClient::disconnect() {
    pimpl_->should_stop = true;
    pimpl_->auto_reconnect = false;
    
    // Stop service loop first
    if (pimpl_->context) {
        lws_cancel_service(pimpl_->context);
    }
    
    // Wait for worker thread to finish
    if (pimpl_->worker_thread.joinable()) {
        pimpl_->worker_thread.join();
    }

    if (pimpl_->wsi) {
        lws_set_timeout(pimpl_->wsi, PENDING_TIMEOUT_CLOSE_SEND, 0);
    }

    if (pimpl_->context) {
        lws_context_destroy(pimpl_->context);
        pimpl_->context = nullptr;
    }

    pimpl_->wsi = nullptr;
    pimpl_->connected = false;
    pimpl_->state = WsConnectionState::Disconnected;
}

bool WsClient::send(const std::string& message) {
    if (!pimpl_->connected || !pimpl_->wsi) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(pimpl_->send_mutex);
        pimpl_->send_queue.push(message);
    }

    lws_callback_on_writable(pimpl_->wsi);
    return true;
}

bool WsClient::is_connected() const noexcept {
    return pimpl_->connected;
}

void WsClient::set_message_callback(WsMessageCallback callback) {
    std::lock_guard<std::mutex> lock(pimpl_->callbacks_mutex);
    pimpl_->message_callback = std::move(callback);
}

void WsClient::set_binary_callback(WsBinaryCallback callback) {
    std::lock_guard<std::mutex> lock(pimpl_->callbacks_mutex);
    pimpl_->binary_callback = std::move(callback);
}

void WsClient::set_error_callback(WsErrorCallback callback) {
    std::lock_guard<std::mutex> lock(pimpl_->callbacks_mutex);
    pimpl_->error_callback = std::move(callback);
}

void WsClient::set_state_callback(WsStateCallback callback) {
    std::lock_guard<std::mutex> lock(pimpl_->callbacks_mutex);
    pimpl_->state_callback = std::move(callback);
}

void WsClient::set_auto_reconnect(bool enable, int max_reconnect_attempts) {
    pimpl_->auto_reconnect = enable;
    pimpl_->max_reconnect_attempts = max_reconnect_attempts;
}

void WsClient::set_reconnect_delay_ms(int delay_ms) {
    pimpl_->reconnect_delay_ms = delay_ms;
}

void WsClient::set_heartbeat_interval_ms(int interval_ms) {
    pimpl_->heartbeat_interval_ms = interval_ms;
}

WsConnectionState WsClient::state() const noexcept {
    return pimpl_->state;
}

} // namespace mexc

