#include "input_manager.h"

#include <linux/input-event-codes.h>

#include <atomic>
#include <chrono>

#include "../logging/logger.h"

extern std::atomic<bool> running;

namespace {
constexpr const char* kTag = "input_manager";
}

InputManager::InputManager() : reader_running_(false), frame_sequence_(0), consumed_sequence_(0) {
    pending_frame_.timestamp = std::chrono::steady_clock::now();
}

InputManager::~InputManager() {
    Shutdown();
}

bool InputManager::Initialize(const std::string& keyboard_override, const std::string& mouse_override) {
    if (!device_scanner_.DiscoverKeyboard(keyboard_override)) {
        LOG_ERROR(kTag, "Failed to discover keyboard " << keyboard_override);
        return false;
    }
    if (!device_scanner_.DiscoverMouse(mouse_override)) {
        LOG_ERROR(kTag, "Failed to discover mouse " << mouse_override);
        return false;
    }

    current_state_ = BuildLogicalState();
    pending_frame_.logical = current_state_;
    pending_frame_.timestamp = std::chrono::steady_clock::now();

    reader_running_.store(true, std::memory_order_relaxed);
    reader_thread_ = std::thread(&InputManager::ReaderLoop, this);
    LOG_INFO(kTag, "Input manager initialized");
    return true;
}

void InputManager::Shutdown() {
    bool was_running = reader_running_.exchange(false);
    if (was_running) {
        device_scanner_.NotifyInputChanged();
    }
    frame_cv_.notify_all();
    if (reader_thread_.joinable()) {
        reader_thread_.join();
    }
}

bool InputManager::WaitForFrame(InputFrame& frame) {
    std::unique_lock<std::mutex> lock(frame_mutex_);
    frame_cv_.wait(lock, [this]() {
        return consumed_sequence_ != frame_sequence_ || !reader_running_.load(std::memory_order_relaxed) ||
               !running.load(std::memory_order_relaxed);
    });
    if (consumed_sequence_ == frame_sequence_) {
        return false;
    }
    frame = pending_frame_;
    pending_frame_.mouse_dx = 0;
    pending_frame_.toggle_pressed = false;
    consumed_sequence_ = frame_sequence_;
    return true;
}

bool InputManager::TryGetFrame(InputFrame& frame) {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (consumed_sequence_ == frame_sequence_) {
        return false;
    }
    frame = pending_frame_;
    pending_frame_.mouse_dx = 0;
    pending_frame_.toggle_pressed = false;
    consumed_sequence_ = frame_sequence_;
    return true;
}

bool InputManager::GrabDevices(bool enable) {
    return device_scanner_.Grab(enable);
}

bool InputManager::AllRequiredGrabbed() const {
    return device_scanner_.AllRequiredGrabbed();
}

void InputManager::ResyncKeyStates() {
    device_scanner_.ResyncKeyStates();
    std::lock_guard<std::mutex> lock(frame_mutex_);
    current_state_ = BuildLogicalState();
}

WheelInputState InputManager::LatestLogicalState() const {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    return current_state_;
}

void InputManager::ReaderLoop() {
    LOG_DEBUG(kTag, "Reader loop started");
    while (reader_running_.load(std::memory_order_relaxed) && running.load(std::memory_order_relaxed)) {
        device_scanner_.WaitForEvents(-1);
        int mouse_dx = 0;
        device_scanner_.Read(mouse_dx);
        bool toggle = device_scanner_.CheckToggle();
        WheelInputState next_state = BuildLogicalState();
        if (!ShouldEmitFrame(mouse_dx, toggle, next_state)) {
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(frame_mutex_);
            current_state_ = next_state;
            pending_frame_.logical = next_state;
            pending_frame_.mouse_dx += mouse_dx;
            pending_frame_.toggle_pressed = pending_frame_.toggle_pressed || toggle;
            pending_frame_.timestamp = std::chrono::steady_clock::now();
            ++frame_sequence_;
        }
        frame_cv_.notify_all();
    }
    frame_cv_.notify_all();
    LOG_DEBUG(kTag, "Reader loop stopped");
}

