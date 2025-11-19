

#ifndef GAMEPAD_H
#define GAMEPAD_H
#include <atomic>

extern std::atomic<bool> running;

#include <cstdint>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>

class Input; // Forward declaration


class GamepadDevice {

public:
    GamepadDevice();
    ~GamepadDevice();
    // Prevent copy and move
    GamepadDevice(const GamepadDevice& /*other*/) { std::cout << "[DEBUG][GamepadDevice] COPY CONSTRUCTOR CALLED" << std::endl; }
    GamepadDevice& operator=(const GamepadDevice& /*other*/) { std::cout << "[DEBUG][GamepadDevice] COPY ASSIGNMENT CALLED" << std::endl; return *this; }
    GamepadDevice(GamepadDevice&& /*other*/) noexcept { std::cout << "[DEBUG][GamepadDevice] MOVE CONSTRUCTOR CALLED" << std::endl; }
    GamepadDevice& operator=(GamepadDevice&& /*other*/) noexcept { std::cout << "[DEBUG][GamepadDevice] MOVE ASSIGNMENT CALLED" << std::endl; return *this; }

    bool IsFFBThreadJoinable() const { return ffb_thread.joinable(); }
private:
        public:
            // Signal threads to exit
            void ShutdownThreads();

            // Create virtual Logitech G29 Racing Wheel
            bool Create();
    // Update gamepad state
    void UpdateSteering(int delta, int sensitivity);
    void UpdateThrottle(bool pressed);
    void UpdateBrake(bool pressed);
    void UpdateButtons(const Input& input);
    void UpdateDPad(const Input& input);
    void UpdateClutch(bool pressed);

    // Send state to virtual device
    void SendState();
    void SendNeutral();

    // Process UHID events (must be called regularly when using UHID)
    void ProcessUHIDEvents();

    // Atomic enable/disable and grab/ungrab, protected by mutex (grab is now outside lock)
    void SetEnabled(bool enable, Input& input);
    bool IsEnabled();
    void ToggleEnabled(Input& input);

private:
    int fd;
    bool use_uhid;
    bool use_gadget;  // USB Gadget mode (proper USB device)

    // USB Gadget polling thread (mimics real USB HID device behavior)
    std::thread gadget_thread;
    std::atomic<bool> gadget_running;
    std::thread ffb_thread;
    std::atomic<bool> ffb_running;
    std::mutex state_mutex;  // Protects state when thread is active

    // State
    bool enabled; // Emulation enabled/disabled, protected by state_mutex
    float steering;
    float throttle;
    float brake;
    float clutch;
    std::map<std::string, bool> buttons;
    int8_t dpad_x;
    int8_t dpad_y;

    // Force Feedback state
    int16_t ffb_force;           // Current FFB force from game (-32768 to 32767)
    int16_t ffb_autocenter;      // Autocenter spring strength
    bool ffb_enabled;
    float user_torque;           // User input torque (from mouse)

    // UHID methods
    bool CreateUHID();
    bool CreateUSBGadget();
    bool CreateUInput();
    void SendUHIDReport();
    std::vector<uint8_t> BuildHIDReport();
    void USBGadgetPollingThread();  // Thread that responds to host polls
    void ParseFFBCommand(const uint8_t* data, size_t size);  // Parse FFB OUTPUT reports
    void FFBUpdateThread();  // Thread that continuously applies FFB forces

    // UInput methods (legacy)
    void EmitEvent(uint16_t type, uint16_t code, int32_t value);
    int16_t ClampSteering(int16_t value);
};

#endif // GAMEPAD_H
