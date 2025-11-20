#include "gamepad.h"
#include "input.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <string>
#include <thread>
#include <unistd.h>

#include <linux/input-event-codes.h>

extern std::atomic<bool> running;

namespace {

constexpr uint16_t kVendorId = 0x046d;   // Logitech
constexpr uint16_t kProductId = 0xc24f;  // G29 Racing Wheel
constexpr uint16_t kVersion = 0x0111;
constexpr size_t kFFBPacketSize = 7;

// Logitech G29 HID Report Descriptor (26 buttons + OUTPUT report for FFB)
constexpr uint8_t kG29HidDescriptor[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x04,        // Usage (Joystick)
    0xA1, 0x01,        // Collection (Application)
    0xA1, 0x02,        //   Collection (Logical)
    0x09, 0x01,        //     Usage (Pointer)
    0xA1, 0x00,        //     Collection (Physical)
    0x09, 0x30,        //       Usage (X) - Steering
    0x09, 0x31,        //       Usage (Y) - Clutch
    0x09, 0x32,        //       Usage (Z) - Throttle
    0x09, 0x35,        //       Usage (Rz) - Brake
    0x15, 0x00,        //       Logical Minimum (0)
    0x27, 0xFF, 0xFF, 0x00, 0x00,  //       Logical Maximum (65535)
    0x35, 0x00,        //       Physical Minimum (0)
    0x47, 0xFF, 0xFF, 0x00, 0x00,  //       Physical Maximum (65535)
    0x75, 0x10,        //       Report Size (16)
    0x95, 0x04,        //       Report Count (4)
    0x81, 0x02,        //       Input (Data,Var,Abs)
    0xC0,              //     End Collection
    0x09, 0x39,        //     Usage (Hat switch)
    0x15, 0x00,        //     Logical Minimum (0)
    0x25, 0x07,        //     Logical Maximum (7)
    0x35, 0x00,        //     Physical Minimum (0)
    0x46, 0x3B, 0x01,  //     Physical Maximum (315)
    0x65, 0x14,        //     Unit (Degrees)
    0x75, 0x04,        //     Report Size (4)
    0x95, 0x01,        //     Report Count (1)
    0x81, 0x42,        //     Input (Data,Var,Abs,Null)
    0x75, 0x04,        //     Report Size (4)
    0x95, 0x01,        //     Report Count (1)
    0x81, 0x03,        //     Input (Const,Var,Abs)
    0x05, 0x09,        //     Usage Page (Button)
    0x19, 0x01,        //     Usage Minimum (Button 1)
    0x29, 0x1A,        //     Usage Maximum (Button 26)
    0x15, 0x00,        //     Logical Minimum (0)
    0x25, 0x01,        //     Logical Maximum (1)
    0x75, 0x01,        //     Report Size (1)
    0x95, 0x1A,        //     Report Count (26)
    0x81, 0x02,        //     Input (Data,Var,Abs)
    0x75, 0x06,        //     Report Size (6) (padding to next byte)
    0x95, 0x01,        //     Report Count (1)
    0x81, 0x03,        //     Input (Const,Var,Abs)
    0xC0,              //   End Collection (Logical)
    0xA1, 0x02,        //   Collection (Logical)
    0x09, 0x02,        //     Usage (0x02) - FFB usage
    0x15, 0x00,        //     Logical Minimum (0)
    0x26, 0xFF, 0x00,  //     Logical Maximum (255)
    0x95, 0x07,        //     Report Count (7)
    0x75, 0x08,        //     Report Size (8)
    0x91, 0x02,        //     Output (Data,Var,Abs)
    0xC0,              //   End Collection (Logical)
    0xC0               // End Collection (Application)
};

constexpr const char* kGadgetName = "g29wheel";
constexpr const char* kHidFunction = "hid.usb0";
constexpr const char* kHidDevice = "/dev/hidg0";

std::string HexValue(uint16_t value) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%04x", value);
    return std::string(buf);
}

}  // namespace

void GamepadDevice::NotifyAllShutdownCVs() {
    state_cv.notify_all();
    ffb_cv.notify_all();
}

GamepadDevice::GamepadDevice()
    : fd(-1), gadget_running(false), gadget_output_running(false),
      enabled(false), steering(0.0f), user_steering(0.0f), ffb_offset(0.0f),
      ffb_velocity(0.0f), ffb_gain(1.0f), throttle(0.0f), brake(0.0f),
      clutch(0.0f), dpad_x(0), dpad_y(0), ffb_force(0),
      ffb_autocenter(0), gadget_output_pending_len(0) {
    ffb_running = false;
    state_dirty = false;
    button_states.fill(0);
}

