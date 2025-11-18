#include "gamepad.h"
#include "input.h"
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <dirent.h>
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
    
    // Enable joystick buttons (matching real G29 wheel - 25 buttons total)
    // First 10 buttons using standard joystick codes
    ioctl(fd, UI_SET_KEYBIT, BTN_TRIGGER);  // Button 1
    ioctl(fd, UI_SET_KEYBIT, BTN_THUMB);    // Button 2
    ioctl(fd, UI_SET_KEYBIT, BTN_THUMB2);   // Button 3
    ioctl(fd, UI_SET_KEYBIT, BTN_TOP);      // Button 4
    ioctl(fd, UI_SET_KEYBIT, BTN_TOP2);     // Button 5
    ioctl(fd, UI_SET_KEYBIT, BTN_PINKIE);   // Button 6
    ioctl(fd, UI_SET_KEYBIT, BTN_BASE);     // Button 7
    ioctl(fd, UI_SET_KEYBIT, BTN_BASE2);    // Button 8
    ioctl(fd, UI_SET_KEYBIT, BTN_BASE3);    // Button 9
    ioctl(fd, UI_SET_KEYBIT, BTN_BASE4);    // Button 10
    
    // Additional buttons
    ioctl(fd, UI_SET_KEYBIT, BTN_BASE5);    // Button 11
    ioctl(fd, UI_SET_KEYBIT, BTN_BASE6);    // Button 12
    ioctl(fd, UI_SET_KEYBIT, BTN_DEAD);     // Button 13
    
    // Extra buttons using BTN_TRIGGER_HAPPY range for total 25
    ioctl(fd, UI_SET_KEYBIT, BTN_TRIGGER_HAPPY1);  // Button 14
    ioctl(fd, UI_SET_KEYBIT, BTN_TRIGGER_HAPPY2);  // Button 15
    ioctl(fd, UI_SET_KEYBIT, BTN_TRIGGER_HAPPY3);  // Button 16
    ioctl(fd, UI_SET_KEYBIT, BTN_TRIGGER_HAPPY4);  // Button 17
    ioctl(fd, UI_SET_KEYBIT, BTN_TRIGGER_HAPPY5);  // Button 18
    ioctl(fd, UI_SET_KEYBIT, BTN_TRIGGER_HAPPY6);  // Button 19
    ioctl(fd, UI_SET_KEYBIT, BTN_TRIGGER_HAPPY7);  // Button 20
    ioctl(fd, UI_SET_KEYBIT, BTN_TRIGGER_HAPPY8);  // Button 21
    ioctl(fd, UI_SET_KEYBIT, BTN_TRIGGER_HAPPY9);  // Button 22
    ioctl(fd, UI_SET_KEYBIT, BTN_TRIGGER_HAPPY10); // Button 23
    ioctl(fd, UI_SET_KEYBIT, BTN_TRIGGER_HAPPY11); // Button 24
    ioctl(fd, UI_SET_KEYBIT, BTN_TRIGGER_HAPPY12); // Button 25
    
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
    
    // Y axis (unused for G29) - Real G29 keeps this at maximum
    memset(&abs_setup, 0, sizeof(abs_setup));
    abs_setup.code = ABS_Y;
    abs_setup.absinfo.minimum = -32768;
    abs_setup.absinfo.maximum = 32767;
    abs_setup.absinfo.value = 32767;  // Match real G29
    abs_setup.absinfo.fuzz = 0;
    abs_setup.absinfo.flat = 0;
    ioctl(fd, UI_ABS_SETUP, &abs_setup);
    
    // Brake pedal (ABS_Z) - G29 pedals are inverted: 255 at rest, 0 when fully pressed
    memset(&abs_setup, 0, sizeof(abs_setup));
    abs_setup.code = ABS_Z;
    abs_setup.absinfo.minimum = 0;
    abs_setup.absinfo.maximum = 255;
    abs_setup.absinfo.value = 255;  // At rest = maximum
    abs_setup.absinfo.fuzz = 0;
    abs_setup.absinfo.flat = 0;
    ioctl(fd, UI_ABS_SETUP, &abs_setup);
    
    // Throttle pedal (ABS_RZ) - G29 pedals are inverted: 255 at rest, 0 when fully pressed
    memset(&abs_setup, 0, sizeof(abs_setup));
    abs_setup.code = ABS_RZ;
    abs_setup.absinfo.minimum = 0;
    abs_setup.absinfo.maximum = 255;
    abs_setup.absinfo.value = 255;  // At rest = maximum
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
    
    // Wait a moment for the device to be created
    usleep(100000);
    
    // Find the event device path
    std::string event_path = "unknown";
    DIR* dir = opendir("/dev/input");
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (strncmp(entry->d_name, "event", 5) == 0) {
                std::string path = std::string("/dev/input/") + entry->d_name;
                int test_fd = open(path.c_str(), O_RDONLY);
                if (test_fd >= 0) {
                    char name[256] = "Unknown";
                    ioctl(test_fd, EVIOCGNAME(sizeof(name)), name);
                    if (strcmp(name, "Logitech G29 Racing Wheel") == 0) {
                        event_path = path;
                        close(test_fd);
                        break;
                    }
                    close(test_fd);
                }
            }
        }
        closedir(dir);
    }
    
    std::cout << "Virtual Logitech G29 Racing Wheel created at " << event_path << std::endl;
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
    // Map keyboard keys to wheel buttons (joystick style)
    buttons["BTN_TRIGGER"] = input.IsKeyPressed(KEY_Q);
    buttons["BTN_THUMB"] = input.IsKeyPressed(KEY_E);
    buttons["BTN_THUMB2"] = input.IsKeyPressed(KEY_F);
    buttons["BTN_TOP"] = input.IsKeyPressed(KEY_G);
    buttons["BTN_TOP2"] = input.IsKeyPressed(KEY_H);
    buttons["BTN_PINKIE"] = input.IsKeyPressed(KEY_R);
    buttons["BTN_BASE"] = input.IsKeyPressed(KEY_T);
    buttons["BTN_BASE2"] = input.IsKeyPressed(KEY_Y);
    buttons["BTN_BASE3"] = input.IsKeyPressed(KEY_U);
    buttons["BTN_BASE4"] = input.IsKeyPressed(KEY_I);
    buttons["BTN_BASE5"] = input.IsKeyPressed(KEY_O);
    buttons["BTN_BASE6"] = input.IsKeyPressed(KEY_P);
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
    
    // Send Y axis (unused for G29, always at maximum like real wheel)
    EmitEvent(EV_ABS, ABS_Y, 32767);
    
    // Send throttle and brake as pedal axes (G29 standard)
    // Real G29 pedals are inverted: 255 at rest, 0 when fully pressed
    uint8_t throttle_val = 255 - static_cast<uint8_t>(throttle * 2.55f);
    uint8_t brake_val = 255 - static_cast<uint8_t>(brake * 2.55f);
    
    EmitEvent(EV_ABS, ABS_Z, brake_val);    // Brake pedal
    EmitEvent(EV_ABS, ABS_RZ, throttle_val); // Throttle pedal
    
    // Send wheel buttons (joystick style - 12 primary buttons)
    EmitEvent(EV_KEY, BTN_TRIGGER, buttons["BTN_TRIGGER"] ? 1 : 0);
    EmitEvent(EV_KEY, BTN_THUMB, buttons["BTN_THUMB"] ? 1 : 0);
    EmitEvent(EV_KEY, BTN_THUMB2, buttons["BTN_THUMB2"] ? 1 : 0);
    EmitEvent(EV_KEY, BTN_TOP, buttons["BTN_TOP"] ? 1 : 0);
    EmitEvent(EV_KEY, BTN_TOP2, buttons["BTN_TOP2"] ? 1 : 0);
    EmitEvent(EV_KEY, BTN_PINKIE, buttons["BTN_PINKIE"] ? 1 : 0);
    EmitEvent(EV_KEY, BTN_BASE, buttons["BTN_BASE"] ? 1 : 0);
    EmitEvent(EV_KEY, BTN_BASE2, buttons["BTN_BASE2"] ? 1 : 0);
    EmitEvent(EV_KEY, BTN_BASE3, buttons["BTN_BASE3"] ? 1 : 0);
    EmitEvent(EV_KEY, BTN_BASE4, buttons["BTN_BASE4"] ? 1 : 0);
    EmitEvent(EV_KEY, BTN_BASE5, buttons["BTN_BASE5"] ? 1 : 0);
    EmitEvent(EV_KEY, BTN_BASE6, buttons["BTN_BASE6"] ? 1 : 0);
    
    // Send D-Pad
    EmitEvent(EV_ABS, ABS_HAT0X, dpad_x);
    EmitEvent(EV_ABS, ABS_HAT0Y, dpad_y);
    
    // Sync
    EmitEvent(EV_SYN, SYN_REPORT, 0);
}

