// Improved toggle: allow either Ctrl key, and tolerate quick presses
#include "device_scanner.h"
#include <iostream>
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <cstring>
#include <dirent.h>
#include <vector>
#include <unordered_set>
#include <linux/input-event-codes.h>
#include <atomic>
#include <poll.h>
#include <thread>
#include "../logging/logger.h"
extern std::atomic<bool> running;

namespace {
constexpr const char* kTag = "device_scanner";
}

void DeviceScanner::Read() {
    int dummy = 0;
    Read(dummy);
}

void DeviceScanner::NotifyInputChanged() {
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

DeviceScanner::DeviceScanner() : resync_pending(true), grab_desired(false), prev_toggle(false) {
    memset(keys, 0, sizeof(keys));
    memset(key_counts, 0, sizeof(key_counts));
    last_scan = std::chrono::steady_clock::time_point::min();
    last_input_activity = std::chrono::steady_clock::time_point::min();
    last_keyboard_error = std::chrono::steady_clock::time_point::min();
    last_mouse_error = std::chrono::steady_clock::time_point::min();
    last_grab_log = std::chrono::steady_clock::time_point::min();
    StartScannerThread();
}

DeviceScanner::~DeviceScanner() {
    StopScannerThread();
    std::lock_guard<std::mutex> lock(devices_mutex);
    for (auto& dev : devices) {
        CloseDevice(dev);
    }
}

void DeviceScanner::StartScannerThread() {
    scanner_stop = false;
    scan_requested = true;
    force_scan_requested = true;
    scanner_thread = std::thread(&DeviceScanner::ScannerThreadMain, this);
}

void DeviceScanner::StopScannerThread() {
    {
        std::lock_guard<std::mutex> lock(scanner_mutex);
        scanner_stop = true;
        scan_requested = true;
        force_scan_requested = true;
    }
    scan_cv.notify_all();
    if (scanner_thread.joinable()) {
        scanner_thread.join();
    }
}

void DeviceScanner::ScannerThreadMain() {
    auto interval = std::chrono::milliseconds(120);
    std::unique_lock<std::mutex> lock(scanner_mutex);
    while (!scanner_stop) {
        scan_cv.wait_for(lock, interval, [this]() {
            return scanner_stop || scan_requested;
        });
        if (scanner_stop) {
            break;
        }
        bool force = force_scan_requested;
        bool requested = scan_requested;
        scan_requested = false;
        force_scan_requested = false;
        lock.unlock();
        RunSynchronousScan(force || requested);
        lock.lock();
    }
}

void DeviceScanner::RequestScan(bool force) {
    {
        std::lock_guard<std::mutex> lock(scanner_mutex);
        scan_requested = true;
        if (force) {
            force_scan_requested = true;
        }
    }
    scan_cv.notify_all();
}

void DeviceScanner::RunSynchronousScan(bool force) {
    RefreshDevices(force);
}

bool DeviceScanner::DiscoverKeyboard(const std::string& device_path) {
    {
        std::lock_guard<std::mutex> lock(devices_mutex);
        keyboard_override = device_path;
        last_keyboard_error = std::chrono::steady_clock::time_point::min();
    }
    RunSynchronousScan(true);

    if (!device_path.empty()) {
        std::lock_guard<std::mutex> lock(devices_mutex);
        if (!FindDeviceLocked(device_path)) {
            std::cerr << "Failed to open keyboard device: " << device_path << std::endl;
            return false;
        }
        resync_pending = true;
    }
    return true;
}

bool DeviceScanner::DiscoverMouse(const std::string& device_path) {
    {
        std::lock_guard<std::mutex> lock(devices_mutex);
        mouse_override = device_path;
        last_mouse_error = std::chrono::steady_clock::time_point::min();
    }
    RunSynchronousScan(true);

    if (!device_path.empty()) {
        std::lock_guard<std::mutex> lock(devices_mutex);
        if (!FindDeviceLocked(device_path)) {
            std::cerr << "Failed to open mouse device: " << device_path << std::endl;
            return false;
        }
    }
    return true;
}

bool DeviceScanner::WaitForEvents(int timeout_ms) {
    std::vector<pollfd> pfds;
    {
        std::lock_guard<std::mutex> lock(devices_mutex);
        pfds.reserve(devices.size());
        for (auto& dev : devices) {
            if (dev.fd >= 0) {
                pollfd p{};
                p.fd = dev.fd;
                p.events = POLLIN;
                pfds.push_back(p);
            }
        }
    }

    if (pfds.empty()) {
        RequestScan(true);
            auto wait_pred = [this]() {
                if (!running.load(std::memory_order_relaxed)) {
                    return true;
                }
                std::lock_guard<std::mutex> guard(devices_mutex);
                return HasOpenDevicesLocked();
            };
            if (timeout_ms < 0) {
                std::unique_lock<std::mutex> lock(input_mutex);
                input_cv.wait(lock, wait_pred);
            } else if (timeout_ms == 0) {
                return false;
            } else {
                std::unique_lock<std::mutex> lock(input_mutex);
                input_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), wait_pred);
            }
        return false;
    }

    int ret = poll(pfds.data(), pfds.size(), timeout_ms);
    if (ret < 0) {
        if (errno == EINTR) {
            return false;
        }
        std::cerr << "[DeviceScanner::WaitForEvents] poll() error: " << strerror(errno) << std::endl;
        return false;
    }
    return ret > 0;
}

