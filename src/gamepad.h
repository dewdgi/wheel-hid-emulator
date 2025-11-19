

#ifndef GAMEPAD_H
#define GAMEPAD_H
#include <atomic>
#include <condition_variable>

extern std::atomic<bool> running;

#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <array>

class Input; // Forward declaration


class GamepadDevice {
public:
    // Notifies all condition variables for shutdown
    void NotifyAllShutdownCVs();

public:
    GamepadDevice();
    ~GamepadDevice();
    // Prevent copy and move
    GamepadDevice(const GamepadDevice&) = delete;
    GamepadDevice& operator=(const GamepadDevice&) = delete;
    GamepadDevice(GamepadDevice&&) noexcept = delete;
    GamepadDevice& operator=(GamepadDevice&&) noexcept = delete;

    bool IsFFBThreadJoinable() const { return ffb_thread.joinable(); }
private:
        public:
            // Signal threads to exit
            void ShutdownThreads();

            // Create virtual Logitech G29 Racing Wheel
            bool Create();
    // Update gamepad state
    void UpdateSteering(int delta, int sensitivity);
    void UpdateThrottle(bool pressed, float dt);
    void UpdateBrake(bool pressed, float dt);
    void UpdateButtons(const Input& input);
    void UpdateDPad(const Input& input);
    void UpdateClutch(bool pressed, float dt);

    // Send state to virtual device
    void SendState();
    void SendNeutral();

    // User-tunable settings
    void SetFFBGain(float gain);

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
    std::atomic<bool> state_dirty;
    std::mutex state_mutex;  // Protects state when thread is active
    std::condition_variable state_cv; // Notifies polling thread of state changes or shutdown
    std::condition_variable ffb_cv;   // Notifies FFB thread of state changes or shutdown
    // Notifies the polling thread that state has changed
    void NotifyStateChanged();

    // State
    bool enabled; // Emulation enabled/disabled, protected by state_mutex
    float steering;
    float user_steering;
    float ffb_offset;
    float ffb_velocity;
    float ffb_gain;
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
    std::array<uint8_t, 7> gadget_output_pending{}; // Incomplete OUTPUT report bytes
    size_t gadget_output_pending_len = 0;

    // UHID methods
    bool CreateUHID();
    bool CreateUSBGadget();
    bool CreateUInput();
    void SendUHIDReport();
    std::vector<uint8_t> BuildHIDReport();
    void USBGadgetPollingThread();  // Thread that responds to host polls
    void ReadGadgetOutput();        // Gather host OUTPUT data (FFB) in gadget mode
    void ParseFFBCommand(const uint8_t* data, size_t size);  // Parse FFB OUTPUT reports
    void FFBUpdateThread();  // Thread that continuously applies FFB forces
    float ShapeFFBTorque(float raw_force) const;
    bool ApplySteeringLocked();

    // UInput methods (fallback path)
    void EmitEvent(uint16_t type, uint16_t code, int32_t value);
    int16_t ClampSteering(int16_t value);
};

#endif // GAMEPAD_H