void GamepadDevice::SendNeutral() {
    if (fd < 0) return;
    
    // Reset steering to center
    steering = 0;
    throttle = 0;
    brake = 0;
    
    // Zero all axes (center steering wheel)
    EmitEvent(EV_ABS, ABS_X, 0);
    EmitEvent(EV_ABS, ABS_Y, 32767);  // Match real G29
    
    // Reset pedals to resting position (inverted: 255 = not pressed, shows as +32767 in jstest)
    EmitEvent(EV_ABS, ABS_Z, 255);
    EmitEvent(EV_ABS, ABS_RZ, 255);
    
    // Zero all buttons
    EmitEvent(EV_KEY, BTN_TRIGGER, 0);
    EmitEvent(EV_KEY, BTN_THUMB, 0);
    EmitEvent(EV_KEY, BTN_THUMB2, 0);
    EmitEvent(EV_KEY, BTN_TOP, 0);
    EmitEvent(EV_KEY, BTN_TOP2, 0);
    EmitEvent(EV_KEY, BTN_PINKIE, 0);
    EmitEvent(EV_KEY, BTN_BASE, 0);
    EmitEvent(EV_KEY, BTN_BASE2, 0);
    EmitEvent(EV_KEY, BTN_BASE3, 0);
    EmitEvent(EV_KEY, BTN_BASE4, 0);
    EmitEvent(EV_KEY, BTN_BASE5, 0);
    EmitEvent(EV_KEY, BTN_BASE6, 0);
    
    // Zero D-Pad
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
