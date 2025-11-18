#include "gamepad.h"
#include "input.h"
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <linux/uinput.h>
#include <linux/input-event-codes.h>

GamepadDevice::GamepadDevice() 
    : fd(-1), steering(0), throttle(0.0f), brake(0.0f), dpad_x(0), dpad_y(0) {
}

GamepadDevice::~GamepadDevice() {
    if (fd >= 0) {
        ioctl(fd, UI_DEV_DESTROY);
        close(fd);
    }
}

bool GamepadDevice::Create() {
    fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        std::cerr << "Failed to open /dev/uinput. Are you running as root?" << std::endl;
        return false;
    }
    
    // Enable event types
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    
    // Enable buttons
    ioctl(fd, UI_SET_KEYBIT, BTN_A);
    ioctl(fd, UI_SET_KEYBIT, BTN_B);
    ioctl(fd, UI_SET_KEYBIT, BTN_X);
    ioctl(fd, UI_SET_KEYBIT, BTN_Y);
    ioctl(fd, UI_SET_KEYBIT, BTN_TL);      // Left bumper
    ioctl(fd, UI_SET_KEYBIT, BTN_TR);      // Right bumper
    ioctl(fd, UI_SET_KEYBIT, BTN_SELECT);
    ioctl(fd, UI_SET_KEYBIT, BTN_START);
    
    // Setup axes
    struct uinput_abs_setup abs_setup;
    
    // Left Stick X (steering)
    memset(&abs_setup, 0, sizeof(abs_setup));
    abs_setup.code = ABS_X;
    abs_setup.absinfo.minimum = -32768;
    abs_setup.absinfo.maximum = 32767;
    abs_setup.absinfo.value = 0;
    ioctl(fd, UI_ABS_SETUP, &abs_setup);
    
    // Left Stick Y (unused, but required for Xbox 360)
    memset(&abs_setup, 0, sizeof(abs_setup));
    abs_setup.code = ABS_Y;
    abs_setup.absinfo.minimum = -32768;
    abs_setup.absinfo.maximum = 32767;
    abs_setup.absinfo.value = 0;
    ioctl(fd, UI_ABS_SETUP, &abs_setup);
    
    // Left Trigger (brake) - for web gamepad API
    memset(&abs_setup, 0, sizeof(abs_setup));
    abs_setup.code = ABS_Z;
    abs_setup.absinfo.minimum = 0;
    abs_setup.absinfo.maximum = 255;
    abs_setup.absinfo.value = 0;
    ioctl(fd, UI_ABS_SETUP, &abs_setup);
    
    // Right Stick X (unused, but required for Xbox 360)
    memset(&abs_setup, 0, sizeof(abs_setup));
    abs_setup.code = ABS_RX;
    abs_setup.absinfo.minimum = -32768;
    abs_setup.absinfo.maximum = 32767;
    abs_setup.absinfo.value = 0;
    ioctl(fd, UI_ABS_SETUP, &abs_setup);
    
    // Right Stick Y (unused, but required for Xbox 360)
    memset(&abs_setup, 0, sizeof(abs_setup));
    abs_setup.code = ABS_RY;
    abs_setup.absinfo.minimum = -32768;
    abs_setup.absinfo.maximum = 32767;
    abs_setup.absinfo.value = 0;
    ioctl(fd, UI_ABS_SETUP, &abs_setup);
    
    // Right Trigger (throttle) - for web gamepad API
    memset(&abs_setup, 0, sizeof(abs_setup));
    abs_setup.code = ABS_RZ;
    abs_setup.absinfo.minimum = 0;
    abs_setup.absinfo.maximum = 255;
    abs_setup.absinfo.value = 0;
    ioctl(fd, UI_ABS_SETUP, &abs_setup);
    
    // Gas pedal (throttle) - for Steam and racing games
    memset(&abs_setup, 0, sizeof(abs_setup));
    abs_setup.code = ABS_GAS;
    abs_setup.absinfo.minimum = 0;
    abs_setup.absinfo.maximum = 255;
    abs_setup.absinfo.value = 0;
    ioctl(fd, UI_ABS_SETUP, &abs_setup);
    
    // Brake pedal (brake) - for Steam and racing games
    memset(&abs_setup, 0, sizeof(abs_setup));
    abs_setup.code = ABS_BRAKE;
    abs_setup.absinfo.minimum = 0;
    abs_setup.absinfo.maximum = 255;
    abs_setup.absinfo.value = 0;
    ioctl(fd, UI_ABS_SETUP, &abs_setup);
    
    // D-Pad X
    memset(&abs_setup, 0, sizeof(abs_setup));
    abs_setup.code = ABS_HAT0X;
    abs_setup.absinfo.minimum = -1;
    abs_setup.absinfo.maximum = 1;
    abs_setup.absinfo.value = 0;
    ioctl(fd, UI_ABS_SETUP, &abs_setup);
    
    // D-Pad Y
    memset(&abs_setup, 0, sizeof(abs_setup));
    abs_setup.code = ABS_HAT0Y;
    abs_setup.absinfo.minimum = -1;
    abs_setup.absinfo.maximum = 1;
    abs_setup.absinfo.value = 0;
    ioctl(fd, UI_ABS_SETUP, &abs_setup);
    
    // Setup device identity
    struct uinput_setup setup;
    memset(&setup, 0, sizeof(setup));
    setup.id.bustype = BUS_USB;
    setup.id.vendor = 0x045e;   // Microsoft
    setup.id.product = 0x028e;  // Xbox 360 Controller
    setup.id.version = 1;
    strcpy(setup.name, "Xbox 360 Controller");
    
    ioctl(fd, UI_DEV_SETUP, &setup);
    ioctl(fd, UI_DEV_CREATE);
    
    std::cout << "Virtual Xbox 360 Controller created" << std::endl;
    return true;
}

