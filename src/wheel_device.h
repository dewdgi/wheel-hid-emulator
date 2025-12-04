#ifndef WHEEL_DEVICE_H
#define WHEEL_DEVICE_H

#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <string>

#include "hid/hid_device.h"
#include "input/wheel_input.h"
#include "wheel_types.h"

class InputManager;
extern std::atomic<bool> running;

class WheelDevice {
public:
    WheelDevice();
    ~WheelDevice();

    WheelDevice(const WheelDevice&) = delete;
    WheelDevice& operator=(const WheelDevice&) = delete;
    WheelDevice(WheelDevice&&) noexcept = delete;
    WheelDevice& operator=(WheelDevice&&) noexcept = delete;

    bool Create();
    void ShutdownThreads();
    void NotifyAllShutdownCVs();

    bool IsEnabled();
    void SetEnabled(bool enable, InputManager& input_manager);
    void ToggleEnabled(InputManager& input_manager);
    void SetFFBGain(float gain);

    void ProcessInputFrame(const InputFrame& frame, int sensitivity);
    void SendNeutral(bool reset_ffb = true);
    void ApplySnapshot(const WheelInputState& snapshot);

private:
    void NotifyStateChanged();
    bool SendGadgetReport();
    std::array<uint8_t, 13> BuildHIDReport();
    std::array<uint8_t, 13> BuildHIDReportLocked() const;
    void USBGadgetPollingThread();
    void USBGadgetOutputThread();
    void ReadGadgetOutput(int fd);
    void FFBUpdateThread();
    void ParseFFBCommand(const uint8_t* data, size_t size);
    float ShapeFFBTorque(float raw_force) const;
    bool ApplySteeringLocked();
    bool ApplySteeringDeltaLocked(int delta, int sensitivity);
    bool ApplySnapshotLocked(const WheelInputState& snapshot);
    void ApplyNeutralLocked(bool reset_ffb);
    uint32_t BuildButtonBitsLocked() const;
    bool WriteReportBlocking(const std::array<uint8_t, 13>& report);
    bool WaitForStateFlush(int timeout_ms);
    void EnsureGadgetThreadsStarted();
    void StopGadgetThreads();

    std::thread gadget_thread;
    std::atomic<bool> gadget_running;
    std::thread gadget_output_thread;
    std::atomic<bool> gadget_output_running;
    std::thread ffb_thread;
    std::atomic<bool> ffb_running;
    std::atomic<bool> state_dirty;
    std::atomic<int> warmup_frames;
    std::atomic<bool> output_enabled;
    std::mutex enable_mutex;
    std::mutex state_mutex;
    std::condition_variable state_cv;
    std::condition_variable ffb_cv;

    hid::HidDevice hid_device_;

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

#endif  // WHEEL_DEVICE_H
