#include "gamepad.h"
#include "input.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
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

std::string ReadTrimmedFile(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        return {};
    }
    std::string line;
    std::getline(in, line);
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) {
        line.pop_back();
    }
    return line;
}

bool WriteStringToFile(const std::string& path, const std::string& value) {
    int fd = open(path.c_str(), O_WRONLY | O_TRUNC);
    if (fd < 0) {
        return false;
    }
    std::string payload = value;
    payload.push_back('\n');
    ssize_t written = write(fd, payload.data(), payload.size());
    close(fd);
    return written == static_cast<ssize_t>(payload.size());
}

}  // namespace

struct GamepadDevice::ControlSnapshot {
    bool throttle = false;
    bool brake = false;
    bool clutch = false;
    int8_t dpad_x = 0;
    int8_t dpad_y = 0;
    std::array<uint8_t, static_cast<size_t>(WheelButton::Count)> buttons{};
};

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
        warmup_frames.store(0, std::memory_order_relaxed);
    output_enabled.store(false, std::memory_order_relaxed);
    udc_bound = false;
    button_states.fill(0);
}

GamepadDevice::~GamepadDevice() {
    ShutdownThreads();
}

void GamepadDevice::ShutdownThreads() {
    ffb_running = false;
    gadget_running = false;
    gadget_output_running = false;
        warmup_frames.store(0, std::memory_order_relaxed);
    output_enabled.store(false, std::memory_order_relaxed);
    UnbindUDC();

    state_cv.notify_all();
    ffb_cv.notify_all();

    StopGadgetThreads();
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

    ffb_running = true;
    ffb_thread = std::thread(&GamepadDevice::FFBUpdateThread, this);

    SendNeutral(true);
    UnbindUDC();
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

    fd = open(kHidDevice, O_RDWR);
    if (fd < 0) {
        std::cerr << "USB Gadget configured but failed to open " << kHidDevice << std::endl;
        return false;
    }

    {
        std::lock_guard<std::mutex> guard(gadget_mutex);
        udc_name = ReadTrimmedFile(GadgetUDCPath());
        udc_bound = !udc_name.empty();
    }

    std::cout << "USB Gadget device created successfully!" << std::endl;
    std::cout << "Real USB Logitech G29 device (VID:046d PID:c24f)" << std::endl;
    return true;
}

void GamepadDevice::DestroyUSBGadget() {
    UnbindUDC();
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

std::string GamepadDevice::GadgetUDCPath() const {
    return std::string("/sys/kernel/config/usb_gadget/") + kGadgetName + "/UDC";
}

std::string GamepadDevice::DetectFirstUDC() const {
    DIR* dir = opendir("/sys/class/udc");
    if (!dir) {
        return {};
    }
    std::string result;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        result = entry->d_name;
        break;
    }
    closedir(dir);
    return result;
}

bool GamepadDevice::BindUDC() {
    std::lock_guard<std::mutex> guard(gadget_mutex);
    if (udc_bound) {
        return true;
    }
    if (udc_name.empty()) {
        std::string current = ReadTrimmedFile(GadgetUDCPath());
        if (!current.empty()) {
            udc_name = current;
        } else {
            udc_name = DetectFirstUDC();
        }
        if (udc_name.empty()) {
            std::cerr << "[GamepadDevice] No UDC available to bind" << std::endl;
            return false;
        }
    }

    if (!WriteStringToFile(GadgetUDCPath(), udc_name)) {
        std::cerr << "[GamepadDevice] Failed to bind UDC '" << udc_name << "'" << std::endl;
        return false;
    }
    udc_bound = true;
    std::cout << "[GamepadDevice] Bound UDC '" << udc_name << "'" << std::endl;

    if (fd >= 0) {
        close(fd);
    }
    fd = open(kHidDevice, O_RDWR);
    if (fd < 0) {
        std::cerr << "[GamepadDevice] Failed to open " << kHidDevice
                  << " after binding: " << strerror(errno) << std::endl;
        WriteStringToFile(GadgetUDCPath(), "");
        udc_bound = false;
        return false;
    }
    return true;
}

bool GamepadDevice::UnbindUDC() {
    std::lock_guard<std::mutex> guard(gadget_mutex);
    if (!udc_bound) {
        return true;
    }
    if (!WriteStringToFile(GadgetUDCPath(), "")) {
        std::cerr << "[GamepadDevice] Failed to unbind UDC" << std::endl;
        return false;
    }
    udc_bound = false;
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
    StopGadgetThreads();
    std::cout << "[GamepadDevice] Unbound UDC" << std::endl;
    return true;
}

