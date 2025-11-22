// Improved toggle: allow either Ctrl key, and tolerate quick presses
#include "input.h"
#include <iostream>
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <cstring>
#include <dirent.h>
#include <vector>
#include <linux/input-event-codes.h>
#include <atomic>
#include <poll.h>
#include <thread>
extern std::atomic<bool> running;

void Input::Read() {
    int dummy = 0;
    Read(dummy);
}

void Input::NotifyInputChanged() {
    input_cv.notify_all();
}

// Bit manipulation macros for input device capabilities
#define BITS_PER_LONG (sizeof(long) * 8)
#define NBITS(x) ((((x)-1)/BITS_PER_LONG)+1)
#define OFF(x)  ((x)%BITS_PER_LONG)
#define BIT(x)  (1UL<<OFF(x))
#define LONG(x) ((x)/BITS_PER_LONG)
#define test_bit(bit, array)    ((array[LONG(bit)] >> OFF(bit)) & 1)

namespace {
bool DeviceSupportsKeyboard(int fd) {
    unsigned long ev_bits[NBITS(EV_MAX)] = {0};
    if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0) {
        return false;
    }
    if (!test_bit(EV_KEY, ev_bits)) {
        return false;
    }
    unsigned long key_bits[NBITS(KEY_MAX)] = {0};
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0) {
        return false;
    }
    return test_bit(KEY_A, key_bits) || test_bit(KEY_Q, key_bits) ||
           test_bit(KEY_Z, key_bits) || test_bit(KEY_SPACE, key_bits);
}

bool DeviceSupportsMouse(int fd) {
    unsigned long rel_bits[NBITS(REL_MAX)] = {0};
    if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel_bits)), rel_bits) < 0) {
        return false;
    }
    return test_bit(REL_X, rel_bits);
}
}

Input::Input() : resync_pending(true), grab_desired(false), prev_toggle(false) {
    memset(keys, 0, sizeof(keys));
    memset(key_counts, 0, sizeof(key_counts));
    last_scan = std::chrono::steady_clock::time_point::min();
    last_input_activity = std::chrono::steady_clock::time_point::min();
    last_keyboard_error = std::chrono::steady_clock::time_point::min();
    last_mouse_error = std::chrono::steady_clock::time_point::min();
    last_grab_log = std::chrono::steady_clock::time_point::min();
}

Input::~Input() {
    for (auto& dev : devices) {
        CloseDevice(dev);
    }
}

bool Input::DiscoverKeyboard(const std::string& device_path) {
    keyboard_override = device_path;
    last_keyboard_error = std::chrono::steady_clock::time_point::min();
    RefreshDevices();
    if (!device_path.empty() && !FindDevice(device_path)) {
        std::cerr << "Failed to open keyboard device: " << device_path << std::endl;
        return false;
    }
    if (!device_path.empty()) {
        MarkResyncNeeded();
    }
    return true;
}

bool Input::DiscoverMouse(const std::string& device_path) {
    mouse_override = device_path;
    last_mouse_error = std::chrono::steady_clock::time_point::min();
    RefreshDevices();
    if (!device_path.empty() && !FindDevice(device_path)) {
        std::cerr << "Failed to open mouse device: " << device_path << std::endl;
        return false;
    }
    return true;
}

bool Input::WaitForEvents(int timeout_ms) {
    RefreshDevices();
    std::vector<pollfd> pfds;
    pfds.reserve(devices.size());
    for (auto& dev : devices) {
        if (dev.fd >= 0) {
            pollfd p{};
            p.fd = dev.fd;
            p.events = POLLIN;
            pfds.push_back(p);
        }
    }

    if (pfds.empty()) {
        if (timeout_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
        }
        return false;
    }

    int ret = poll(pfds.data(), pfds.size(), timeout_ms);
    if (ret < 0) {
        if (errno == EINTR) {
            return false;
        }
        std::cerr << "[Input::WaitForEvents] poll() error: " << strerror(errno) << std::endl;
        return false;
    }
    return ret > 0;
}

void Input::Read(int& mouse_dx) {
    if (!running) {
        return;
    }

    RefreshDevices();
    mouse_dx = 0;

    for (size_t i = 0; i < devices.size();) {
        if (!DrainDevice(devices[i], mouse_dx)) {
            CloseDevice(devices[i]);
            devices.erase(devices.begin() + i);
        } else {
            ++i;
        }
    }
}

