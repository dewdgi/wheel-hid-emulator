// Improved toggle: allow either Ctrl key, and tolerate quick presses
#include "input.h"
#include <iostream>
#include <cerrno>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <vector>
#include <algorithm>
#include <dirent.h>
#include <sys/ioctl.h>
#include <linux/input-event-codes.h>
#include <atomic>
extern std::atomic<bool> running;
#include <poll.h>

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

bool Input::DiscoverKeyboard(const std::string& device_path) {
    // If explicit device path provided, use it
    std::cout << "[DEBUG][DiscoverKeyboard] called with device_path='" << device_path << "'" << std::endl;
    if (!device_path.empty()) {
        kbd_fd = open(device_path.c_str(), O_RDONLY | O_NONBLOCK);
        std::cout << "[DEBUG][DiscoverKeyboard] open(" << device_path << ") returned fd=" << kbd_fd << std::endl;
        if (kbd_fd >= 0) {
            int flags = fcntl(kbd_fd, F_GETFL);
            std::cout << "[DEBUG][DiscoverKeyboard] kbd_fd F_GETFL flags=" << flags << std::endl;
            char name[256] = {0};
            if (ioctl(kbd_fd, EVIOCGNAME(sizeof(name)), name) >= 0) {
                std::cout << "[DEBUG][DiscoverKeyboard] kbd_fd EVIOCGNAME: '" << name << "'" << std::endl;
            }
            int grab_result = ioctl(kbd_fd, EVIOCGRAB, 0);
            std::cout << "[DEBUG][DiscoverKeyboard] kbd_fd EVIOCGRAB(0) result=" << grab_result << ", errno=" << errno << std::endl;

            // TEMP: Try a blocking read loop for diagnostics
            std::cout << "[DEBUG][DiscoverKeyboard] Starting direct blocking read test (press keys)..." << std::endl;
            for (int i = 0; i < 10; ++i) {
                struct input_event ev;
                ssize_t n = read(kbd_fd, &ev, sizeof(ev));
                std::cout << "[DEBUG][DiscoverKeyboard] direct read n=" << n << ", errno=" << errno;
                if (n == sizeof(ev)) {
                    std::cout << ", type=" << ev.type << ", code=" << ev.code << ", value=" << ev.value;
                }
                std::cout << std::endl;
            }
            unsigned long evbit[8] = {0};
            if (ioctl(kbd_fd, EVIOCGBIT(0, sizeof(evbit)), evbit) >= 0) {
                std::cout << "[DEBUG][DiscoverKeyboard] Supported event types:";
                for (int i = 0; i < 64; ++i) {
                    if (evbit[0] & (1UL << i)) std::cout << " " << i;
                }
                std::cout << std::endl;
            }
            unsigned long keybit[8] = {0};
            if (ioctl(kbd_fd, EVIOCGBIT(EV_KEY, sizeof(keybit)), keybit) >= 0) {
                std::cout << "[DEBUG][DiscoverKeyboard] Supported keys:";
                for (int i = 0; i < 64; ++i) {
                    if (keybit[0] & (1UL << i)) std::cout << " " << i;
                }
                std::cout << std::endl;
            }
        }
        if (kbd_fd < 0) {
            std::cerr << "Failed to open keyboard device: " << device_path << ", errno=" << errno << std::endl;
            return false;
        }
        char name[256] = "Unknown";
        ioctl(kbd_fd, EVIOCGNAME(sizeof(name)), name);
        std::cout << "Using configured keyboard: " << name << " at " << device_path << std::endl;
        return true;
    }
    
    // Otherwise, auto-detect
    std::cout << "[DEBUG][DiscoverKeyboard] auto-detecting in /dev/input" << std::endl;
    DIR* dir = opendir("/dev/input");
    if (!dir) {
        std::cerr << "Failed to open /dev/input" << std::endl;
        return false;
    }
    
    struct KeyboardCandidate {
        std::string path;
        std::string name;
        int priority;
        int fd;
    };
    std::vector<KeyboardCandidate> candidates;
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strncmp(entry->d_name, "event", 5) != 0) {
            continue;
        }
        std::string path = std::string("/dev/input/") + entry->d_name;
        int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
        std::cout << "[DEBUG][DiscoverKeyboard] open(" << path << ") returned fd=" << fd << std::endl;
        if (fd < 0) {
            continue;
        }
        char name[256] = "Unknown";
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        std::cout << "[DEBUG][DiscoverKeyboard] found device: " << name << " at " << path << std::endl;
        // Convert to lowercase for case-insensitive comparison
        std::string name_lower = name;
        for (char& c : name_lower) {
            c = tolower(c);
        }
        if (name_lower.find("keyboard") != std::string::npos) {
            int priority = 50; // default
            
            // Deprioritize consumer control and system control
            if (name_lower.find("consumer control") != std::string::npos ||
                name_lower.find("system control") != std::string::npos) {
                priority = 10;
            }
            // Prioritize actual keyboard devices
            else if (name_lower.find(" keyboard") != std::string::npos) {
                priority = 100;
            }
            
            candidates.push_back({path, name, priority, fd});
        } else {
            close(fd);
        }
    }
    
    closedir(dir);
    
    if (candidates.empty()) {
        std::cerr << "No keyboard found" << std::endl;
        return false;
    }
    
    // Sort by priority (highest first)
    std::sort(candidates.begin(), candidates.end(),
              [](const KeyboardCandidate& a, const KeyboardCandidate& b) {
                  return a.priority > b.priority;
              });
    
    // Close all except the best one
    for (size_t i = 1; i < candidates.size(); i++) {
        close(candidates[i].fd);
    }
    
    // Use the highest priority device
    kbd_fd = candidates[0].fd;
    std::cout << "[DEBUG][DiscoverKeyboard] Found keyboard: " << candidates[0].name << " at " << candidates[0].path << ", fd=" << kbd_fd << std::endl;
    return true;
}