void GamepadDevice::EnsureGadgetThreadsStarted() {
    if (fd >= 0) {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0 && !(flags & O_NONBLOCK)) {
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }
    }
    if (!gadget_running) {
        gadget_running = true;
        gadget_thread = std::thread(&GamepadDevice::USBGadgetPollingThread, this);
    }
    if (!gadget_output_running) {
        gadget_output_running = true;
        gadget_output_thread = std::thread(&GamepadDevice::USBGadgetOutputThread, this);
    }
}

void GamepadDevice::StopGadgetThreads() {
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

bool GamepadDevice::IsEnabled() {
    std::lock_guard<std::mutex> lock(state_mutex);
    return enabled;
}

GamepadDevice::ControlSnapshot GamepadDevice::CaptureSnapshot(const Input& input) const {
    ControlSnapshot snapshot;
    snapshot.throttle = input.IsKeyPressed(KEY_W);
    snapshot.brake = input.IsKeyPressed(KEY_S);
    snapshot.clutch = input.IsKeyPressed(KEY_A);

    int right = input.IsKeyPressed(KEY_RIGHT) ? 1 : 0;
    int left = input.IsKeyPressed(KEY_LEFT) ? 1 : 0;
    int down = input.IsKeyPressed(KEY_DOWN) ? 1 : 0;
    int up = input.IsKeyPressed(KEY_UP) ? 1 : 0;
    snapshot.dpad_x = static_cast<int8_t>(right - left);
    snapshot.dpad_y = static_cast<int8_t>(down - up);

    auto set_button = [&](WheelButton button, bool pressed) {
        snapshot.buttons[static_cast<size_t>(button)] = pressed ? 1 : 0;
    };

    set_button(WheelButton::South, input.IsKeyPressed(KEY_Q));
    set_button(WheelButton::East, input.IsKeyPressed(KEY_E));
    set_button(WheelButton::West, input.IsKeyPressed(KEY_F));
    set_button(WheelButton::North, input.IsKeyPressed(KEY_G));
    set_button(WheelButton::TL, input.IsKeyPressed(KEY_H));
    set_button(WheelButton::TR, input.IsKeyPressed(KEY_R));
    set_button(WheelButton::TL2, input.IsKeyPressed(KEY_T));
    set_button(WheelButton::TR2, input.IsKeyPressed(KEY_Y));
    set_button(WheelButton::Select, input.IsKeyPressed(KEY_U));
    set_button(WheelButton::Start, input.IsKeyPressed(KEY_I));
    set_button(WheelButton::ThumbL, input.IsKeyPressed(KEY_O));
    set_button(WheelButton::ThumbR, input.IsKeyPressed(KEY_P));
    set_button(WheelButton::Mode, input.IsKeyPressed(KEY_1));
    set_button(WheelButton::Dead, input.IsKeyPressed(KEY_2));
    set_button(WheelButton::TriggerHappy1, input.IsKeyPressed(KEY_3));
    set_button(WheelButton::TriggerHappy2, input.IsKeyPressed(KEY_4));
    set_button(WheelButton::TriggerHappy3, input.IsKeyPressed(KEY_5));
    set_button(WheelButton::TriggerHappy4, input.IsKeyPressed(KEY_6));
    set_button(WheelButton::TriggerHappy5, input.IsKeyPressed(KEY_7));
    set_button(WheelButton::TriggerHappy6, input.IsKeyPressed(KEY_8));
    set_button(WheelButton::TriggerHappy7, input.IsKeyPressed(KEY_9));
    set_button(WheelButton::TriggerHappy8, input.IsKeyPressed(KEY_0));
    set_button(WheelButton::TriggerHappy9, input.IsKeyPressed(KEY_LEFTSHIFT));
    set_button(WheelButton::TriggerHappy10, input.IsKeyPressed(KEY_SPACE));
    set_button(WheelButton::TriggerHappy11, input.IsKeyPressed(KEY_TAB));
    set_button(WheelButton::TriggerHappy12, input.IsKeyPressed(KEY_ENTER));

    return snapshot;
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
        if (!enable) {
            input.Grab(false);
        }
        return;
    }

    if (enable) {
        if (!input.Grab(true)) {
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                enabled = false;
            }
            std::cerr << "Enable aborted: unable to grab keyboard/mouse" << std::endl;
            return;
        }
        if (!input.AllRequiredGrabbed()) {
            input.Grab(false);
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                enabled = false;
            }
            std::cerr << "Enable aborted: missing required input device" << std::endl;
            return;
        }

        input.ResyncKeyStates();
        ControlSnapshot snapshot = CaptureSnapshot(input);

        output_enabled.store(false, std::memory_order_release);
        warmup_frames.store(0, std::memory_order_release);
        state_dirty.store(false, std::memory_order_release);

        std::array<uint8_t, 13> neutral_report;
        std::array<uint8_t, 13> snapshot_report;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            ApplyNeutralLocked(false);
            neutral_report = BuildHIDReportLocked();
            ApplySnapshotLocked(snapshot);
            snapshot_report = BuildHIDReportLocked();
        }

        if (!BindUDC()) {
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                ApplyNeutralLocked(true);
                enabled = false;
            }
            input.Grab(false);
            return;
        }

        if (!WriteReportBlocking(neutral_report) || !WriteReportBlocking(snapshot_report)) {
            std::cerr << "[GamepadDevice] Failed to prime HID reports; disconnecting" << std::endl;
            UnbindUDC();
            input.Grab(false);
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                enabled = false;
            }
            return;
        }

        EnsureGadgetThreadsStarted();
        output_enabled.store(true, std::memory_order_release);
        warmup_frames.store(25, std::memory_order_release);
        state_cv.notify_all();
    } else {
        warmup_frames.store(0, std::memory_order_release);
        output_enabled.store(false, std::memory_order_release);
        state_dirty.store(false, std::memory_order_release);

        std::array<uint8_t, 13> neutral_report;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            ApplyNeutralLocked(true);
            neutral_report = BuildHIDReportLocked();
        }
        if (!WriteReportBlocking(neutral_report)) {
            std::cerr << "[GamepadDevice] Failed to send neutral frame while disabling" << std::endl;
        }
        input.Grab(false);
        UnbindUDC();
    }
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