bool Input::DrainDevice(DeviceHandle& dev, int& mouse_dx) {
    if (dev.fd < 0) {
        return false;
    }
    constexpr int kMaxEventsPerDevice = 256;
    int processed = 0;
    struct input_event ev;
    bool keep = true;
    bool had_activity = false;

    while (processed < kMaxEventsPerDevice) {
        ssize_t n = read(dev.fd, &ev, sizeof(ev));
        if (n == -1) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (errno == ENODEV || errno == EIO) {
                keep = false;
                break;
            }
            std::cerr << "[Input::Read] (" << dev.path << ") read error: " << strerror(errno) << std::endl;
            keep = false;
            break;
        }
        if (n == 0) {
            keep = false;
            break;
        }
        if (n != sizeof(ev)) {
            std::cerr << "[Input::Read] (" << dev.path << ") short read" << std::endl;
            continue;
        }

        processed++;
        if (dev.keyboard_capable && ev.type == EV_KEY && ev.code < KEY_MAX) {
            if (dev.key_shadow.empty()) {
                dev.key_shadow.assign(KEY_MAX, 0);
            }
            uint8_t prev = dev.key_shadow[ev.code];
            uint8_t next = ev.value ? 1 : 0;
            if (prev != next) {
                dev.key_shadow[ev.code] = next;
                if (next) {
                    key_counts[ev.code]++;
                } else if (key_counts[ev.code] > 0) {
                    key_counts[ev.code]--;
                }
                keys[ev.code] = key_counts[ev.code] > 0;
            }
            dev.last_active = std::chrono::steady_clock::now();
            had_activity = true;
        }
        if (dev.mouse_capable && ev.type == EV_REL && ev.code == REL_X) {
            mouse_dx += ev.value;
            dev.last_active = std::chrono::steady_clock::now();
            had_activity = true;
        }
    }

    if (had_activity) {
        last_input_activity = std::chrono::steady_clock::now();
    }

    return keep;
}

void Input::RefreshDevices() {
    EnsureManualDevice(keyboard_override, true, false);
    EnsureManualDevice(mouse_override, false, true);

    bool need_auto = WantsKeyboardAuto() || WantsMouseAuto();
    if (!need_auto) {
        // Prune any previously auto-discovered devices so "manual only" truly pins to overrides
        for (size_t i = 0; i < devices.size();) {
            if (!devices[i].manual) {
                CloseDevice(devices[i]);
                devices.erase(devices.begin() + i);
            } else {
                ++i;
            }
        }
        return;
    }

    auto now = std::chrono::steady_clock::now();
    constexpr auto kScanInterval = std::chrono::milliseconds(500);
    constexpr auto kIdleBeforeScan = std::chrono::milliseconds(40);
    constexpr auto kMaxScanDelay = std::chrono::seconds(5);
    if (last_scan != std::chrono::steady_clock::time_point::min() &&
        now - last_scan < kScanInterval) {
        return;
    }

    bool recently_active = last_input_activity != std::chrono::steady_clock::time_point::min() &&
                           (now - last_input_activity) < kIdleBeforeScan;
    bool force_scan = last_scan == std::chrono::steady_clock::time_point::min() ||
                      (now - last_scan) >= kMaxScanDelay;
    if (recently_active && !force_scan) {
        return;
    }
    last_scan = now;

    DIR* dir = opendir("/dev/input");
    if (!dir) {
        return;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strncmp(entry->d_name, "event", 5) != 0) {
            continue;
        }
        std::string path = std::string("/dev/input/") + entry->d_name;
        DeviceHandle* existing = FindDevice(path);
        if (existing) {
            if (WantsKeyboardAuto() && !existing->keyboard_capable) {
                bool now_keyboard = DeviceSupportsKeyboard(existing->fd);
                if (now_keyboard) {
                    existing->keyboard_capable = true;
                    MarkResyncNeeded();
                }
            }
            if (WantsMouseAuto() && !existing->mouse_capable) {
                existing->mouse_capable = DeviceSupportsMouse(existing->fd);
            }
            continue;
        }

        int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            continue;
        }

        DeviceHandle handle;
        handle.fd = fd;
        handle.path = path;
        handle.manual = false;
        handle.last_active = now;
        if (WantsKeyboardAuto()) {
            handle.keyboard_capable = DeviceSupportsKeyboard(fd);
        }
        if (WantsMouseAuto()) {
            handle.mouse_capable = DeviceSupportsMouse(fd);
        }

        if (!handle.keyboard_capable && !handle.mouse_capable) {
            close(fd);
            continue;
        }

        devices.push_back(std::move(handle));
        DeviceHandle& new_dev = devices.back();
        if (new_dev.keyboard_capable) {
            MarkResyncNeeded();
        }
        if (grab_desired && (new_dev.keyboard_capable || new_dev.mouse_capable)) {
            if (ioctl(new_dev.fd, EVIOCGRAB, 1) == 0) {
                new_dev.grabbed = true;
            } else if (ShouldLogAgain(last_grab_log)) {
                std::cerr << "Failed to grab device " << new_dev.path
                          << ": " << strerror(errno) << std::endl;
            }
        }
    }
    closedir(dir);
}

