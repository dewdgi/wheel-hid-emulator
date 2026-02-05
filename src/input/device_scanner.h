
#ifndef DEVICE_SCANNER_H
#define DEVICE_SCANNER_H

#include <windows.h>
#include "../input_defs.h"

#include <string>
#include <mutex>
#include <unordered_set>

class DeviceScanner {
public:
    DeviceScanner();
    ~DeviceScanner();

    // Discover devices (no-op on Windows — Raw Input handles everything)
    bool DiscoverKeyboard(const std::string& device_path = "");
    bool DiscoverMouse(const std::string& device_path = "");
    
    // Read events from keyboard and mouse
    void Read(int& mouse_dx);
    void Read();
    
    // Check for Ctrl+M toggle (edge detection)
    bool CheckToggle();
    
    // Grab/ungrab devices (locks/unlocks cursor on Windows)
    bool Grab(bool enable);

    // Rebuild aggregated key state (no-op on Windows)
    void ResyncKeyStates();

    // Check if a key is currently pressed
    bool IsKeyPressed(int keycode) const;
    bool HasGrabbedKeyboard() const;
    bool HasGrabbedMouse() const;
    bool AllRequiredGrabbed() const;
    bool HasRequiredDevices() const;

    // Event notification
    std::mutex input_mutex;
    void NotifyInputChanged();
    bool WaitForEvents(int timeout_ms);

    // Raw Input state updates (called from window proc)
    void UpdateKeyState(int linux_code, bool pressed);
    void UpdateMouseState(int dx);

private:
    std::unordered_set<int> active_keys;
    int accumulated_mouse_dx = 0;
    bool toggle_latch_ = false;
    bool cursor_locked_ = false;
    POINT saved_cursor_pos_ = {0, 0};
    void LockCursor();
    void UnlockCursor();
};

#endif // DEVICE_SCANNER_H