GamepadDevice::~GamepadDevice() {
    ShutdownThreads();
}

void GamepadDevice::ShutdownThreads() {
    ffb_running = false;
    gadget_running = false;
    gadget_output_running = false;

    state_cv.notify_all();
    ffb_cv.notify_all();

    if (gadget_thread.joinable()) {
        gadget_thread.join();
    }
    if (gadget_output_thread.joinable()) {
        gadget_output_thread.join();
    }
    if (ffb_thread.joinable()) {
        ffb_thread.join();
    }

    if (fd >= 0) {
        close(fd);
        fd = -1;
    }

    DestroyUSBGadget();
}

bool GamepadDevice::Create() {
    std::cout << "Attempting to create device using USB Gadget (real USB device)..." << std::endl;
    if (!CreateUSBGadget()) {
        std::cerr << "USB Gadget creation failed; wheel emulator requires a USB gadget capable kernel" << std::endl;
        std::cerr << "Ensure configfs is mounted, libcomposite/dummy_hcd modules are available, and a UDC is present." << std::endl;
        return false;
    }

    gadget_running = true;
    gadget_thread = std::thread(&GamepadDevice::USBGadgetPollingThread, this);
    gadget_output_running = true;
    gadget_output_thread = std::thread(&GamepadDevice::USBGadgetOutputThread, this);
    ffb_running = true;
    ffb_thread = std::thread(&GamepadDevice::FFBUpdateThread, this);

    SendNeutral();
    return true;
}

bool GamepadDevice::CreateUSBGadget() {
    system("modprobe libcomposite 2>/dev/null");
    system("modprobe dummy_hcd 2>/dev/null");
    usleep(100000);

    if (access("/sys/kernel/config", F_OK) != 0) {
        system("mkdir -p /sys/kernel/config 2>/dev/null");
        system("mount -t configfs none /sys/kernel/config 2>/dev/null");
    }

    if (access("/sys/kernel/config/usb_gadget", F_OK) != 0) {
        std::cerr << "USB Gadget ConfigFS not available in kernel" << std::endl;
        std::cerr << "Kernel needs CONFIG_USB_CONFIGFS=y" << std::endl;
        return false;
    }

    if (access("/sys/class/udc", F_OK) != 0) {
        std::cerr << "No USB Device Controller (UDC) found" << std::endl;
        std::cerr << "Load dummy_hcd: sudo modprobe dummy_hcd" << std::endl;
        return false;
    }

    const std::string gadget_name(kGadgetName);
    const std::string hid_function(kHidFunction);

    std::string cleanup = "cd /sys/kernel/config/usb_gadget 2>/dev/null && ";
    cleanup += "if [ -d " + gadget_name + " ]; then ";
    cleanup += "  cd " + gadget_name + " && ";
    cleanup += "  echo '' > UDC 2>/dev/null || true; ";
    cleanup += "  rm -f configs/c.1/" + hid_function + " 2>/dev/null || true; ";
    cleanup += "  rmdir configs/c.1/strings/0x409 2>/dev/null || true; ";
    cleanup += "  rmdir configs/c.1 2>/dev/null || true; ";
    cleanup += "  rmdir functions/" + hid_function + " 2>/dev/null || true; ";
    cleanup += "  rmdir strings/0x409 2>/dev/null || true; ";
    cleanup += "  cd .. && rmdir " + gadget_name + " 2>/dev/null || true; ";
    cleanup += "fi";
    system(cleanup.c_str());

    std::string descriptor_hex;
    descriptor_hex.reserve(sizeof(kG29HidDescriptor) * 4);
    for (uint8_t byte : kG29HidDescriptor) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "\\x%02x", byte);
        descriptor_hex += buf;
    }

    const std::string vendor_hex = HexValue(kVendorId);
    const std::string product_hex = HexValue(kProductId);
    const std::string version_hex = HexValue(kVersion);

    std::string cmd = "cd /sys/kernel/config/usb_gadget && ";
    cmd += "mkdir " + gadget_name + " && cd " + gadget_name + " && ";
    cmd += "echo 0x" + vendor_hex + " > idVendor && ";
    cmd += "echo 0x" + product_hex + " > idProduct && ";
    cmd += "echo 0x" + version_hex + " > bcdDevice && ";
    cmd += "echo 0x0200 > bcdUSB && ";
    cmd += "mkdir -p strings/0x409 && ";
    cmd += "echo 'Logitech' > strings/0x409/manufacturer && ";
    cmd += "echo 'G29 Driving Force Racing Wheel' > strings/0x409/product && ";
    cmd += "echo '000000000001' > strings/0x409/serialnumber && ";
    cmd += "mkdir -p functions/" + hid_function + " && cd functions/" + hid_function + " && ";
    cmd += "echo 1 > protocol && echo 1 > subclass && echo 13 > report_length && ";
    cmd += "printf '" + descriptor_hex + "' > report_desc && ";
    cmd += "cd /sys/kernel/config/usb_gadget/" + gadget_name + " && ";
    cmd += "mkdir -p configs/c.1/strings/0x409 && ";
    cmd += "echo 'G29 Configuration' > configs/c.1/strings/0x409/configuration && ";
    cmd += "echo 500 > configs/c.1/MaxPower && ";
    cmd += "ln -s functions/" + hid_function + " configs/c.1/ && ";
    cmd += "UDC=$(ls /sys/class/udc 2>/dev/null | head -n1) && ";
    cmd += "if [ -n \"$UDC\" ]; then echo $UDC > UDC; fi";

    if (system(cmd.c_str()) != 0) {
        std::cerr << "Failed to setup USB Gadget" << std::endl;
        return false;
    }

    usleep(500000);

    fd = open(kHidDevice, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        std::cerr << "USB Gadget configured but failed to open " << kHidDevice << std::endl;
        return false;
    }

    std::cout << "USB Gadget device created successfully!" << std::endl;
    std::cout << "Real USB Logitech G29 device (VID:046d PID:c24f)" << std::endl;
    return true;
}

