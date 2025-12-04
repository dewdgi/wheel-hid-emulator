#include "wheel_device.h"
#include "input/input_manager.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <string>
#include <thread>
#include <unistd.h>

#include "logging/logger.h"

extern std::atomic<bool> running;

namespace {

constexpr size_t kFFBPacketSize = 7;
constexpr const char* kTag = "wheel_device";

}  // namespace

void WheelDevice::NotifyAllShutdownCVs() {
    state_cv.notify_all();
    ffb_cv.notify_all();
}
WheelDevice::WheelDevice()
        : gadget_running(false), gadget_output_running(false),
            enabled(false), steering(0.0f), user_steering(0.0f), ffb_offset(0.0f),
      ffb_velocity(0.0f), ffb_gain(1.0f), throttle(0.0f), brake(0.0f),
      clutch(0.0f), dpad_x(0), dpad_y(0), ffb_force(0),
            ffb_autocenter(0), gadget_output_pending_len(0) {
    ffb_running = false;
    state_dirty = false;
        warmup_frames.store(0, std::memory_order_relaxed);
    output_enabled.store(false, std::memory_order_relaxed);
    button_states.fill(0);
}

WheelDevice::~WheelDevice() {
    ShutdownThreads();
}

void WheelDevice::ShutdownThreads() {
    ffb_running = false;
    gadget_running = false;
    gadget_output_running = false;
        warmup_frames.store(0, std::memory_order_relaxed);
    output_enabled.store(false, std::memory_order_relaxed);

    state_cv.notify_all();
    ffb_cv.notify_all();

    StopGadgetThreads();
    if (ffb_thread.joinable()) {
        ffb_thread.join();
    }

}

bool WheelDevice::Create() {
    LOG_DEBUG(kTag, "Attempting to create device using USB Gadget (real USB device)...");
    if (!hid_device_.Initialize()) {
        std::cerr << "USB Gadget creation failed; wheel emulator requires a USB gadget capable kernel" << std::endl;
        std::cerr << "Ensure configfs is mounted, libcomposite/dummy_hcd modules are available, and a UDC is present." << std::endl;
        return false;
    }

    SendNeutral(true);

    ffb_running = true;
    ffb_thread = std::thread(&WheelDevice::FFBUpdateThread, this);
    return true;
}



void WheelDevice::EnsureGadgetThreadsStarted() {
    hid_device_.SetNonBlockingMode(true);
    if (!gadget_running) {
        gadget_running = true;
        gadget_thread = std::thread(&WheelDevice::USBGadgetPollingThread, this);
    }
    if (!gadget_output_running) {
        gadget_output_running = true;
        gadget_output_thread = std::thread(&WheelDevice::USBGadgetOutputThread, this);
    }
}

void WheelDevice::StopGadgetThreads() {
    if (gadget_running) {
        gadget_running = false;
        state_cv.notify_all();
    }
    if (gadget_output_running) {
        gadget_output_running = false;
    }
    if (gadget_thread.joinable()) {
        gadget_thread.join();
    }
    if (gadget_output_thread.joinable()) {
        gadget_output_thread.join();
    }
}

bool WheelDevice::IsEnabled() {
    std::lock_guard<std::mutex> lock(state_mutex);
    return enabled;
}

