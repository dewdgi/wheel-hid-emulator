#ifndef WHEEL_INPUT_H
#define WHEEL_INPUT_H

#include <array>
#include <chrono>
#include <cstdint>

#include "../wheel_types.h"

struct WheelInputState {
    bool throttle = false;
    bool brake = false;
    bool clutch = false;
    int8_t dpad_x = 0;
    int8_t dpad_y = 0;
    std::array<uint8_t, static_cast<size_t>(WheelButton::Count)> buttons{};
};

struct InputFrame {
    WheelInputState logical;
    int mouse_dx = 0;
    std::chrono::steady_clock::time_point timestamp;
    bool toggle_pressed = false;
};

#endif  // WHEEL_INPUT_H