void DeviceScanner::Read(int& mouse_dx) {
    if (!running) {
        return;
    }
    bool lost_device = false;
    mouse_dx = 0;

    {
        std::lock_guard<std::mutex> lock(devices_mutex);
        for (size_t i = 0; i < devices.size();) {
            if (!DrainDevice(devices[i], mouse_dx)) {
                CloseDevice(devices[i]);
                devices.erase(devices.begin() + i);
                lost_device = true;
            } else {
                ++i;
            }
        }
    }

    if (lost_device) {
        // Run another scan soon so replacements are discovered quickly.
        RequestScan(false);
    }
}

bool DeviceScanner::DrainDevice(DeviceHandle& dev, int& mouse_dx) {
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
            std::cerr << "[DeviceScanner::Read] (" << dev.path << ") read error: " << strerror(errno) << std::endl;
            keep = false;
            break;
        }
        if (n == 0) {
            keep = false;
            break;
        }
        if (n != sizeof(ev)) {
            std::cerr << "[DeviceScanner::Read] (" << dev.path << ") short read" << std::endl;
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

void DeviceScanner::RefreshDevices(bool force) {
    EnsureManualDevice(keyboard_override, true, false);
    EnsureManualDevice(mouse_override, false, true);

    struct ScanPlan {
        bool need_auto = false;
        bool want_keyboard = false;
        bool want_mouse = false;
        std::chrono::steady_clock::time_point scan_time{};
        std::unordered_set<std::string> known_paths;
    } plan;

    auto now = std::chrono::steady_clock::now();
    constexpr auto kScanInterval = std::chrono::milliseconds(500);
    constexpr auto kIdleBeforeScan = std::chrono::milliseconds(40);
    constexpr auto kMaxScanDelay = std::chrono::seconds(5);

    {
        std::lock_guard<std::mutex> lock(devices_mutex);
        plan.want_keyboard = WantsKeyboardAuto();
        plan.want_mouse = WantsMouseAuto();
        plan.need_auto = plan.want_keyboard || plan.want_mouse;

        if (!plan.need_auto) {
            RemoveAutoDevicesLocked();
            return;
        }

        bool recently_active = last_input_activity != std::chrono::steady_clock::time_point::min() &&
                               (now - last_input_activity) < kIdleBeforeScan;
        bool overdue = last_scan == std::chrono::steady_clock::time_point::min() ||
                       (now - last_scan) >= kMaxScanDelay;
        bool throttled = last_scan != std::chrono::steady_clock::time_point::min() &&
                         (now - last_scan) < kScanInterval;

        if (!force) {
            if (throttled) {
                return;
            }
            if (recently_active && !overdue) {
                return;
            }
        }

        plan.scan_time = now;
        last_scan = now;
        plan.known_paths.reserve(devices.size());
        for (const auto& dev : devices) {
            if (!dev.path.empty()) {
                plan.known_paths.insert(dev.path);
            }
        }
    }

    auto event_nodes = EnumerateEventNodes();
    std::vector<DeviceHandle> additions;
    additions.reserve(event_nodes.size());

    for (const auto& path : event_nodes) {
        if (plan.known_paths.find(path) != plan.known_paths.end()) {
            continue;
        }
        DeviceHandle handle;
        if (!BuildAutoDeviceHandle(path, plan.want_keyboard, plan.want_mouse, handle)) {
            continue;
        }
        additions.push_back(std::move(handle));
    }

    if (additions.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(devices_mutex);
    for (auto& handle : additions) {
        if (FindDeviceLocked(handle.path)) {
            CloseDevice(handle);
            continue;
        }
        devices.push_back(std::move(handle));
        NotifyInputChanged();
        DeviceHandle& new_dev = devices.back();
        if (new_dev.keyboard_capable) {
            resync_pending = true;
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
    if (!additions.empty()) {
        LOG_DEBUG(kTag, "scan added " << additions.size() << " device(s)");
    }
}

void DeviceScanner::EnsureManualDevice(const std::string& path, bool want_keyboard, bool want_mouse) {
    if (path.empty()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(devices_mutex);
        DeviceHandle* existing = FindDeviceLocked(path);
        if (existing) {
            existing->manual = true;
            if (want_keyboard && !existing->keyboard_capable) {
                existing->keyboard_capable = true;
                resync_pending = true;
            }
            if (want_mouse) {
                existing->mouse_capable = true;
            }
            return;
        }
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

    std::lock_guard<std::mutex> lock(devices_mutex);
    if (FindDeviceLocked(path)) {
        close(handle.fd);
        return;
    }
    devices.push_back(std::move(handle));
    NotifyInputChanged();
    if (want_keyboard) {
        resync_pending = true;
    }
}

DeviceScanner::DeviceHandle* DeviceScanner::FindDeviceLocked(const std::string& path) {
    for (auto& dev : devices) {
        if (dev.path == path) {
            return &dev;
        }
    }
    return nullptr;
}

void DeviceScanner::CloseDevice(DeviceHandle& dev) {
    ReleaseDeviceKeys(dev);
    dev.grabbed = false;
    if (dev.fd >= 0) {
        close(dev.fd);
        dev.fd = -1;
        NotifyInputChanged();
    }
}

void DeviceScanner::ReleaseDeviceKeys(DeviceHandle& dev) {
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

bool DeviceScanner::ShouldLogAgain(std::chrono::steady_clock::time_point& last_log) {
    auto now = std::chrono::steady_clock::now();
    constexpr auto kLogInterval = std::chrono::seconds(2);
    if (last_log == std::chrono::steady_clock::time_point::min() ||
        now - last_log >= kLogInterval) {
        last_log = now;
        return true;
    }
    return false;
}

std::vector<std::string> DeviceScanner::EnumerateEventNodes() const {
    std::vector<std::string> nodes;
    DIR* dir = opendir("/dev/input");
    if (!dir) {
        return nodes;
    }
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strncmp(entry->d_name, "event", 5) != 0) {
            continue;
        }
        nodes.emplace_back(std::string("/dev/input/") + entry->d_name);
    }
    closedir(dir);
    return nodes;
}

bool DeviceScanner::BuildAutoDeviceHandle(const std::string& path,
                                          bool want_keyboard,
                                          bool want_mouse,
                                          DeviceHandle& out_handle) {
    int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        return false;
    }

    DeviceHandle candidate;
    candidate.fd = fd;
    candidate.path = path;
    candidate.manual = false;
    candidate.last_active = std::chrono::steady_clock::now();

    if (want_keyboard) {
        candidate.keyboard_capable = DeviceSupportsKeyboard(fd);
    }
    if (want_mouse) {
        candidate.mouse_capable = DeviceSupportsMouse(fd);
    }

    if (!candidate.keyboard_capable && !candidate.mouse_capable) {
        close(fd);
        return false;
    }

    out_handle = std::move(candidate);
    return true;
}

void DeviceScanner::RemoveAutoDevicesLocked() {
    for (size_t i = 0; i < devices.size();) {
        if (!devices[i].manual) {
            CloseDevice(devices[i]);
            devices.erase(devices.begin() + i);
        } else {
            ++i;
        }
    }
}

bool DeviceScanner::WantsKeyboardAuto() const {
    return keyboard_override.empty();
}

bool DeviceScanner::WantsMouseAuto() const {
    return mouse_override.empty();
}

// --- Place these at the end of the file ---

bool DeviceScanner::CheckToggle() {
    std::lock_guard<std::mutex> lock(devices_mutex);
    bool ctrl = keys[KEY_LEFTCTRL] || keys[KEY_RIGHTCTRL];
    bool m = keys[KEY_M];
    bool both = ctrl && m;
    bool toggled = both && !prev_toggle;
    prev_toggle = both;
    return toggled;
}

bool DeviceScanner::Grab(bool enable) {
    {
        std::lock_guard<std::mutex> lock(devices_mutex);
        grab_desired = enable;
    }

    if (enable) {
        RunSynchronousScan(true);
    }

    std::unique_lock<std::mutex> lock(devices_mutex);
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
        lock.unlock();
        Grab(false);
        return false;
    }

    if (!AllRequiredGrabbedLocked()) {
        if (ShouldLogAgain(last_grab_log)) {
            std::cerr << "Unable to grab required keyboard/mouse devices" << std::endl;
        }
        lock.unlock();
        Grab(false);
        return false;
    }

    return true;
}

void DeviceScanner::ResyncKeyStates() {
    std::lock_guard<std::mutex> lock(devices_mutex);
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

bool DeviceScanner::IsKeyPressed(int keycode) const {
    std::lock_guard<std::mutex> lock(devices_mutex);
    if (keycode >= 0 && keycode < KEY_MAX) {
        return keys[keycode];
    }
    return false;
}

bool DeviceScanner::HasGrabbedKeyboard() const {
    std::lock_guard<std::mutex> lock(devices_mutex);
    return HasGrabbedKeyboardLocked();
}

bool DeviceScanner::HasGrabbedMouse() const {
    std::lock_guard<std::mutex> lock(devices_mutex);
    return HasGrabbedMouseLocked();
}

bool DeviceScanner::AllRequiredGrabbed() const {
    std::lock_guard<std::mutex> lock(devices_mutex);
    return AllRequiredGrabbedLocked();
}

bool DeviceScanner::NeedsKeyboard() const {
    return true;
}

bool DeviceScanner::NeedsMouse() const {
    return true;
}

bool DeviceScanner::HasOpenDevicesLocked() const {
    for (const auto& dev : devices) {
        if (dev.fd >= 0) {
            return true;
        }
    }
    return false;
}

bool DeviceScanner::HasGrabbedKeyboardLocked() const {
    for (const auto& dev : devices) {
        if (dev.keyboard_capable && dev.grabbed) {
            return true;
        }
    }
    return false;
}

bool DeviceScanner::HasGrabbedMouseLocked() const {
    for (const auto& dev : devices) {
        if (dev.mouse_capable && dev.grabbed) {
            return true;
        }
    }
    return false;
}

bool DeviceScanner::AllRequiredGrabbedLocked() const {
    bool need_keyboard = NeedsKeyboard();
    bool need_mouse = NeedsMouse();
    bool keyboard_ok = !need_keyboard || HasGrabbedKeyboardLocked();
    bool mouse_ok = !need_mouse || HasGrabbedMouseLocked();
    return keyboard_ok && mouse_ok;
}