void Input::EnsureManualDevice(const std::string& path, bool want_keyboard, bool want_mouse) {
    if (path.empty()) {
        return;
    }

    DeviceHandle* existing = FindDevice(path);
    if (existing) {
        existing->manual = true;
        if (want_keyboard && !existing->keyboard_capable) {
            existing->keyboard_capable = true;
            MarkResyncNeeded();
        }
        if (want_mouse) existing->mouse_capable = true;
        return;
    }

    int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        auto& last_log = want_keyboard ? last_keyboard_error : last_mouse_error;
        if (ShouldLogAgain(last_log)) {
            std::cerr << "Failed to open device " << path << ": " << strerror(errno) << std::endl;
        }
        return;
    }

    DeviceHandle handle;
    handle.fd = fd;
    handle.path = path;
    handle.manual = true;
    handle.keyboard_capable = want_keyboard;
    handle.mouse_capable = want_mouse;
    handle.last_active = std::chrono::steady_clock::now();
    devices.push_back(std::move(handle));
    if (want_keyboard) {
        MarkResyncNeeded();
    }
}

Input::DeviceHandle* Input::FindDevice(const std::string& path) {
    for (auto& dev : devices) {
        if (dev.path == path) {
            return &dev;
        }
    }
    return nullptr;
}

void Input::CloseDevice(DeviceHandle& dev) {
    ReleaseDeviceKeys(dev);
    dev.grabbed = false;
    if (dev.fd >= 0) {
        close(dev.fd);
        dev.fd = -1;
    }
}

void Input::ReleaseDeviceKeys(DeviceHandle& dev) {
    if (dev.key_shadow.empty()) {
        return;
    }
    for (size_t code = 0; code < dev.key_shadow.size(); ++code) {
        if (!dev.key_shadow[code]) {
            continue;
        }
        dev.key_shadow[code] = 0;
        if (code < KEY_MAX && key_counts[code] > 0) {
            key_counts[code]--;
            keys[code] = key_counts[code] > 0;
        }
    }
}

bool Input::ShouldLogAgain(std::chrono::steady_clock::time_point& last_log) {
    auto now = std::chrono::steady_clock::now();
    constexpr auto kLogInterval = std::chrono::seconds(2);
    if (last_log == std::chrono::steady_clock::time_point::min() ||
        now - last_log >= kLogInterval) {
        last_log = now;
        return true;
    }
    return false;
}

bool Input::WantsKeyboardAuto() const {
    return keyboard_override.empty();
}

bool Input::WantsMouseAuto() const {
    return mouse_override.empty();
}

// --- Place these at the end of the file ---

bool Input::CheckToggle() {
    bool ctrl = keys[KEY_LEFTCTRL] || keys[KEY_RIGHTCTRL];
    bool m = keys[KEY_M];
    bool both = ctrl && m;
    bool toggled = both && !prev_toggle;
    prev_toggle = both;
    return toggled;
}

