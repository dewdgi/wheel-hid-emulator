#ifndef GAMEPAD_H
#define GAMEPAD_H

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <string>

class Input;
extern std::atomic<bool> running;

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

class GamepadDevice {
public:
    struct ControlSnapshot;

    GamepadDevice();
    ~GamepadDevice();

    GamepadDevice(const GamepadDevice&) = delete;
    GamepadDevice& operator=(const GamepadDevice&) = delete;
    GamepadDevice(GamepadDevice&&) noexcept = delete;
    GamepadDevice& operator=(GamepadDevice&&) noexcept = delete;

    bool Create();
    void ShutdownThreads();
    void NotifyAllShutdownCVs();

    bool IsEnabled();
    void SetEnabled(bool enable, Input& input);
    void ToggleEnabled(Input& input);
    void SetFFBGain(float gain);

    void ProcessInputFrame(int mouse_dx, int sensitivity, const Input& input);
    void SendNeutral(bool reset_ffb = true);
    void ApplyCurrentInput(const Input& input);
    void ApplySnapshot(const ControlSnapshot& snapshot);

private:
    void NotifyStateChanged();
    bool CreateUSBGadget();
    void DestroyUSBGadget();
    void SendGadgetReport();
    std::array<uint8_t, 13> BuildHIDReport();
    std::array<uint8_t, 13> BuildHIDReportLocked() const;
    void USBGadgetPollingThread();
    void USBGadgetOutputThread();
    void ReadGadgetOutput();
    void FFBUpdateThread();
    void ParseFFBCommand(const uint8_t* data, size_t size);
    float ShapeFFBTorque(float raw_force) const;
    bool ApplySteeringLocked();
    bool ApplySteeringDeltaLocked(int delta, int sensitivity);
    bool ApplySnapshotLocked(const ControlSnapshot& snapshot);
    void ApplyNeutralLocked(bool reset_ffb);
    ControlSnapshot CaptureSnapshot(const Input& input) const;
    uint32_t BuildButtonBitsLocked() const;
    bool WriteHIDBlocking(const uint8_t* data, size_t size);
    bool WriteReportBlocking(const std::array<uint8_t, 13>& report);
    bool BindUDC();
    bool UnbindUDC();
    std::string GadgetUDCPath() const;
    std::string DetectFirstUDC() const;
    void EnsureGadgetThreadsStarted();
    void StopGadgetThreads();

    int fd;
    std::thread gadget_thread;
    std::atomic<bool> gadget_running;
    std::thread gadget_output_thread;
    std::atomic<bool> gadget_output_running;
    std::thread ffb_thread;
    std::atomic<bool> ffb_running;
    std::atomic<bool> state_dirty;
    std::atomic<int> warmup_frames;
    std::atomic<bool> output_enabled;
    std::mutex gadget_mutex;
    bool udc_bound;
    std::string udc_name;
    std::mutex state_mutex;
    std::condition_variable state_cv;
    std::condition_variable ffb_cv;

    bool enabled;
    float steering;
    float user_steering;
    float ffb_offset;
    float ffb_velocity;
    float ffb_gain;
    float throttle;
    float brake;
    float clutch;
    std::array<uint8_t, static_cast<size_t>(WheelButton::Count)> button_states;
    int8_t dpad_x;
    int8_t dpad_y;

    int16_t ffb_force;
    int16_t ffb_autocenter;
    std::array<uint8_t, 7> gadget_output_pending{};
    size_t gadget_output_pending_len;
};

#endif  // GAMEPAD_H
