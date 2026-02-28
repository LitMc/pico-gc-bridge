#pragma once
#include <cstdint>

namespace gcinput::domain {
enum class PollMode : uint8_t { Mode0 = 0, Mode1 = 1, Mode2 = 2, Mode3 = 3, Mode4 = 4 };
enum class RumbleMode : uint8_t { Off = 0, On = 1, Brake = 2 };
} // namespace gcinput::domain
