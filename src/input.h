
#ifndef INPUT_H
#define INPUT_H

#include <linux/input.h>
#include <string>
#include <vector>
#include <chrono>
#include <condition_variable>
#include <mutex>

class Input {
    // Event-driven additions
public:
    std::condition_variable input_cv;
    std::mutex input_mutex;
    void NotifyInputChanged();
    void Read();
    bool WaitForEvents(int timeout_ms);
public:
    Input();
    ~Input();
    
    // Discover and open input devices
    // If device_path is provided, use it directly; otherwise auto-detect
    bool DiscoverKeyboard(const std::string& device_path = "");
    bool DiscoverMouse(const std::string& device_path = "");
    
    // Read events from keyboard and mouse
    void Read(int& mouse_dx);
    
    // Check for Ctrl+M toggle (edge detection)
    bool CheckToggle();
    
    // Grab/ungrab devices for exclusive access
    void Grab(bool enable);

    // Rebuild aggregated key state by querying each keyboard device directly
    void ResyncKeyStates();

    // Mark that a resync is needed (e.g., new keyboard device discovered)
    void MarkResyncNeeded();

    // Check if a key is currently pressed
    bool IsKeyPressed(int keycode) const;

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
    std::string keyboard_override;
    std::string mouse_override;
    std::chrono::steady_clock::time_point last_scan;
    std::chrono::steady_clock::time_point last_input_activity;
    std::chrono::steady_clock::time_point last_keyboard_error;
    std::chrono::steady_clock::time_point last_mouse_error;
    bool resync_pending;
    bool keys[KEY_MAX];
    int key_counts[KEY_MAX];
    bool prev_toggle;
    
    void RefreshDevices();
    void EnsureManualDevice(const std::string& path, bool want_keyboard, bool want_mouse);
    void CloseDevice(DeviceHandle& dev);
    DeviceHandle* FindDevice(const std::string& path);
    bool DrainDevice(DeviceHandle& dev, int& mouse_dx);
    void ReleaseDeviceKeys(DeviceHandle& dev);
    bool ShouldLogAgain(std::chrono::steady_clock::time_point& last_log);
    bool WantsKeyboardAuto() const;
    bool WantsMouseAuto() const;
};

#endif // INPUT_H
