#ifndef INPUT_MANAGER_H
#define INPUT_MANAGER_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "device_scanner.h"
#include "wheel_input.h"

class InputManager {
public:
    InputManager();
    ~InputManager();

    bool Initialize(const std::string& keyboard_override, const std::string& mouse_override);
    void Shutdown();

    bool WaitForFrame(InputFrame& frame);
    bool TryGetFrame(InputFrame& frame);

    bool GrabDevices(bool enable);
    bool AllRequiredGrabbed() const;
    void ResyncKeyStates();

    WheelInputState LatestLogicalState() const;

private:
    void ReaderLoop();
    WheelInputState BuildLogicalState();
    bool ShouldEmitFrame(int mouse_dx, bool toggle, const WheelInputState& next_state);

    DeviceScanner device_scanner_;
    std::thread reader_thread_;
    std::atomic<bool> reader_running_;
    mutable std::mutex frame_mutex_;
    std::condition_variable frame_cv_;
    InputFrame pending_frame_;
    WheelInputState current_state_;
    uint64_t frame_sequence_;
    uint64_t consumed_sequence_;
};

#endif  // INPUT_MANAGER_H