void GamepadDevice::DestroyUSBGadget() {
    const std::string gadget_name(kGadgetName);
    const std::string hid_function(kHidFunction);

    std::string cleanup = "cd /sys/kernel/config/usb_gadget 2>/dev/null && ";
    cleanup += "if [ -d " + gadget_name + " ]; then ";
    cleanup += "  cd " + gadget_name + " && ";
    cleanup += "  echo '' > UDC 2>/dev/null || true; ";
    cleanup += "  rm -f configs/c.1/" + hid_function + " 2>/dev/null || true; ";
    cleanup += "  rmdir configs/c.1/strings/0x409 2>/dev/null || true; ";
    cleanup += "  rmdir configs/c.1 2>/dev/null || true; ";
    cleanup += "  rmdir functions/" + hid_function + " 2>/dev/null || true; ";
    cleanup += "  cd .. && rmdir " + gadget_name + " 2>/dev/null || true; ";
    cleanup += "fi";
    system(cleanup.c_str());
}

bool GamepadDevice::IsEnabled() {
    std::lock_guard<std::mutex> lock(state_mutex);
    return enabled;
}

void GamepadDevice::SetEnabled(bool enable, Input& input) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        if (enabled != enable) {
            enabled = enable;
            changed = true;
        }
    }
    if (!changed) {
        return;
    }

    input.Grab(enable);
    if (!enable) {
        input.ResetState();
    }
    SendNeutral();
    std::cout << (enable ? "Emulation ENABLED" : "Emulation DISABLED") << std::endl;
}

void GamepadDevice::ToggleEnabled(Input& input) {
    bool next_state;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        next_state = !enabled;
    }
    SetEnabled(next_state, input);
}

void GamepadDevice::SetFFBGain(float gain) {
    if (gain < 0.1f) {
        gain = 0.1f;
    } else if (gain > 4.0f) {
        gain = 4.0f;
    }
    std::lock_guard<std::mutex> lock(state_mutex);
    ffb_gain = gain;
}

void GamepadDevice::UpdateSteering(int delta, int sensitivity) {
    if (delta == 0) {
        return;
    }

    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        constexpr float base_gain = 0.05f;
        const float gain = static_cast<float>(sensitivity) * base_gain;
        const float max_step = 2000.0f;
        float step = delta * gain;
        step = std::clamp(step, -max_step, max_step);
        user_steering += step;
        const float max_angle = 32767.0f;
        user_steering = std::clamp(user_steering, -max_angle, max_angle);
        changed = ApplySteeringLocked();
    }

    if (changed) {
        NotifyStateChanged();
    }
}