void GamepadDevice::ProcessInputFrame(int mouse_dx, int sensitivity, const Input& input) {
    if (!enabled || !output_enabled.load(std::memory_order_acquire)) {
        return;
    }
    ControlSnapshot snapshot = CaptureSnapshot(input);
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        changed |= ApplySteeringDeltaLocked(mouse_dx, sensitivity);
        changed |= ApplySnapshotLocked(snapshot);
    }
    if (changed) {
        NotifyStateChanged();
    }
}

void GamepadDevice::ApplyCurrentInput(const Input& input) {
    ControlSnapshot snapshot = CaptureSnapshot(input);
    ApplySnapshot(snapshot);
}

void GamepadDevice::ApplySnapshot(const ControlSnapshot& snapshot) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        changed = ApplySnapshotLocked(snapshot);
    }
    if (changed) {
        NotifyStateChanged();
    }
}

void GamepadDevice::SendNeutral(bool reset_ffb) {
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        ApplyNeutralLocked(reset_ffb);
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

bool GamepadDevice::ApplySteeringDeltaLocked(int delta, int sensitivity) {
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

bool GamepadDevice::ApplySnapshotLocked(const ControlSnapshot& snapshot) {
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

void GamepadDevice::ApplyNeutralLocked(bool reset_ffb) {
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


std::array<uint8_t, 13> GamepadDevice::BuildHIDReport() {
    std::lock_guard<std::mutex> lock(state_mutex);
    return BuildHIDReportLocked();
}

std::array<uint8_t, 13> GamepadDevice::BuildHIDReportLocked() const {
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

bool GamepadDevice::WriteReportBlocking(const std::array<uint8_t, 13>& report) {
    constexpr auto kMaxWait = std::chrono::milliseconds(2000);
    auto deadline = std::chrono::steady_clock::now() + kMaxWait;
    int attempt = 0;

    while (running && std::chrono::steady_clock::now() < deadline) {
        if (fd >= 0 && WriteHIDBlocking(report.data(), report.size())) {
            return true;
        }

        int err = errno;
        ++attempt;
        std::cerr << "[GamepadDevice] Failed to write HID report (attempt " << attempt
                  << ", errno=" << err << " " << strerror(err) << ")" << std::endl;

        if (fd >= 0) {
            close(fd);
            fd = -1;
        }

        if (!running) {
            break;
        }

        // Host not ready yet; allow some time before retrying.
        if (err == EPIPE || err == ENODEV || err == ESHUTDOWN) {
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        fd = open(kHidDevice, O_RDWR);
        if (fd < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        if (gadget_running.load(std::memory_order_relaxed) ||
            gadget_output_running.load(std::memory_order_relaxed)) {
            int flags = fcntl(fd, F_GETFL, 0);
            if (flags >= 0 && !(flags & O_NONBLOCK)) {
                fcntl(fd, F_SETFL, flags | O_NONBLOCK);
            }
        }
    }
    return false;
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
                if (output_enabled.load(std::memory_order_acquire)) {
                    ParseFFBCommand(gadget_output_pending.data(), kFFBPacketSize);
                }
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

void GamepadDevice::ParseFFBCommand(const uint8_t* data, size_t size) {
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

uint32_t GamepadDevice::BuildButtonBitsLocked() const {
    uint32_t bits = 0;
    for (size_t i = 0; i < button_states.size(); ++i) {
        if (button_states[i]) {
            bits |= (1u << i);
        }
    }
    return bits;
}
