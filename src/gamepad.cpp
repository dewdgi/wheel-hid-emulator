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
    
    // Steering wheel (ABS_X)
    memset(&abs_setup, 0, sizeof(abs_setup));
    abs_setup.code = ABS_X;
    abs_setup.absinfo.minimum = -32768;
    abs_setup.absinfo.maximum = 32767;
    abs_setup.absinfo.value = 0;
    abs_setup.absinfo.fuzz = 0;
    abs_setup.absinfo.flat = 0;
    ioctl(fd, UI_ABS_SETUP, &abs_setup);
    
    // Y axis (unused for G29)
    memset(&abs_setup, 0, sizeof(abs_setup));
    abs_setup.code = ABS_Y;
    abs_setup.absinfo.minimum = -32768;
    abs_setup.absinfo.maximum = 32767;
    abs_setup.absinfo.value = 0;
    abs_setup.absinfo.fuzz = 0;
    abs_setup.absinfo.flat = 0;
    ioctl(fd, UI_ABS_SETUP, &abs_setup);
    
    // Brake pedal (ABS_Z - left trigger)
    memset(&abs_setup, 0, sizeof(abs_setup));
    abs_setup.code = ABS_Z;
    abs_setup.absinfo.minimum = 0;
    abs_setup.absinfo.maximum = 255;
    abs_setup.absinfo.value = 0;
    abs_setup.absinfo.fuzz = 0;
    abs_setup.absinfo.flat = 0;
    ioctl(fd, UI_ABS_SETUP, &abs_setup);
    
    // RX axis (unused for G29)
    memset(&abs_setup, 0, sizeof(abs_setup));
    abs_setup.code = ABS_RX;
    abs_setup.absinfo.minimum = -32768;
    abs_setup.absinfo.maximum = 32767;
    abs_setup.absinfo.value = 0;
    abs_setup.absinfo.fuzz = 0;
    abs_setup.absinfo.flat = 0;
    ioctl(fd, UI_ABS_SETUP, &abs_setup);
    
    // RY axis (unused for G29)
    memset(&abs_setup, 0, sizeof(abs_setup));
    abs_setup.code = ABS_RY;
    abs_setup.absinfo.minimum = -32768;
    abs_setup.absinfo.maximum = 32767;
    abs_setup.absinfo.value = 0;
    abs_setup.absinfo.fuzz = 0;
    abs_setup.absinfo.flat = 0;
    ioctl(fd, UI_ABS_SETUP, &abs_setup);
    
    // Throttle pedal (ABS_RZ - right trigger)
    memset(&abs_setup, 0, sizeof(abs_setup));
    abs_setup.code = ABS_RZ;
    abs_setup.absinfo.minimum = 0;
    abs_setup.absinfo.maximum = 255;
    abs_setup.absinfo.value = 0;
    abs_setup.absinfo.fuzz = 0;
    abs_setup.absinfo.flat = 0;
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
    setup.id.vendor = 0x046d;   // Logitech
    setup.id.product = 0xc24f;  // G29 Racing Wheel
    setup.id.version = 1;
    strcpy(setup.name, "Logitech G29 Racing Wheel");
    
    ioctl(fd, UI_DEV_SETUP, &setup);
    ioctl(fd, UI_DEV_CREATE);
    
    std::cout << "Virtual Logitech G29 Racing Wheel created" << std::endl;
    return true;
}

void GamepadDevice::UpdateSteering(int delta, int sensitivity) {
    // Pure linear steering: each pixel of mouse movement adds to steering
    // sensitivity directly controls how many steering units per pixel
    // At sensitivity=50: 50 units per pixel, full lock at 32768/50 = 655 pixels (~7cm at 1000 DPI)
    // At sensitivity=25: 25 units per pixel, full lock at 32768/25 = 1310 pixels (~14cm at 1000 DPI)
    // Completely linear scaling
    steering += delta * static_cast<float>(sensitivity);
    
    // Clamp to int16_t range
    if (steering < -32768.0f) steering = -32768.0f;
    if (steering > 32767.0f) steering = 32767.0f;
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
    
    // Send steering wheel position - convert float to int16_t
    EmitEvent(EV_ABS, ABS_X, static_cast<int16_t>(steering));
    
    // Send Y axis (unused for G29, always 0)
    EmitEvent(EV_ABS, ABS_Y, 0);
    
    // Send RX/RY axes (unused for G29, always 0)
    EmitEvent(EV_ABS, ABS_RX, 0);
    EmitEvent(EV_ABS, ABS_RY, 0);
    
    // Send throttle and brake as pedal axes (G29 standard)
    uint8_t throttle_val = static_cast<uint8_t>(throttle * 2.55f);
    uint8_t brake_val = static_cast<uint8_t>(brake * 2.55f);
    
    EmitEvent(EV_ABS, ABS_Z, brake_val);    // Brake pedal
    EmitEvent(EV_ABS, ABS_RZ, throttle_val); // Throttle pedal
    
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
    
    // Zero all axes (center steering wheel)
    EmitEvent(EV_ABS, ABS_X, 0);
    EmitEvent(EV_ABS, ABS_Y, 0);
    EmitEvent(EV_ABS, ABS_RX, 0);
    EmitEvent(EV_ABS, ABS_RY, 0);
    
    // Zero pedals
    EmitEvent(EV_ABS, ABS_Z, 0);
    EmitEvent(EV_ABS, ABS_RZ, 0);
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
