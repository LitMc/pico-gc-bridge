#pragma once
#include "domain/state.hpp"

namespace gcinput::domain::transform::builtins {
// アナログ入力をすべて正確なニュートラルポジションに固定
inline void fix_origin_to_neutral(void *, domain::PadState &state) {
    auto &analog = state.input.analog;
    analog.stick_x = domain::AnalogInput::kAxisCenter;
    analog.stick_y = domain::AnalogInput::kAxisCenter;
    analog.c_stick_x = domain::AnalogInput::kAxisCenter;
    analog.c_stick_y = domain::AnalogInput::kAxisCenter;
    analog.l_analog = domain::AnalogInput::kTriggerReleased;
    analog.r_analog = domain::AnalogInput::kTriggerReleased;
}

} // namespace gcinput::domain::transform::builtins
