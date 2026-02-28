#pragma once
#include "joybus/protocol/protocol.hpp"
#include <algorithm>
#include <array>
#include <span>

namespace gcinput {

class JoybusReply {
  public:
    JoybusReply() = default;
    template <std::size_t N>
    JoybusReply(joybus::Command cmd, const std::array<uint8_t, N> &src)
        : command_{cmd}, length_{N} {
        static_assert(N <= joybus::kMaxResponseSize);
        std::copy_n(src.data(), N, bytes_.begin());
    }

    JoybusReply(joybus::Command cmd, std::span<const uint8_t> src)
        : command_{cmd}, length_{static_cast<uint8_t>(
                             std::min<std::size_t>(src.size(), joybus::kMaxResponseSize))} {
        std::copy_n(src.data(), length_, bytes_.begin());
    }

    // 応答のコマンド種別
    joybus::Command command() const { return command_; }

    // 応答内容: modifiable
    std::span<uint8_t> view() { return {bytes_.data(), length_}; }

    // 応答内容: read-only
    std::span<const uint8_t> view() const { return {bytes_.data(), length_}; }

  private:
    joybus::Command command_{joybus::Command::Invalid};
    uint8_t length_{0};
    std::array<uint8_t, joybus::kMaxResponseSize> bytes_{};
};

} // namespace gcinput