void GamepadDevice::UpdateThrottle(bool pressed, float) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        float next = pressed ? 100.0f : 0.0f;
        if (throttle != next) {
            throttle = next;
            changed = true;
        }
    }
    if (changed) {
        NotifyStateChanged();
    }
}

void GamepadDevice::UpdateBrake(bool pressed, float) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        float next = pressed ? 100.0f : 0.0f;
        if (brake != next) {
            brake = next;
            changed = true;
        }
    }
    if (changed) {
        NotifyStateChanged();
    }
}

void GamepadDevice::UpdateClutch(bool pressed, float) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        float next = pressed ? 100.0f : 0.0f;
        if (clutch != next) {
            clutch = next;
            changed = true;
        }
    }
    if (changed) {
        NotifyStateChanged();
    }
}

void GamepadDevice::UpdateButtons(const Input& input) {
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        SetButton(WheelButton::South, input.IsKeyPressed(KEY_Q));
        SetButton(WheelButton::East, input.IsKeyPressed(KEY_E));
        SetButton(WheelButton::West, input.IsKeyPressed(KEY_F));
        SetButton(WheelButton::North, input.IsKeyPressed(KEY_G));
        SetButton(WheelButton::TL, input.IsKeyPressed(KEY_H));
        SetButton(WheelButton::TR, input.IsKeyPressed(KEY_R));
        SetButton(WheelButton::TL2, input.IsKeyPressed(KEY_T));
        SetButton(WheelButton::TR2, input.IsKeyPressed(KEY_Y));
        SetButton(WheelButton::Select, input.IsKeyPressed(KEY_U));
        SetButton(WheelButton::Start, input.IsKeyPressed(KEY_I));
        SetButton(WheelButton::ThumbL, input.IsKeyPressed(KEY_O));
        SetButton(WheelButton::ThumbR, input.IsKeyPressed(KEY_P));
        SetButton(WheelButton::Mode, input.IsKeyPressed(KEY_1));
        SetButton(WheelButton::Dead, input.IsKeyPressed(KEY_2));
        SetButton(WheelButton::TriggerHappy1, input.IsKeyPressed(KEY_3));
        SetButton(WheelButton::TriggerHappy2, input.IsKeyPressed(KEY_4));
        SetButton(WheelButton::TriggerHappy3, input.IsKeyPressed(KEY_5));
        SetButton(WheelButton::TriggerHappy4, input.IsKeyPressed(KEY_6));
        SetButton(WheelButton::TriggerHappy5, input.IsKeyPressed(KEY_7));
        SetButton(WheelButton::TriggerHappy6, input.IsKeyPressed(KEY_8));
        SetButton(WheelButton::TriggerHappy7, input.IsKeyPressed(KEY_9));
        SetButton(WheelButton::TriggerHappy8, input.IsKeyPressed(KEY_0));
        SetButton(WheelButton::TriggerHappy9, input.IsKeyPressed(KEY_LEFTSHIFT));
        SetButton(WheelButton::TriggerHappy10, input.IsKeyPressed(KEY_SPACE));
        SetButton(WheelButton::TriggerHappy11, input.IsKeyPressed(KEY_TAB));
        SetButton(WheelButton::TriggerHappy12, input.IsKeyPressed(KEY_ENTER));
    }
    NotifyStateChanged();
}

void GamepadDevice::UpdateDPad(const Input& input) {
    std::lock_guard<std::mutex> lock(state_mutex);

    int right = input.IsKeyPressed(KEY_RIGHT) ? 1 : 0;
    int left = input.IsKeyPressed(KEY_LEFT) ? 1 : 0;
    int down = input.IsKeyPressed(KEY_DOWN) ? 1 : 0;
    int up = input.IsKeyPressed(KEY_UP) ? 1 : 0;

    dpad_x = right - left;
    dpad_y = down - up;
}

void GamepadDevice::SendState() {
    if (fd < 0) {
        return;
    }
    NotifyStateChanged();
}

void GamepadDevice::SendNeutral() {
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        steering = 0.0f;
        user_steering = 0.0f;
        ffb_offset = 0.0f;
        ffb_velocity = 0.0f;
        throttle = 0.0f;
        brake = 0.0f;
        clutch = 0.0f;
        dpad_x = 0;
        dpad_y = 0;
        button_states.fill(0);
    }

    if (fd >= 0) {
        NotifyStateChanged();
    }
}

void GamepadDevice::NotifyStateChanged() {
    state_dirty.store(true, std::memory_order_release);
    state_cv.notify_all();
    ffb_cv.notify_all();
}