bool Input::DiscoverMouse(const std::string& device_path) {
    // If explicit device path provided, use it
    std::cout << "[DEBUG][DiscoverMouse] called with device_path='" << device_path << "'" << std::endl;
    if (!device_path.empty()) {
        mouse_fd = open(device_path.c_str(), O_RDONLY | O_NONBLOCK);
        std::cout << "[DEBUG][DiscoverMouse] open(" << device_path << ") returned fd=" << mouse_fd << std::endl;
        if (mouse_fd >= 0) {
            int flags = fcntl(mouse_fd, F_GETFL);
            std::cout << "[DEBUG][DiscoverMouse] mouse_fd F_GETFL flags=" << flags << std::endl;
            char name[256] = {0};
            if (ioctl(mouse_fd, EVIOCGNAME(sizeof(name)), name) >= 0) {
                std::cout << "[DEBUG][DiscoverMouse] mouse_fd EVIOCGNAME: '" << name << "'" << std::endl;
            }
            int grab_result = ioctl(mouse_fd, EVIOCGRAB, 0);
            std::cout << "[DEBUG][DiscoverMouse] mouse_fd EVIOCGRAB(0) result=" << grab_result << ", errno=" << errno << std::endl;

            // TEMP: Try a blocking read loop for diagnostics
            std::cout << "[DEBUG][DiscoverMouse] Starting direct blocking read test (move/click mouse)..." << std::endl;
            for (int i = 0; i < 10; ++i) {
                struct input_event ev;
                ssize_t n = read(mouse_fd, &ev, sizeof(ev));
                std::cout << "[DEBUG][DiscoverMouse] direct read n=" << n << ", errno=" << errno;
                if (n == sizeof(ev)) {
                    std::cout << ", type=" << ev.type << ", code=" << ev.code << ", value=" << ev.value;
                }
                std::cout << std::endl;
            }
            unsigned long evbit[8] = {0};
            if (ioctl(mouse_fd, EVIOCGBIT(0, sizeof(evbit)), evbit) >= 0) {
                std::cout << "[DEBUG][DiscoverMouse] Supported event types:";
                for (int i = 0; i < 64; ++i) {
                    if (evbit[0] & (1UL << i)) std::cout << " " << i;
                }
                std::cout << std::endl;
            }
            unsigned long relbit[8] = {0};
            if (ioctl(mouse_fd, EVIOCGBIT(EV_REL, sizeof(relbit)), relbit) >= 0) {
                std::cout << "[DEBUG][DiscoverMouse] Supported rel codes:";
                for (int i = 0; i < 64; ++i) {
                    if (relbit[0] & (1UL << i)) std::cout << " " << i;
                }
                std::cout << std::endl;
            }
        }
        if (mouse_fd < 0) {
            std::cerr << "Failed to open mouse device: " << device_path << ", errno=" << errno << std::endl;
            return false;
        }
        char name[256] = "Unknown";
        ioctl(mouse_fd, EVIOCGNAME(sizeof(name)), name);
        std::cout << "Using configured mouse: " << name << " at " << device_path << std::endl;
        return true;
    }
    
    // Otherwise, auto-detect
    std::cout << "[DEBUG][DiscoverMouse] auto-detecting in /dev/input" << std::endl;
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
        std::cout << "[DEBUG][DiscoverMouse] open(" << path << ") returned fd=" << fd << std::endl;
        if (fd < 0) {
            continue;
        }
        // Check for REL_X capability
        unsigned long rel_bitmask[NBITS(REL_MAX)] = {0};
        ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel_bitmask)), rel_bitmask);
        if (test_bit(REL_X, rel_bitmask)) {
            char name[256] = "Unknown";
            ioctl(fd, EVIOCGNAME(sizeof(name)), name);
            std::cout << "[DEBUG][DiscoverMouse] found device: " << name << " at " << path << std::endl;
            std::string name_lower = name;
            for (char& c : name_lower) {
                c = tolower(c);
            }
            // Skip keyboard devices that have pointer capabilities
            if (name_lower.find("keyboard") != std::string::npos) {
                std::cout << "[DEBUG][DiscoverMouse] skipping device (keyboard capability): " << name << std::endl;
                close(fd);
                continue;
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
            // Deprioritize consumer control / system control devices
            else if (name_lower.find("consumer control") != std::string::npos ||
                     name_lower.find("system control") != std::string::npos) {
                priority = 5;
            }
            // Prioritize real mice - check for "mouse" or "wireless device" in name
            else if (name_lower.find("mouse") != std::string::npos ||
                     (name_lower.find("wireless") != std::string::npos && name_lower.find("device") != std::string::npos) ||
                     name_lower.find("beken") != std::string::npos) {
                priority = 100;
            }
            std::cout << "[DEBUG][DiscoverMouse] candidate: " << name << ", priority=" << priority << std::endl;
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
    std::cout << "[DEBUG][DiscoverMouse] open(best.path) returned fd=" << mouse_fd << std::endl;
    if (mouse_fd < 0) {
        std::cerr << "Failed to open mouse: " << best.path << ", errno=" << errno << std::endl;
        return false;
    }
    std::cout << "[DEBUG][DiscoverMouse] Found mouse: " << best.name << " at " << best.path << ", fd=" << mouse_fd << std::endl;
    return true;
}

void Input::Read(int& mouse_dx) {
    mouse_dx = 0;
    struct input_event ev;
    std::cout << "[DEBUG][Input::Read] Entered, running=" << running << ", getuid()=" << getuid() << std::endl;
    // Try a non-blocking read at startup to see if any event is available
    if (kbd_fd >= 0) {
        ssize_t n = read(kbd_fd, &ev, sizeof(ev));
        std::cout << "[DEBUG][Input::Read] Startup test read from kbd_fd, n=" << n << ", errno=" << errno << std::endl;
    }
    if (mouse_fd >= 0) {
        ssize_t n = read(mouse_fd, &ev, sizeof(ev));
        std::cout << "[DEBUG][Input::Read] Startup test read from mouse_fd, n=" << n << ", errno=" << errno << std::endl;
    }
    // struct pollfd pfds[2]; // removed unused variable
    // ...existing code for poll and event handling...
}

// --- Place these at the end of the file ---

bool Input::CheckToggle() {
    bool ctrl = keys[KEY_LEFTCTRL] || keys[KEY_RIGHTCTRL];
    bool m = keys[KEY_M];
    bool both = ctrl && m;
    bool toggled = false;
    if (both && !prev_toggle) {
        toggled = true;
    }
    prev_toggle = both;
    return toggled;
}

void Input::Grab(bool enable) {
    int grab = enable ? 1 : 0;
    if (kbd_fd >= 0 && fcntl(kbd_fd, F_GETFD) != -1) {
        if (ioctl(kbd_fd, EVIOCGRAB, grab) < 0) {
            std::cerr << "Failed to " << (enable ? "grab" : "ungrab") << " keyboard (fd=" << kbd_fd << ") errno=" << errno << std::endl;
        } else {
            std::cout << (enable ? "Grabbed" : "Ungrabbed") << " keyboard (fd=" << kbd_fd << ")" << std::endl;
        }
    } else if (enable) {
        std::cerr << "Cannot grab keyboard: invalid or closed file descriptor." << std::endl;
    }

    if (mouse_fd >= 0 && fcntl(mouse_fd, F_GETFD) != -1) {
        if (ioctl(mouse_fd, EVIOCGRAB, grab) < 0) {
            std::cerr << "Failed to " << (enable ? "grab" : "ungrab") << " mouse (fd=" << mouse_fd << ") errno=" << errno << std::endl;
        } else {
            std::cout << (enable ? "Grabbed" : "Ungrabbed") << " mouse (fd=" << mouse_fd << ")" << std::endl;
        }
    } else if (enable) {
        std::cerr << "Cannot grab mouse: invalid or closed file descriptor." << std::endl;
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
