# WebSocket Keepalive Fix

## Issues Found

1. **No actual ping frames being sent**: The heartbeat mechanism was only calling `lws_callback_on_writable()`, which doesn't send ping frames
2. **No connection monitoring**: The system doesn't detect when messages stop arriving
3. **Silent connection death**: The `connected` flag stays true even if the connection is dead

## Fixes Applied

### 1. Proper Ping Frame Sending (`ws_client.cpp`)

**Before**: Only marked connection as writable (didn't actually send ping)
```cpp
lws_callback_on_writable(pimpl_->wsi);  // Doesn't send ping!
```

**After**: Actually sends WebSocket ping frames
```cpp
unsigned char ping_frame[LWS_PRE + 0];
int ret = lws_write(pimpl_->wsi, ping_frame + LWS_PRE, 0, LWS_WRITE_PING);
```

**Impact**: Keeps connection alive by sending ping frames every 20 seconds

### 2. Connection Activity Tracking (`ws_client.cpp`)

**Added**: Update `last_ping_time` on any message receive
```cpp
case LWS_CALLBACK_CLIENT_RECEIVE: {
    // Update last ping time on any receive to track connection activity
    impl->last_ping_time = std::chrono::steady_clock::now();
    // ... rest of receive handling
}
```

**Impact**: Tracks when last message was received to detect dead connections

### 3. Connection Timeout Detection (`ws_client.cpp`)

**Added**: Automatic detection of dead connections
- If no data received for 90 seconds (3x heartbeat interval), mark connection as dead
- Triggers error callback to notify upper layers
- Allows reconnection logic to kick in

**Impact**: Detects and handles dead connections automatically

### 4. Heartbeat Configuration

- **Heartbeat interval**: 20 seconds (send ping every 20s)
- **Connection timeout**: 90 seconds (3x heartbeat)
- **Default**: Previously was 30 seconds, now optimized for better reliability

## How It Works

1. **Every 20 seconds**: Worker thread sends a WebSocket ping frame
2. **On every message**: `last_ping_time` is updated (tracks activity)
3. **Every loop iteration**: Check if timeout exceeded, mark connection dead if so
4. **Error callback**: Notifies application layer about dead connection

## Testing

To verify keepalive is working:

1. Monitor logs for ping frames being sent
2. Check that connection stays alive during idle periods
3. Verify that dead connections are detected and handled
4. Confirm reconnection works properly

## Notes

- libwebsockets handles pong responses automatically
- Ping frames with empty payload are valid per WebSocket spec
- The connection timeout (90s) is generous to avoid false positives
- Actual message reception (updates) resets the timeout timer