void WheelDevice::SetEnabled(bool enable, InputManager& input_manager) {
    std::unique_lock<std::mutex> enable_lock(enable_mutex);
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        if (enabled != enable) {
            enabled = enable;
            changed = true;
        }
    }
    if (!changed) {
        if (!enable) {
            input_manager.GrabDevices(false);
        }
        return;
    }

    if (enable) {
        if (!input_manager.GrabDevices(true)) {
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                enabled = false;
            }
            std::cerr << "Enable aborted: unable to grab keyboard/mouse" << std::endl;
            return;
        }
        if (!input_manager.AllRequiredGrabbed()) {
            input_manager.GrabDevices(false);
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                enabled = false;
            }
            std::cerr << "Enable aborted: missing required input device" << std::endl;
            return;
        }

        input_manager.ResyncKeyStates();

        output_enabled.store(false, std::memory_order_release);
        warmup_frames.store(0, std::memory_order_release);
        state_dirty.store(false, std::memory_order_release);

        std::array<uint8_t, 13> neutral_report;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            ApplyNeutralLocked(false);
            neutral_report = BuildHIDReportLocked();
        }

        if (!hid_device_.IsUdcBound() && !hid_device_.BindUDC()) {
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                ApplyNeutralLocked(true);
                enabled = false;
            }
            input_manager.GrabDevices(false);
            return;
        }

        if (!hid_device_.WaitForEndpointReady()) {
            std::cerr << "[WheelDevice] HID endpoint never became ready; holding neutral" << std::endl;
            input_manager.GrabDevices(false);
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                enabled = false;
            }
            return;
        }

        EnsureGadgetThreadsStarted();

        bool neutral_sent = false;
        output_enabled.store(true, std::memory_order_release);
        warmup_frames.store(0, std::memory_order_release);
        state_dirty.store(false, std::memory_order_release);

        {
            std::lock_guard<std::mutex> lock(state_mutex);
            ApplyNeutralLocked(false);
        }
        state_dirty.store(true, std::memory_order_release);
        state_cv.notify_all();
        neutral_sent = WaitForStateFlush(150);

        if (!neutral_sent) {
            output_enabled.store(false, std::memory_order_release);
            state_dirty.store(false, std::memory_order_release);
            if (!WriteReportBlocking(neutral_report)) {
                std::cerr << "[WheelDevice] Failed to prime HID reports; holding neutral" << std::endl;
                input_manager.GrabDevices(false);
                {
                    std::lock_guard<std::mutex> lock(state_mutex);
                    enabled = false;
                }
                return;
            }
            output_enabled.store(true, std::memory_order_release);
        }

        warmup_frames.store(25, std::memory_order_release);
        state_cv.notify_all();
    } else {
        warmup_frames.store(0, std::memory_order_release);

        std::array<uint8_t, 13> neutral_report;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            ApplyNeutralLocked(true);
            neutral_report = BuildHIDReportLocked();
        }

        bool neutral_sent = false;
        if (gadget_running.load(std::memory_order_acquire) && output_enabled.load(std::memory_order_acquire)) {
            state_dirty.store(true, std::memory_order_release);
            state_cv.notify_all();
            neutral_sent = WaitForStateFlush(150);
        }

        output_enabled.store(false, std::memory_order_release);
        state_dirty.store(false, std::memory_order_release);

        if (!neutral_sent && !WriteReportBlocking(neutral_report)) {
            std::cerr << "[WheelDevice] Failed to send neutral frame while disabling" << std::endl;
        }
        input_manager.ResyncKeyStates();
        input_manager.GrabDevices(false);
    }
    LOG_INFO(kTag, (enable ? "Emulation ENABLED" : "Emulation DISABLED"));
}

void WheelDevice::ToggleEnabled(InputManager& input_manager) {
    bool next_state;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        next_state = !enabled;
    }
    SetEnabled(next_state, input_manager);
}

void WheelDevice::SetFFBGain(float gain) {
    if (gain < 0.1f) {
        gain = 0.1f;
    } else if (gain > 4.0f) {
        gain = 4.0f;
    }
    std::lock_guard<std::mutex> lock(state_mutex);
    ffb_gain = gain;
}

void WheelDevice::ProcessInputFrame(const InputFrame& frame, int sensitivity) {
    if (!enabled || !output_enabled.load(std::memory_order_acquire)) {
        return;
    }
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        changed |= ApplySteeringDeltaLocked(frame.mouse_dx, sensitivity);
        changed |= ApplySnapshotLocked(frame.logical);
    }
    if (changed) {
        NotifyStateChanged();
    }
}

void WheelDevice::ApplySnapshot(const WheelInputState& snapshot) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        changed = ApplySnapshotLocked(snapshot);
    }
    if (changed) {
        NotifyStateChanged();
    }
}

void WheelDevice::SendNeutral(bool reset_ffb) {
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        ApplyNeutralLocked(reset_ffb);
    }
    if (hid_device_.IsReady()) {
        NotifyStateChanged();
    }
}

void WheelDevice::NotifyStateChanged() {
    state_dirty.store(true, std::memory_order_release);
    state_cv.notify_all();
    ffb_cv.notify_all();
}

bool WheelDevice::WaitForStateFlush(int timeout_ms) {
    if (timeout_ms <= 0) {
        return !state_dirty.load(std::memory_order_acquire);
    }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!state_dirty.load(std::memory_order_acquire)) {
            return true;
        }
        if (!running.load(std::memory_order_acquire) ||
            !gadget_running.load(std::memory_order_acquire) ||
            !output_enabled.load(std::memory_order_acquire)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return !state_dirty.load(std::memory_order_acquire);
}

