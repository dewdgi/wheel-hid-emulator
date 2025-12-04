#ifndef WHEEL_TYPES_H
#define WHEEL_TYPES_H

#include <cstdint>

enum class WheelButton : uint8_t {
    South = 0,
    East,
    West,
    North,
    TL,
    TR,
    TL2,
    TR2,
    Select,
    Start,
    ThumbL,
    ThumbR,
    Mode,
    Dead,
    TriggerHappy1,
    TriggerHappy2,
    TriggerHappy3,
    TriggerHappy4,
    TriggerHappy5,
    TriggerHappy6,
    TriggerHappy7,
    TriggerHappy8,
    TriggerHappy9,
    TriggerHappy10,
    TriggerHappy11,
    TriggerHappy12,
    Count
};

#endif  // WHEEL_TYPES_H
