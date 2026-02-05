#include "wheel_device.h"
#include "input/input_manager.h"
#include "hid/vjoy_loader.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>

#include "logging/logger.h"
#include <windows.h>

namespace {
constexpr size_t kFFBPacketSize = 7;
constexpr const char* kTag = "wheel_device";
}

// vJoy FFB Callback Wrapper
static void CALLBACK FFB_Callback(PVOID data, PVOID user_data) {
    if (user_data) {
        static_cast<WheelDevice*>(user_data)->OnFFBPacket(data);
    }
}

void WheelDevice::NotifyAllShutdownCVs() {
    state_cv.notify_all();
    ffb_cv.notify_all();
}

WheelDevice::WheelDevice()
        : polling_running_(false),
          enabled(false), steering(0.0f), user_steering(0.0f), ffb_offset(0.0f),
          ffb_velocity(0.0f), ffb_gain(1.0f), throttle(0.0f), brake(0.0f),
          clutch(0.0f), dpad_x(0), dpad_y(0), ffb_force(0),
          ffb_autocenter(0) {
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
    polling_running_ = false;
    warmup_frames.store(0, std::memory_order_relaxed);
    output_enabled.store(false, std::memory_order_relaxed);

    state_cv.notify_all();
    ffb_cv.notify_all();

    StopPollingThread();
    if (ffb_thread.joinable()) {
        ffb_thread.join();
    }
}

bool WheelDevice::Create() {
    LOG_DEBUG(kTag, "Attempting to create device using vJoy...");
    
    // Windows Integration
    if (!hid_device_.Initialize()) {
        std::cerr << "vJoy creation failed Check vJoy is installed and enabled." << std::endl;
        return false;
    }

    // Register FFB Callback
    hid_device_.RegisterFFBCallback((void*)FFB_Callback, this);

    SendNeutral(true);

    ffb_running = true;
    ffb_thread = std::thread(&WheelDevice::FFBUpdateThread, this);
    return true;
}

void WheelDevice::EnsurePollingThreadStarted() {
    if (!polling_running_) {
        polling_running_ = true;
        polling_thread_ = std::thread(&WheelDevice::VJoyPollingThread, this);
    }
}

void WheelDevice::StopPollingThread() {
    if (polling_running_) {
        polling_running_ = false;
        state_cv.notify_all();
    }
    if (polling_thread_.joinable()) {
        polling_thread_.join();
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
            std::cerr << "Enable aborted: unable to grab input" << std::endl;
            return;
        }
        
        input_manager.ResyncKeyStates();

        output_enabled.store(false, std::memory_order_release);
        warmup_frames.store(0, std::memory_order_release);
        state_dirty.store(false, std::memory_order_release);

        // Windows: Check vJoy ready
        if (!hid_device_.IsReady() && !hid_device_.Initialize()) {
             {
                std::lock_guard<std::mutex> lock(state_mutex);
                enabled = false;
            }
            input_manager.GrabDevices(false);
            return;
        }

        EnsurePollingThreadStarted();

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
        
        warmup_frames.store(5, std::memory_order_release);
        state_cv.notify_all();
    } else {
        warmup_frames.store(0, std::memory_order_release);
        
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            ApplyNeutralLocked(true);
        }
        state_dirty.store(true, std::memory_order_release);
        state_cv.notify_all();

        output_enabled.store(false, std::memory_order_release);
        state_dirty.store(false, std::memory_order_release);

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
    if (gain < 0.1f) gain = 0.1f;
    else if (gain > 4.0f) gain = 4.0f;
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
    NotifyStateChanged(); 
}

void WheelDevice::NotifyStateChanged() {
    state_dirty.store(true, std::memory_order_release);
    state_cv.notify_all();
    ffb_cv.notify_all();
}

bool WheelDevice::ApplySteeringDeltaLocked(int delta, int sensitivity) {
    if (delta == 0) return false;

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

bool WheelDevice::SendReport() {
    auto report_data = BuildHIDReport();
    return hid_device_.WriteReportBlocking(report_data);
}

void WheelDevice::VJoyPollingThread() {
    std::unique_lock<std::mutex> lock(state_mutex);
    while (polling_running_ && running) {
        state_cv.wait_for(lock, std::chrono::milliseconds(2), [&] {
            return !polling_running_ || !running ||
                   state_dirty.load(std::memory_order_acquire) ||
                   warmup_frames.load(std::memory_order_acquire) > 0;
        });
        if (!polling_running_ || !running) break;

        bool should_send = state_dirty.exchange(false, std::memory_order_acq_rel);
        int pending = warmup_frames.load(std::memory_order_acquire);
        if (pending > 0) {
            warmup_frames.fetch_sub(1, std::memory_order_acq_rel);
            should_send = true;
        }
        
        bool allow_output = output_enabled.load(std::memory_order_acquire);
        lock.unlock();
        
        if (allow_output && should_send) {
            SendReport(); 
        }
        lock.lock();
    }
}

void WheelDevice::OnFFBPacket(void* data) {
    if (!enabled || !data) return;

    FFB_DATA* packet = static_cast<FFB_DATA*>(data);
    FFBPType type = PT_CONSTREP; 
    
    // Using dynamic loader pointer
    if (vJoy.Ffb_h_Type(packet, &type) != ERROR_SUCCESS) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(state_mutex);
    bool state_changed = false;

    switch (type) {
        case PT_CONSTREP: {
            FFB_EFF_CONSTANT effect;
            if (vJoy.Ffb_h_Eff_Constant(packet, &effect) == ERROR_SUCCESS) {
                // vJoy/Game sends 16-bit signed data in a 32-bit field.
                int16_t raw_mag = static_cast<int16_t>(effect.Magnitude & 0xFFFF);

                // Linux Logic: Positive USB Input (Right) -> Negative Internal Force.
                ffb_force = -static_cast<int16_t>((static_cast<int32_t>(raw_mag) * 6096) / 10000);
                
                state_changed = true;
            }
            break;
        }

        case PT_EFOPREP: {
            FFB_EFF_OP op;
            if (vJoy.Ffb_h_EffOp(packet, &op) == ERROR_SUCCESS) {
                if (op.EffectOp == EFF_STOP) {
                    ffb_force = 0;
                    state_changed = true;
                }
            }
            break;
        }
        case PT_CTRLREP: {
            FFB_CTRL control;
            if (vJoy.Ffb_h_DevCtrl(packet, &control) == ERROR_SUCCESS) {
                if (control == CTRL_STOPALL || control == CTRL_DEVRST) {
                    ffb_force = 0;
                    state_changed = true;
                }
            }
            break;
        }
        default:
            break;
    }
    
    if (state_changed) {
        ffb_cv.notify_all();
    }
}

void WheelDevice::FFBUpdateThread() {
    float filtered_ffb = 0.0f;
    using clock = std::chrono::steady_clock;
    auto last = clock::now();

    while (true) {
        std::unique_lock<std::mutex> lock(state_mutex);
        ffb_cv.wait_for(lock, std::chrono::milliseconds(1));
        if (!ffb_running || !running) break;

        if (!enabled) {
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
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
        if (!ffb_running || !running) break;
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

// FFB Torque Shaping
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

    const float boost = 3.0f; // Restored to 3.0f to match Linux config exactly
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