bool WheelDevice::ApplySteeringDeltaLocked(int delta, int sensitivity) {
    if (delta == 0) {
        return false;
    }

    constexpr float base_gain = 0.05f;
    const float gain = static_cast<float>(sensitivity) * base_gain;
    const float max_step = 2000.0f;
    float step = static_cast<float>(delta) * gain;
    step = std::clamp(step, -max_step, max_step);
    user_steering += step;
    const float max_angle = 32767.0f;
    user_steering = std::clamp(user_steering, -max_angle, max_angle);
    return ApplySteeringLocked();
}

bool WheelDevice::ApplySnapshotLocked(const WheelInputState& snapshot) {
    bool changed = false;
    auto set_axis = [&](float& axis, bool pressed) {
        float next = pressed ? 100.0f : 0.0f;
        if (axis != next) {
            axis = next;
            changed = true;
        }
    };

    set_axis(throttle, snapshot.throttle);
    set_axis(brake, snapshot.brake);
    set_axis(clutch, snapshot.clutch);

    if (dpad_x != snapshot.dpad_x) {
        dpad_x = snapshot.dpad_x;
        changed = true;
    }
    if (dpad_y != snapshot.dpad_y) {
        dpad_y = snapshot.dpad_y;
        changed = true;
    }

    if (button_states != snapshot.buttons) {
        button_states = snapshot.buttons;
        changed = true;
    }

    return changed;
}

void WheelDevice::ApplyNeutralLocked(bool reset_ffb) {
    steering = 0.0f;
    user_steering = 0.0f;
    if (reset_ffb) {
        ffb_offset = 0.0f;
        ffb_velocity = 0.0f;
    }
    throttle = 0.0f;
    brake = 0.0f;
    clutch = 0.0f;
    dpad_x = 0;
    dpad_y = 0;
    button_states.fill(0);
}


std::array<uint8_t, 13> WheelDevice::BuildHIDReport() {
    std::lock_guard<std::mutex> lock(state_mutex);
    return BuildHIDReportLocked();
}

std::array<uint8_t, 13> WheelDevice::BuildHIDReportLocked() const {
    std::array<uint8_t, 13> report{};

    uint16_t steering_u = static_cast<uint16_t>(static_cast<int16_t>(steering) + 32768);
    report[0] = steering_u & 0xFF;
    report[1] = (steering_u >> 8) & 0xFF;

    uint16_t clutch_u = 65535 - static_cast<uint16_t>(clutch * 655.35f);
    report[2] = clutch_u & 0xFF;
    report[3] = (clutch_u >> 8) & 0xFF;

    uint16_t throttle_u = 65535 - static_cast<uint16_t>(throttle * 655.35f);
    report[4] = throttle_u & 0xFF;
    report[5] = (throttle_u >> 8) & 0xFF;

    uint16_t brake_u = 65535 - static_cast<uint16_t>(brake * 655.35f);
    report[6] = brake_u & 0xFF;
    report[7] = (brake_u >> 8) & 0xFF;

    uint8_t hat = 0x0F;
    if (dpad_y == -1 && dpad_x == 0) hat = 0;
    else if (dpad_y == -1 && dpad_x == 1) hat = 1;
    else if (dpad_y == 0 && dpad_x == 1) hat = 2;
    else if (dpad_y == 1 && dpad_x == 1) hat = 3;
    else if (dpad_y == 1 && dpad_x == 0) hat = 4;
    else if (dpad_y == 1 && dpad_x == -1) hat = 5;
    else if (dpad_y == 0 && dpad_x == -1) hat = 6;
    else if (dpad_y == -1 && dpad_x == -1) hat = 7;

    report[8] = hat & 0x0F;

    uint32_t button_bits = BuildButtonBitsLocked();
    report[9] = button_bits & 0xFF;
    report[10] = (button_bits >> 8) & 0xFF;
    report[11] = (button_bits >> 16) & 0xFF;
    report[12] = (button_bits >> 24) & 0xFF;

    return report;
}

bool WheelDevice::SendGadgetReport() {
    auto report_data = BuildHIDReport();
    return hid_device_.WriteReportBlocking(report_data);
}

bool WheelDevice::WriteReportBlocking(const std::array<uint8_t, 13>& report) {
    return hid_device_.WriteReportBlocking(report);
}