WheelInputState InputManager::BuildLogicalState() {
    WheelInputState snapshot;
    snapshot.throttle = device_scanner_.IsKeyPressed(KEY_W);
    snapshot.brake = device_scanner_.IsKeyPressed(KEY_S);
    snapshot.clutch = device_scanner_.IsKeyPressed(KEY_A);

    int right = device_scanner_.IsKeyPressed(KEY_RIGHT) ? 1 : 0;
    int left = device_scanner_.IsKeyPressed(KEY_LEFT) ? 1 : 0;
    int down = device_scanner_.IsKeyPressed(KEY_DOWN) ? 1 : 0;
    int up = device_scanner_.IsKeyPressed(KEY_UP) ? 1 : 0;
    snapshot.dpad_x = static_cast<int8_t>(right - left);
    snapshot.dpad_y = static_cast<int8_t>(down - up);

    auto set_button = [&](WheelButton button, bool pressed) {
        snapshot.buttons[static_cast<size_t>(button)] = pressed ? 1 : 0;
    };

    set_button(WheelButton::South, device_scanner_.IsKeyPressed(KEY_Q));
    set_button(WheelButton::East, device_scanner_.IsKeyPressed(KEY_E));
    set_button(WheelButton::West, device_scanner_.IsKeyPressed(KEY_F));
    set_button(WheelButton::North, device_scanner_.IsKeyPressed(KEY_G));
    set_button(WheelButton::TL, device_scanner_.IsKeyPressed(KEY_H));
    set_button(WheelButton::TR, device_scanner_.IsKeyPressed(KEY_R));
    set_button(WheelButton::TL2, device_scanner_.IsKeyPressed(KEY_T));
    set_button(WheelButton::TR2, device_scanner_.IsKeyPressed(KEY_Y));
    set_button(WheelButton::Select, device_scanner_.IsKeyPressed(KEY_U));
    set_button(WheelButton::Start, device_scanner_.IsKeyPressed(KEY_I));
    set_button(WheelButton::ThumbL, device_scanner_.IsKeyPressed(KEY_O));
    set_button(WheelButton::ThumbR, device_scanner_.IsKeyPressed(KEY_P));
    set_button(WheelButton::Mode, device_scanner_.IsKeyPressed(KEY_1));
    set_button(WheelButton::Dead, device_scanner_.IsKeyPressed(KEY_2));
    set_button(WheelButton::TriggerHappy1, device_scanner_.IsKeyPressed(KEY_3));
    set_button(WheelButton::TriggerHappy2, device_scanner_.IsKeyPressed(KEY_4));
    set_button(WheelButton::TriggerHappy3, device_scanner_.IsKeyPressed(KEY_5));
    set_button(WheelButton::TriggerHappy4, device_scanner_.IsKeyPressed(KEY_6));
    set_button(WheelButton::TriggerHappy5, device_scanner_.IsKeyPressed(KEY_7));
    set_button(WheelButton::TriggerHappy6, device_scanner_.IsKeyPressed(KEY_8));
    set_button(WheelButton::TriggerHappy7, device_scanner_.IsKeyPressed(KEY_9));
    set_button(WheelButton::TriggerHappy8, device_scanner_.IsKeyPressed(KEY_0));
    set_button(WheelButton::TriggerHappy9, device_scanner_.IsKeyPressed(KEY_LEFTSHIFT));
    set_button(WheelButton::TriggerHappy10, device_scanner_.IsKeyPressed(KEY_SPACE));
    set_button(WheelButton::TriggerHappy11, device_scanner_.IsKeyPressed(KEY_TAB));
    set_button(WheelButton::TriggerHappy12, device_scanner_.IsKeyPressed(KEY_ENTER));
    return snapshot;
}

bool InputManager::ShouldEmitFrame(int mouse_dx, bool toggle, const WheelInputState& next_state) {
    if (mouse_dx != 0 || toggle) {
        return true;
    }
    if (next_state.buttons != current_state_.buttons) {
        return true;
    }
    if (next_state.throttle != current_state_.throttle ||
        next_state.brake != current_state_.brake ||
        next_state.clutch != current_state_.clutch) {
        return true;
    }
    if (next_state.dpad_x != current_state_.dpad_x || next_state.dpad_y != current_state_.dpad_y) {
        return true;
    }
    return false;
}