std::array<uint8_t, 13> GamepadDevice::BuildHIDReport() {
    std::lock_guard<std::mutex> lock(state_mutex);

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

void GamepadDevice::SendGadgetReport() {
    auto report_data = BuildHIDReport();
    WriteHIDBlocking(report_data.data(), report_data.size());
}

bool GamepadDevice::WriteHIDBlocking(const uint8_t* data, size_t size) {
    if (fd < 0) {
        return false;
    }

    size_t written = 0;
    while (written < size) {
        ssize_t ret = write(fd, data + written, size - written);
        if (ret > 0) {
            written += static_cast<size_t>(ret);
            continue;
        }

        if (ret == -1) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                pollfd p{};
                p.fd = fd;
                p.events = POLLOUT;
                int poll_ret = poll(&p, 1, 5);
                if (poll_ret <= 0) {
                    if (poll_ret == -1 && errno == EINTR) {
                        continue;
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(500));
                    continue;
                }
                continue;
            }
        }
        return false;
    }
    return true;
}

void GamepadDevice::USBGadgetPollingThread() {
    if (fd < 0) {
        return;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    std::unique_lock<std::mutex> lock(state_mutex);
    while (gadget_running && running) {
        state_cv.wait_for(lock, std::chrono::milliseconds(2), [&] {
            return !gadget_running || !running || state_dirty.load(std::memory_order_acquire);
        });
        if (!gadget_running || !running) {
            break;
        }
        bool should_send = state_dirty.exchange(false, std::memory_order_acq_rel);
        lock.unlock();
        if (should_send) {
            SendGadgetReport();
        }
        lock.lock();
    }
}

void GamepadDevice::USBGadgetOutputThread() {
    if (fd < 0) {
        return;
    }

    while (gadget_output_running && running) {
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
            break;
        }
        if (ret == 0) {
            continue;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            break;
        }
        if (pfd.revents & POLLIN) {
            ReadGadgetOutput();
        }
    }
}

void GamepadDevice::ReadGadgetOutput() {
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
                ParseFFBCommand(gadget_output_pending.data(), kFFBPacketSize);
                gadget_output_pending_len = 0;
            }
        }
    }
}

void GamepadDevice::FFBUpdateThread() {
    float filtered_ffb = 0.0f;
    using clock = std::chrono::steady_clock;
    auto last = clock::now();

    while (true) {
        std::unique_lock<std::mutex> lock(state_mutex);
        ffb_cv.wait_for(lock, std::chrono::milliseconds(1));
        if (!ffb_running || !running) {
            break;
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

void GamepadDevice::ParseFFBCommand(const uint8_t* data, size_t size) {
    if (size != kFFBPacketSize) {
        return;
    }

    std::lock_guard<std::mutex> lock(state_mutex);
    bool state_changed = false;

    uint8_t cmd = data[0];

    switch (cmd) {
        case 0x11: {
            int8_t force = static_cast<int8_t>(data[2]) - 0x80;
            ffb_force = static_cast<int16_t>(-force) * 48;
            state_changed = true;
            break;
        }
        case 0x13:
            ffb_force = 0;
            state_changed = true;
            break;
        case 0xf5:
            ffb_autocenter = 0;
            state_changed = true;
            break;
        case 0xfe:
            if (data[1] == 0x0d) {
                ffb_autocenter = static_cast<int16_t>(data[2]) * 16;
                state_changed = true;
            }
            break;
        case 0x14:
            if (ffb_autocenter == 0) {
                ffb_autocenter = 1024;
                state_changed = true;
            }
            break;
        case 0xf8:
            switch (data[1]) {
                case 0x81:
                case 0x12:
                case 0x09:
                case 0x0a:
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

float GamepadDevice::ShapeFFBTorque(float raw_force) const {
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

bool GamepadDevice::ApplySteeringLocked() {
    float combined = user_steering + ffb_offset;
    combined = std::clamp(combined, -32768.0f, 32767.0f);
    if (std::fabs(combined - steering) < 0.1f) {
        return false;
    }
    steering = combined;
    return true;
}

void GamepadDevice::SetButton(WheelButton button, bool pressed) {
    button_states[static_cast<size_t>(button)] = pressed ? 1 : 0;
}

uint32_t GamepadDevice::BuildButtonBitsLocked() const {
    uint32_t bits = 0;
    for (size_t i = 0; i < button_states.size(); ++i) {
        if (button_states[i]) {
            bits |= (1u << i);
        }
    }
    return bits;
}