void Input::WaitForAllKeysReleased(int timeout_ms) {
    auto clamp_timeout = std::max(timeout_ms, 0);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(clamp_timeout);

    auto any_pressed = [&]() {
        for (int code = 0; code < KEY_MAX; ++code) {
            if (keys[code]) {
                return true;
            }
        }
        return false;
    };

    while (any_pressed()) {
        if (clamp_timeout > 0 && std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        WaitForEvents(5);
        int ignore_dx = 0;
        Read(ignore_dx);
    }
}

bool Input::Grab(bool enable) {
    grab_desired = enable;
    if (enable) {
        RefreshDevices();
    }
    int grab = enable ? 1 : 0;
    int changed = 0;
    bool had_error = false;
    for (auto& dev : devices) {
        if (dev.fd < 0) {
            continue;
        }
        if (!dev.keyboard_capable && !dev.mouse_capable) {
            continue;
        }
        if (enable && dev.grabbed) {
            continue;
        }
        if (!enable && !dev.grabbed) {
            continue;
        }
        if (ioctl(dev.fd, EVIOCGRAB, grab) < 0) {
            if (enable) {
                std::cerr << "Failed to grab device " << dev.path << ": " << strerror(errno) << std::endl;
                had_error = true;
            } else if (errno != EINVAL && errno != ENODEV) {
                std::cerr << "Failed to release device " << dev.path << ": " << strerror(errno) << std::endl;
            }
            if (!enable) {
                dev.grabbed = false;
            }
        } else if (enable) {
            dev.grabbed = true;
            changed++;
        } else {
            dev.grabbed = false;
            changed++;
        }
    }

    if (changed > 0 && ShouldLogAgain(last_grab_log)) {
        std::cout << (enable ? "Grabbed " : "Released ")
                  << changed << " device" << (changed == 1 ? "" : "s")
                  << std::endl;
    }

    if (!enable) {
        return true;
    }

    if (had_error) {
        Grab(false);
        return false;
    }

    if (!AllRequiredGrabbed()) {
        if (ShouldLogAgain(last_grab_log)) {
            std::cerr << "Unable to grab required keyboard/mouse devices" << std::endl;
        }
        Grab(false);
        return false;
    }

    return true;
}

void Input::ResyncKeyStates() {
    if (!resync_pending) {
        return;
    }

    memset(keys, 0, sizeof(keys));
    memset(key_counts, 0, sizeof(key_counts));

    for (auto& dev : devices) {
        if (dev.fd < 0 || !dev.keyboard_capable) {
            if (!dev.key_shadow.empty()) {
                std::fill(dev.key_shadow.begin(), dev.key_shadow.end(), 0);
            }
            continue;
        }

        if (dev.key_shadow.empty()) {
            dev.key_shadow.assign(KEY_MAX, 0);
        } else {
            std::fill(dev.key_shadow.begin(), dev.key_shadow.end(), 0);
        }

        unsigned long key_bits[NBITS(KEY_MAX)] = {0};
        if (ioctl(dev.fd, EVIOCGKEY(sizeof(key_bits)), key_bits) < 0) {
            continue;
        }

        for (int code = 0; code < KEY_MAX; ++code) {
            if (test_bit(code, key_bits)) {
                dev.key_shadow[code] = 1;
                key_counts[code]++;
            }
        }
    }

    for (int code = 0; code < KEY_MAX; ++code) {
        keys[code] = key_counts[code] > 0;
    }

    prev_toggle = false;
    resync_pending = false;
}

void Input::MarkResyncNeeded() {
    resync_pending = true;
}

bool Input::IsKeyPressed(int keycode) const {
    if (keycode >= 0 && keycode < KEY_MAX) {
        return keys[keycode];
    }
    return false;
}

bool Input::HasGrabbedKeyboard() const {
    for (const auto& dev : devices) {
        if (dev.keyboard_capable && dev.grabbed) {
            return true;
        }
    }
    return false;
}

bool Input::HasGrabbedMouse() const {
    for (const auto& dev : devices) {
        if (dev.mouse_capable && dev.grabbed) {
            return true;
        }
    }
    return false;
}

bool Input::AllRequiredGrabbed() const {
    bool need_keyboard = NeedsKeyboard();
    bool need_mouse = NeedsMouse();
    bool keyboard_ok = !need_keyboard || HasGrabbedKeyboard();
    bool mouse_ok = !need_mouse || HasGrabbedMouse();
    return keyboard_ok && mouse_ok;
}

bool Input::NeedsKeyboard() const {
    return true;
}

bool Input::NeedsMouse() const {
    return true;
}