void WheelDevice::USBGadgetPollingThread() {
    std::unique_lock<std::mutex> lock(state_mutex);
    while (gadget_running && running) {
        state_cv.wait_for(lock, std::chrono::milliseconds(2), [&] {
            return !gadget_running || !running ||
                   state_dirty.load(std::memory_order_acquire) ||
                   warmup_frames.load(std::memory_order_acquire) > 0;
        });
        if (!gadget_running || !running) {
            break;
        }
        bool should_send = state_dirty.exchange(false, std::memory_order_acq_rel);
        bool warmup = false;
        int pending = warmup_frames.load(std::memory_order_acquire);
        if (pending > 0) {
            warmup = true;
            warmup_frames.fetch_sub(1, std::memory_order_acq_rel);
        }
        bool allow_output = output_enabled.load(std::memory_order_acquire);
        lock.unlock();
        if (allow_output && (should_send || warmup)) {
            bool ready = hid_device_.IsReady();
            if (!ready) {
                if (!hid_device_.IsUdcBound()) {
                    state_dirty.store(true, std::memory_order_release);
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                } else if (!hid_device_.WaitForEndpointReady(50)) {
                    state_dirty.store(true, std::memory_order_release);
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                } else {
                    ready = true;
                }
            }
            if (ready && !SendGadgetReport()) {
                hid_device_.ResetEndpoint();
                state_dirty.store(true, std::memory_order_release);
            }
        }
        lock.lock();
    }
}

