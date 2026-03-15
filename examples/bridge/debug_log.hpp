#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "pico/stdlib.h"

namespace debug_log {

enum class Port : uint8_t { Pad, Console };
enum class Dir : uint8_t { TX, RX };

struct LogEntry {
    uint32_t timestamp_us;
    Port port;
    Dir dir;
    uint8_t command_byte;
    uint8_t data[16];
    uint8_t data_len;
    uint8_t poll_mode{0};       // コンソールからリクエストされたPollMode
    uint8_t rumble_mode{0};     // コンソールからリクエストされたRumbleMode
    bool    has_poll_mode{false}; // PollMode/RumbleMode情報の有無
    bool is_state;
    bool is_timeout;
    char state_str[48];
};

inline constexpr std::size_t kRingSize = 256;
inline LogEntry g_ring[kRingSize];
inline volatile uint32_t g_ring_head = 0;
inline uint32_t g_ring_tail = 0;
inline uint32_t g_drop_count = 0;

#ifdef GCINPUT_ENABLE_LOG

inline void ring_push(LogEntry entry) {
    uint32_t next = (g_ring_head + 1) % kRingSize;
    if (next == g_ring_tail) {
        g_drop_count++;
        return;
    }
    g_ring[g_ring_head] = entry;
    g_ring_head = next;
}

// ISR安全: データログをリングバッファに積む
inline void ring_push_data(Port port, Dir dir, uint8_t cmd, const uint8_t *data, std::size_t len) {
    LogEntry e{};
    e.timestamp_us = time_us_32();
    e.port = port;
    e.dir = dir;
    e.command_byte = cmd;
    e.data_len = static_cast<uint8_t>(len > 16 ? 16 : len);
    if (data && e.data_len > 0) {
        memcpy(e.data, data, e.data_len);
    }
    e.is_state = false;
    e.is_timeout = false;
    e.has_poll_mode = false;
    ring_push(e);
}

// ISR安全: PollMode/RumbleMode情報付きデータログをリングバッファに積む
inline void ring_push_data_with_poll_mode(Port port, Dir dir, uint8_t cmd,
                                          const uint8_t *data, std::size_t len,
                                          uint8_t pm, uint8_t rm) {
    LogEntry e{};
    e.timestamp_us = time_us_32();
    e.port = port;
    e.dir = dir;
    e.command_byte = cmd;
    e.data_len = static_cast<uint8_t>(len > 16 ? 16 : len);
    if (data && e.data_len > 0) {
        memcpy(e.data, data, e.data_len);
    }
    e.is_state = false;
    e.is_timeout = false;
    e.has_poll_mode = true;
    e.poll_mode = pm;
    e.rumble_mode = rm;
    ring_push(e);
}

// main安全: 状態遷移ログをリングバッファに積む
inline void ring_push_state(Port port, const char *msg) {
    LogEntry e{};
    e.timestamp_us = time_us_32();
    e.port = port;
    e.is_state = true;
    e.is_timeout = false;
    strncpy(e.state_str, msg, sizeof(e.state_str) - 1);
    e.state_str[sizeof(e.state_str) - 1] = '\0';
    ring_push(e);
}

// main安全: タイムアウトログをリングバッファに積む
inline void ring_push_timeout(Port port, const char *msg) {
    LogEntry e{};
    e.timestamp_us = time_us_32();
    e.port = port;
    e.is_state = false;
    e.is_timeout = true;
    strncpy(e.state_str, msg, sizeof(e.state_str) - 1);
    e.state_str[sizeof(e.state_str) - 1] = '\0';
    ring_push(e);
}

#else  // GCINPUT_ENABLE_LOG 未定義時はno-op

inline void ring_push(LogEntry) {}
inline void ring_push_data(Port, Dir, uint8_t, const uint8_t *, std::size_t) {}
inline void ring_push_data_with_poll_mode(Port, Dir, uint8_t, const uint8_t *, std::size_t, uint8_t, uint8_t) {}
inline void ring_push_state(Port, const char *) {}
inline void ring_push_timeout(Port, const char *) {}

#endif  // GCINPUT_ENABLE_LOG

inline const char *cmd_name(uint8_t cmd) {
    switch (cmd) {
    case 0x00: return "Id";
    case 0x40: return "Status";
    case 0x41: return "Origin";
    case 0x42: return "Recalibrate";
    case 0xFF: return "Reset";
    default:   return "Unknown";
    }
}

} // namespace debug_log
