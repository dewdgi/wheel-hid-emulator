#include "input.h"
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <vector>
#include <algorithm>
#include <dirent.h>
#include <sys/ioctl.h>
#include <linux/input-event-codes.h>

// Bit manipulation macros for input device capabilities
#define BITS_PER_LONG (sizeof(long) * 8)
#define NBITS(x) ((((x)-1)/BITS_PER_LONG)+1)
#define OFF(x)  ((x)%BITS_PER_LONG)
#define BIT(x)  (1UL<<OFF(x))
#define LONG(x) ((x)/BITS_PER_LONG)
#define test_bit(bit, array)    ((array[LONG(bit)] >> OFF(bit)) & 1)

Input::Input() : kbd_fd(-1), mouse_fd(-1), prev_toggle(false) {
    memset(keys, 0, sizeof(keys));
}

Input::~Input() {
    CloseDevice(kbd_fd);
    CloseDevice(mouse_fd);
}

bool Input::DiscoverKeyboard() {
    DIR* dir = opendir("/dev/input");
    if (!dir) {
        std::cerr << "Failed to open /dev/input" << std::endl;
        return false;
    }
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strncmp(entry->d_name, "event", 5) != 0) {
            continue;
        }
        
        std::string path = std::string("/dev/input/") + entry->d_name;
        int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            continue;
        }
        
        char name[256] = "Unknown";
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        
        // Convert to lowercase for case-insensitive comparison
        std::string name_lower = name;
        for (char& c : name_lower) {
            c = tolower(c);
        }
        
        if (name_lower.find("keyboard") != std::string::npos) {
            kbd_fd = fd;
            std::cout << "Found keyboard: " << name << " at " << path << std::endl;
            closedir(dir);
            return true;
        }
        
        close(fd);
    }
    
    closedir(dir);
    std::cerr << "No keyboard found" << std::endl;
    return false;
}

bool Input::DiscoverMouse() {
    DIR* dir = opendir("/dev/input");
    if (!dir) {
        std::cerr << "Failed to open /dev/input" << std::endl;
        return false;
    }
    
    struct MouseCandidate {
        std::string path;
        std::string name;
        int priority;
    };
    std::vector<MouseCandidate> candidates;
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strncmp(entry->d_name, "event", 5) != 0) {
            continue;
        }
        
        std::string path = std::string("/dev/input/") + entry->d_name;
        int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            continue;
        }
        
        // Check for REL_X capability
        unsigned long rel_bitmask[NBITS(REL_MAX)] = {0};
        ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel_bitmask)), rel_bitmask);
        
        if (test_bit(REL_X, rel_bitmask)) {
            char name[256] = "Unknown";
            ioctl(fd, EVIOCGNAME(sizeof(name)), name);
            
            std::string name_lower = name;
            for (char& c : name_lower) {
                c = tolower(c);
            }
            
            // Prioritize: avoid touchpads and virtual devices
            int priority = 50; // default
            
            // Deprioritize touchpads
            if (name_lower.find("touchpad") != std::string::npos) {
                priority = 10;
            }
            // Deprioritize virtual/internal mouse devices from touchpads
            else if (name_lower.find("uniw") != std::string::npos || 
                     name_lower.find("elan") != std::string::npos ||
                     name_lower.find("synaptics") != std::string::npos) {
                priority = 20;
            }
            // Prioritize USB/wireless mice
            else if (name_lower.find("wireless") != std::string::npos ||
                     name_lower.find("usb") != std::string::npos ||
                     name_lower.find("beken") != std::string::npos ||
                     name_lower.find("logitech") != std::string::npos ||
                     name_lower.find("razer") != std::string::npos) {
                priority = 100;
            }
            
            candidates.push_back({path, name, priority});
        }
        
        close(fd);
    }
    
    closedir(dir);
    
    if (candidates.empty()) {
        std::cerr << "No mouse found" << std::endl;
        return false;
    }
    
    // Sort by priority (highest first)
    std::sort(candidates.begin(), candidates.end(), 
              [](const MouseCandidate& a, const MouseCandidate& b) {
                  return a.priority > b.priority;
              });
    
    // Use the highest priority device
    auto& best = candidates[0];
    mouse_fd = open(best.path.c_str(), O_RDONLY | O_NONBLOCK);
    if (mouse_fd < 0) {
        std::cerr << "Failed to open mouse: " << best.path << std::endl;
        return false;
    }
    
    std::cout << "Found mouse: " << best.name << " at " << best.path << std::endl;
    return true;
}

void Input::Read(int& mouse_dx) {
    mouse_dx = 0;
    struct input_event ev;
    
    // Read keyboard events
    if (kbd_fd >= 0) {
        while (read(kbd_fd, &ev, sizeof(ev)) > 0) {
            if (ev.type == EV_KEY && ev.code < KEY_MAX) {
                keys[ev.code] = (ev.value != 0);
            }
        }
    }
    
    // Read mouse events
    if (mouse_fd >= 0) {
        while (read(mouse_fd, &ev, sizeof(ev)) > 0) {
            if (ev.type == EV_REL && ev.code == REL_X) {
                mouse_dx += ev.value;
            }
        }
    }
}

bool Input::CheckToggle() {
    bool both = keys[KEY_LEFTCTRL] && keys[KEY_M];
    bool toggled = false;
    
    if (both && !prev_toggle) {
        toggled = true;
    }
    
    prev_toggle = both;
    return toggled;
}

void Input::Grab(bool enable) {
    int grab = enable ? 1 : 0;
    
    if (kbd_fd >= 0) {
        if (ioctl(kbd_fd, EVIOCGRAB, grab) < 0) {
            std::cerr << "Failed to " << (enable ? "grab" : "ungrab") << " keyboard" << std::endl;
        } else {
            std::cout << (enable ? "Grabbed" : "Ungrabbed") << " keyboard" << std::endl;
        }
    }
    
    if (mouse_fd >= 0) {
        if (ioctl(mouse_fd, EVIOCGRAB, grab) < 0) {
            std::cerr << "Failed to " << (enable ? "grab" : "ungrab") << " mouse" << std::endl;
        } else {
            std::cout << (enable ? "Grabbed" : "Ungrabbed") << " mouse" << std::endl;
        }
    }
}

bool Input::IsKeyPressed(int keycode) const {
    if (keycode >= 0 && keycode < KEY_MAX) {
        return keys[keycode];
    }
    return false;
}

bool Input::OpenDevice(const char* path, int& fd) {
    fd = open(path, O_RDONLY | O_NONBLOCK);
    return fd >= 0;
}

void Input::CloseDevice(int& fd) {
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}
