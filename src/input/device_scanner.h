
#ifndef DEVICE_SCANNER_H
#define DEVICE_SCANNER_H

#include <linux/input.h>
#include <string>
#include <vector>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "device_enumerator.h"

class DeviceScanner {
    // Event-driven additions
public:
    std::condition_variable input_cv;
    std::mutex input_mutex;
    void NotifyInputChanged();
    void Read();
    bool WaitForEvents(int timeout_ms);
public:
    DeviceScanner();
    ~DeviceScanner();
    
    // Discover and open input devices
    // If device_path is provided, use it directly; otherwise auto-detect
    bool DiscoverKeyboard(const std::string& device_path = "");
    bool DiscoverMouse(const std::string& device_path = "");
    
    // Read events from keyboard and mouse
    void Read(int& mouse_dx);
    
    // Check for Ctrl+M toggle (edge detection)
    bool CheckToggle();
    
    // Grab/ungrab devices for exclusive access. Returns true if all required devices are grabbed.
    bool Grab(bool enable);

    // Rebuild aggregated key state by querying each keyboard device directly
    void ResyncKeyStates();

    // Check if a key is currently pressed
    bool IsKeyPressed(int keycode) const;
    bool HasGrabbedKeyboard() const;
    bool HasGrabbedMouse() const;
    bool AllRequiredGrabbed() const;
    bool HasRequiredDevices() const;

private:
    struct DeviceHandle {
        int fd = -1;
        std::string path;
        bool keyboard_capable = false;
        bool mouse_capable = false;
        bool manual = false;
        bool grabbed = false;
        std::chrono::steady_clock::time_point last_active;
        std::vector<uint8_t> key_shadow;
    };

    std::vector<DeviceHandle> devices;
    mutable std::mutex devices_mutex;
    DeviceEnumerator enumerator_;
    std::string keyboard_override;
    std::string mouse_override;
    std::chrono::steady_clock::time_point last_keyboard_error;
    std::chrono::steady_clock::time_point last_mouse_error;
    std::chrono::steady_clock::time_point last_grab_log;
    bool resync_pending;
    bool grab_desired;
    bool keys[KEY_MAX];
    int key_counts[KEY_MAX];
    bool prev_toggle;
    int wake_event_fd_;
    
    void RequestScan(bool force);
    void HandleEnumeration(std::vector<std::string>&& nodes, bool force);
    void RefreshDevices(bool force, std::vector<std::string>&& nodes);
    void EnsureManualDevice(const std::string& path, bool want_keyboard, bool want_mouse);
    void CloseDevice(DeviceHandle& dev);
    DeviceHandle* FindDeviceLocked(const std::string& path);
    bool DrainDevice(DeviceHandle& dev, int& mouse_dx);
    void ReleaseDeviceKeys(DeviceHandle& dev);
    bool ShouldLogAgain(std::chrono::steady_clock::time_point& last_log);
    bool WantsKeyboardAuto() const;
    bool WantsMouseAuto() const;
    bool NeedsKeyboard() const;
    bool NeedsMouse() const;
    bool HasGrabbedKeyboardLocked() const;
    bool HasGrabbedMouseLocked() const;
    bool AllRequiredGrabbedLocked() const;
    bool HasOpenDevicesLocked() const;
    bool HasRequiredDevicesLocked() const;
    bool BuildAutoDeviceHandle(const std::string& path,
                               bool want_keyboard,
                               bool want_mouse,
                               DeviceHandle& out_handle);
    void RemoveAutoDevicesLocked();
    void SignalWakeFd() const;
    void DrainWakeFd() const;
};

#endif // DEVICE_SCANNER_H