void WheelDevice::USBGadgetOutputThread() {
    while (gadget_output_running && running) {
        if (!hid_device_.IsUdcBound()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        if (!hid_device_.IsReady() && !hid_device_.WaitForEndpointReady(10)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        int fd = hid_device_.fd();
        if (fd < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int ret = poll(&pfd, 1, 5);
        if (!gadget_output_running || !running) {
            break;
        }
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            hid_device_.ResetEndpoint();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        if (ret == 0) {
            continue;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            hid_device_.ResetEndpoint();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        if (pfd.revents & POLLIN) {
            ReadGadgetOutput(fd);
        }
    }
}

void WheelDevice::ReadGadgetOutput(int fd) {
    if (fd < 0) {
        return;
    }

    uint8_t buffer[32];
    while (gadget_output_running && running) {
        ssize_t bytes = read(fd, buffer, sizeof(buffer));
        if (bytes < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            hid_device_.ResetEndpoint();
            break;
        }
        if (bytes == 0) {
            break;
        }

        size_t total = static_cast<size_t>(bytes);
        size_t offset = 0;
        while (offset < total) {
            size_t needed = kFFBPacketSize - gadget_output_pending_len;
            size_t chunk = total - offset;
            if (chunk > needed) {
                chunk = needed;
            }
            std::memcpy(gadget_output_pending.data() + gadget_output_pending_len,
                        buffer + offset,
                        chunk);
            gadget_output_pending_len += chunk;
            offset += chunk;

            if (gadget_output_pending_len == kFFBPacketSize) {
                if (output_enabled.load(std::memory_order_acquire)) {
                    ParseFFBCommand(gadget_output_pending.data(), kFFBPacketSize);
                }
                gadget_output_pending_len = 0;
            }
        }
    }
}

void WheelDevice::FFBUpdateThread() {
    float filtered_ffb = 0.0f;
    using clock = std::chrono::steady_clock;
    auto last = clock::now();

    while (true) {
        std::unique_lock<std::mutex> lock(state_mutex);
        ffb_cv.wait_for(lock, std::chrono::milliseconds(1));
        if (!ffb_running || !running) {
            break;
        }

        if (!enabled || !output_enabled.load(std::memory_order_acquire)) {
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        int16_t local_force = ffb_force;
        int16_t local_autocenter = ffb_autocenter;
        float local_offset = ffb_offset;
        float local_velocity = ffb_velocity;
        float local_gain = ffb_gain;
        float local_steering = steering;
        lock.unlock();

        auto now = clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        if (dt <= 0.0f) dt = 0.001f;
        if (dt > 0.01f) dt = 0.01f;
        last = now;

        float commanded_force = ShapeFFBTorque(static_cast<float>(local_force));

        const float force_filter_hz = 38.0f;
        float alpha = 1.0f - std::exp(-dt * force_filter_hz);
        alpha = std::clamp(alpha, 0.0f, 1.0f);
        filtered_ffb += (commanded_force - filtered_ffb) * alpha;

        float spring = 0.0f;
        if (local_autocenter > 0) {
            spring = -(local_steering * static_cast<float>(local_autocenter)) / 32768.0f;
        }

        const float offset_limit = 22000.0f;
        float target_offset = (filtered_ffb + spring) * local_gain;
        target_offset = std::clamp(target_offset, -offset_limit, offset_limit);

        const float stiffness = 120.0f;
        const float damping = 8.0f;
        const float max_velocity = 90000.0f;
        float error = target_offset - local_offset;
        local_velocity += error * stiffness * dt;
        float damping_factor = std::exp(-damping * dt);
        local_velocity *= damping_factor;
        local_velocity = std::clamp(local_velocity, -max_velocity, max_velocity);

        local_offset += local_velocity * dt;
        if (local_offset > offset_limit) {
            local_offset = offset_limit;
            local_velocity = 0.0f;
        } else if (local_offset < -offset_limit) {
            local_offset = -offset_limit;
            local_velocity = 0.0f;
        }

        lock.lock();
        if (!ffb_running || !running) {
            break;
        }
        ffb_offset = local_offset;
        ffb_velocity = local_velocity;
        bool steering_changed = ApplySteeringLocked();
        lock.unlock();

        if (steering_changed) {
            state_dirty.store(true, std::memory_order_release);
            state_cv.notify_all();
        }
    }
}

void WheelDevice::ParseFFBCommand(const uint8_t* data, size_t size) {
    if (size != kFFBPacketSize) {
        return;
    }

    std::lock_guard<std::mutex> lock(state_mutex);
    if (!enabled) {
        return;
    }
    bool state_changed = false;

    uint8_t cmd = data[0];

    switch (cmd) {
        case 0x11: {  // Constant force slot update
            int8_t force = static_cast<int8_t>(data[2]) - 0x80;
            ffb_force = static_cast<int16_t>(-force) * 48;
            state_changed = true;
            break;
        }
        case 0x13:  // Stop force effect
            ffb_force = 0;
            state_changed = true;
            break;
        case 0xf5:  // Disable autocenter
            if (ffb_autocenter != 0) {
                ffb_autocenter = 0;
                state_changed = true;
            }
            break;
        case 0xfe:  // Configure autocenter
            if (data[1] == 0x0d) {
                int16_t strength = static_cast<int16_t>(data[2]) * 16;
                if (ffb_autocenter != strength) {
                    ffb_autocenter = strength;
                    state_changed = true;
                }
            }
            break;
        case 0x14:  // Enable default autocenter
            if (ffb_autocenter == 0) {
                ffb_autocenter = 1024;
                state_changed = true;
            }
            break;
        case 0xf8:  // Extended commands
            switch (data[1]) {
                case 0x81:  // Wheel range
                case 0x12:  // LEDs
                case 0x09:  // Mode switch
                case 0x0a:  // Mode revert on reset
                default:
                    break;
            }
            break;
        default:
            break;
    }

    if (state_changed) {
        ffb_cv.notify_all();
    }
}

float WheelDevice::ShapeFFBTorque(float raw_force) const {
    float abs_force = std::fabs(raw_force);
    if (abs_force < 80.0f) {
        return raw_force * (abs_force / 80.0f);
    }

    const float min_gain = 0.25f;
    const float slip_knee = 4000.0f;
    const float slip_full = 14000.0f;
    float t = (abs_force - 80.0f) / (slip_full - 80.0f);
    t = std::clamp(t, 0.0f, 1.0f);
    float slip_weight = t * t;

    float gain = min_gain;
    if (abs_force > slip_knee) {
        float heavy = (abs_force - slip_knee) / (slip_full - slip_knee);
        heavy = std::clamp(heavy, 0.0f, 1.0f);
        gain = min_gain + (1.0f - min_gain) * heavy;
    } else {
        gain = min_gain + (slip_weight * (1.0f - min_gain));
    }

    const float boost = 3.0f;
    return raw_force * gain * boost;
}

bool WheelDevice::ApplySteeringLocked() {
    float combined = user_steering + ffb_offset;
    combined = std::clamp(combined, -32768.0f, 32767.0f);
    if (std::fabs(combined - steering) < 0.1f) {
        return false;
    }
    steering = combined;
    return true;
}

uint32_t WheelDevice::BuildButtonBitsLocked() const {
    uint32_t bits = 0;
    for (size_t i = 0; i < button_states.size(); ++i) {
        if (button_states[i]) {
            bits |= (1u << i);
        }
    }
    return bits;
}