void GamepadDevice::UpdateSteering(int delta, int sensitivity) {
    float delta_steering = delta * (800.0f * sensitivity / 100.0f);
    int32_t new_steering = steering + static_cast<int32_t>(delta_steering);
    
    // Clamp to int16_t range
    if (new_steering < -32768) new_steering = -32768;
    if (new_steering > 32767) new_steering = 32767;
    
    steering = static_cast<int16_t>(new_steering);
}

void GamepadDevice::UpdateThrottle(bool pressed) {
    if (pressed) {
        throttle = (throttle + 3.0f > 100.0f) ? 100.0f : throttle + 3.0f;
    } else {
        throttle = (throttle - 3.0f < 0.0f) ? 0.0f : throttle - 3.0f;
    }
}

void GamepadDevice::UpdateBrake(bool pressed) {
    if (pressed) {
        brake = (brake + 3.0f > 100.0f) ? 100.0f : brake + 3.0f;
    } else {
        brake = (brake - 3.0f < 0.0f) ? 0.0f : brake - 3.0f;
    }
}

void GamepadDevice::UpdateButtons(const Input& input) {
    // These will be mapped from config
    buttons["BTN_A"] = input.IsKeyPressed(KEY_Q);
    buttons["BTN_B"] = input.IsKeyPressed(KEY_E);
    buttons["BTN_X"] = input.IsKeyPressed(KEY_F);
    buttons["BTN_Y"] = input.IsKeyPressed(KEY_G);
    buttons["BTN_TL"] = input.IsKeyPressed(KEY_H);
}

void GamepadDevice::UpdateDPad(const Input& input) {
    int right = input.IsKeyPressed(KEY_RIGHT) ? 1 : 0;
    int left = input.IsKeyPressed(KEY_LEFT) ? 1 : 0;
    int down = input.IsKeyPressed(KEY_DOWN) ? 1 : 0;
    int up = input.IsKeyPressed(KEY_UP) ? 1 : 0;
    
    dpad_x = right - left;
    dpad_y = down - up;
}

void GamepadDevice::SendState() {
    if (fd < 0) return;
    
    // Send steering (left stick X)
    EmitEvent(EV_ABS, ABS_X, steering);
    
    // Send left stick Y (unused, always 0)
    EmitEvent(EV_ABS, ABS_Y, 0);
    
    // Send right stick (unused, always 0)
    EmitEvent(EV_ABS, ABS_RX, 0);
    EmitEvent(EV_ABS, ABS_RY, 0);
    
    // Send throttle and brake values
    uint8_t throttle_val = static_cast<uint8_t>(throttle * 2.55f);
    uint8_t brake_val = static_cast<uint8_t>(brake * 2.55f);
    
    // Send as triggers (for web gamepad API)
    EmitEvent(EV_ABS, ABS_RZ, throttle_val);
    EmitEvent(EV_ABS, ABS_Z, brake_val);
    
    // Send as pedals (for Steam and racing games)
    EmitEvent(EV_ABS, ABS_GAS, throttle_val);
    EmitEvent(EV_ABS, ABS_BRAKE, brake_val);
    
    // Send buttons
    EmitEvent(EV_KEY, BTN_A, buttons["BTN_A"] ? 1 : 0);
    EmitEvent(EV_KEY, BTN_B, buttons["BTN_B"] ? 1 : 0);
    EmitEvent(EV_KEY, BTN_X, buttons["BTN_X"] ? 1 : 0);
    EmitEvent(EV_KEY, BTN_Y, buttons["BTN_Y"] ? 1 : 0);
    EmitEvent(EV_KEY, BTN_TL, buttons["BTN_TL"] ? 1 : 0);
    
    // Send D-Pad
    EmitEvent(EV_ABS, ABS_HAT0X, dpad_x);
    EmitEvent(EV_ABS, ABS_HAT0Y, dpad_y);
    
    // Sync
    EmitEvent(EV_SYN, SYN_REPORT, 0);
}

void GamepadDevice::SendNeutral() {
    if (fd < 0) return;
    
    // Zero all stick axes
    EmitEvent(EV_ABS, ABS_X, 0);
    EmitEvent(EV_ABS, ABS_Y, 0);
    EmitEvent(EV_ABS, ABS_RX, 0);
    EmitEvent(EV_ABS, ABS_RY, 0);
    
    // Zero triggers and pedals
    EmitEvent(EV_ABS, ABS_Z, 0);
    EmitEvent(EV_ABS, ABS_RZ, 0);
    EmitEvent(EV_ABS, ABS_GAS, 0);
    EmitEvent(EV_ABS, ABS_BRAKE, 0);
    EmitEvent(EV_KEY, BTN_A, 0);
    EmitEvent(EV_KEY, BTN_B, 0);
    EmitEvent(EV_KEY, BTN_X, 0);
    EmitEvent(EV_KEY, BTN_Y, 0);
    EmitEvent(EV_KEY, BTN_TL, 0);
    EmitEvent(EV_ABS, ABS_HAT0X, 0);
    EmitEvent(EV_ABS, ABS_HAT0Y, 0);
    EmitEvent(EV_SYN, SYN_REPORT, 0);
}

void GamepadDevice::EmitEvent(uint16_t type, uint16_t code, int32_t value) {
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.code = code;
    ev.value = value;
    
    write(fd, &ev, sizeof(ev));
}

int16_t GamepadDevice::ClampSteering(int16_t value) {
    // int16_t is already in range [-32768, 32767], no clamping needed
    return value;
}
