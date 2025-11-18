#ifndef INPUT_H
#define INPUT_H

#include <linux/input.h>
#include <string>

class Input {
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
    
    // Check if a key is currently pressed
    bool IsKeyPressed(int keycode) const;

private:
    int kbd_fd;
    int mouse_fd;
    bool keys[KEY_MAX];
    bool prev_toggle;
    
    bool OpenDevice(const char* path, int& fd);
    void CloseDevice(int& fd);
};

#endif // INPUT_H
