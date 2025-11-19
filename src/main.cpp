#include <iostream>
#include <csignal>
#include <unistd.h>
#include <cstring>
#include <vector>
#include <fcntl.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <linux/input-event-codes.h>
#include "config.h"
#include "input.h"
#include "gamepad.h"

// Global flag for clean shutdown
static volatile bool running = true;

void signal_handler(int signal) {
    if (signal == SIGINT) {
        std::cout << "\nReceived Ctrl+C, shutting down..." << std::endl;
        running = false;
    }
}

bool check_root() {
    if (geteuid() != 0) {
        std::cerr << "This program must be run as root to access /dev/uinput and grab input devices." << std::endl;
        std::cerr << "Please run with: sudo ./wheel-emulator" << std::endl;
        return false;
    }
    return true;
}

// Bit manipulation macros for input device capabilities
#define BITS_PER_LONG (sizeof(long) * 8)
#define NBITS(x) ((((x)-1)/BITS_PER_LONG)+1)
#define OFF(x)  ((x)%BITS_PER_LONG)
#define BIT(x)  (1UL<<OFF(x))
#define LONG(x) ((x)/BITS_PER_LONG)
#define test_bit(bit, array) ((array[LONG(bit)] >> OFF(bit)) & 1)

int run_detection_mode() {
    std::cout << "=== Device Detection Mode ===" << std::endl;
    std::cout << "This will help you identify the correct keyboard and mouse devices." << std::endl;
    std::cout << std::endl;
    
    // Open all input devices
    struct DeviceInfo {
        std::string path;
        std::string name;
        int fd;
        bool has_keys;
        bool has_rel_x;
        int key_events;
        int mouse_events;
    };
    std::vector<DeviceInfo> devices;
    
    DIR* dir = opendir("/dev/input");
    if (!dir) {
        std::cerr << "Failed to open /dev/input" << std::endl;
        return 1;
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
        
        // Check capabilities
        unsigned long evbit[NBITS(EV_MAX)];
        unsigned long keybit[NBITS(KEY_MAX)];
        unsigned long relbit[NBITS(REL_MAX)];
        
        memset(evbit, 0, sizeof(evbit));
        memset(keybit, 0, sizeof(keybit));
        memset(relbit, 0, sizeof(relbit));
        
        ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), evbit);
        
        bool has_keys = false;
        bool has_rel_x = false;
        
        if (test_bit(EV_KEY, evbit)) {
            ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybit)), keybit);
            // Check for actual keyboard keys (not just BTN_*)
            if (test_bit(KEY_A, keybit) || test_bit(KEY_SPACE, keybit)) {
                has_keys = true;
            }
        }
        
        if (test_bit(EV_REL, evbit)) {
            ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relbit)), relbit);
            if (test_bit(REL_X, relbit)) {
                has_rel_x = true;
            }
        }
        
        devices.push_back({path, name, fd, has_keys, has_rel_x, 0, 0});
    }
    closedir(dir);
    
    std::cout << "Found " << devices.size() << " input devices." << std::endl;
    std::cout << std::endl;
    
    // Keyboard detection
    std::cout << "=== KEYBOARD DETECTION ===" << std::endl;
    std::cout << "Please type some keys (e.g., type 'hello')..." << std::endl;
    std::cout << "Monitoring for 5 seconds..." << std::endl;
    
    for (int i = 0; i < 50; i++) {  // 5 seconds at 100ms intervals
        for (auto& dev : devices) {
            if (!dev.has_keys) continue;
            
            struct input_event ev;
            while (read(dev.fd, &ev, sizeof(ev)) > 0) {
                if (ev.type == EV_KEY && ev.value == 1) {  // Key press only
                    dev.key_events++;
                }
            }
        }
        usleep(100000);  // 100ms
    }
    
    std::cout << std::endl << "Keyboard detection results:" << std::endl;
    DeviceInfo* best_keyboard = nullptr;
    for (auto& dev : devices) {
        if (dev.key_events > 0) {
            std::cout << "  " << dev.path << ": " << dev.name << " (" << dev.key_events << " key presses)" << std::endl;
            if (!best_keyboard || dev.key_events > best_keyboard->key_events) {
                best_keyboard = &dev;
            }
        }
    }
    
    if (!best_keyboard) {
        std::cout << "  No keyboard detected! Did you type?" << std::endl;
    }
    
    // Mouse detection
    std::cout << std::endl << "=== MOUSE DETECTION ===" << std::endl;
    std::cout << "Please move your mouse..." << std::endl;
    std::cout << "Monitoring for 5 seconds..." << std::endl;
    
    for (int i = 0; i < 50; i++) {  // 5 seconds at 100ms intervals
        for (auto& dev : devices) {
            if (!dev.has_rel_x) continue;
            
            struct input_event ev;
            while (read(dev.fd, &ev, sizeof(ev)) > 0) {
                if (ev.type == EV_REL && ev.code == REL_X) {
                    dev.mouse_events++;
                }
            }
        }
        usleep(100000);  // 100ms
    }
    
    std::cout << std::endl << "Mouse detection results:" << std::endl;
    DeviceInfo* best_mouse = nullptr;
    for (auto& dev : devices) {
        if (dev.mouse_events > 0) {
            std::cout << "  " << dev.path << ": " << dev.name << " (" << dev.mouse_events << " movement events)" << std::endl;
            if (!best_mouse || dev.mouse_events > best_mouse->mouse_events) {
                best_mouse = &dev;
            }
        }
    }
    
    if (!best_mouse) {
        std::cout << "  No mouse detected! Did you move the mouse?" << std::endl;
    }
    
    // Close all devices
    for (auto& dev : devices) {
        close(dev.fd);
    }
    
    // Output configuration
    std::cout << std::endl << "=== DETECTED DEVICES ===" << std::endl;
    if (best_keyboard) {
        std::cout << "Keyboard: " << best_keyboard->path << " (" << best_keyboard->name << ")" << std::endl;
    } else {
        std::cout << "Keyboard: NOT DETECTED" << std::endl;
    }
    if (best_mouse) {
        std::cout << "Mouse: " << best_mouse->path << " (" << best_mouse->name << ")" << std::endl;
    } else {
        std::cout << "Mouse: NOT DETECTED" << std::endl;
    }
    std::cout << std::endl;
    
    // Update config file if both devices detected
    if (best_keyboard && best_mouse) {
        Config config;
        // Ensure config exists (will create if missing)
        config.Load();
        
        std::cout << "Updating /etc/wheel-emulator.conf with detected devices..." << std::endl;
        if (config.UpdateDevices(best_keyboard->path, best_mouse->path)) {
            std::cout << "Success! You can now run the emulator normally." << std::endl;
        } else {
            std::cout << "Failed to update config. Please manually add:" << std::endl;
            std::cout << "  keyboard=" << best_keyboard->path << std::endl;
            std::cout << "  mouse=" << best_mouse->path << std::endl;
        }
    } else {
        std::cout << "Cannot update config: not all devices detected." << std::endl;
        std::cout << "Please ensure you typed on keyboard and moved mouse during detection." << std::endl;
    }
    
    return 0;
}

int main(int argc, char* argv[]) {
    // Check root privileges
    if (!check_root()) {
        return 1;
    }
    
    // Check for --detect flag
    if (argc > 1 && strcmp(argv[1], "--detect") == 0) {
        return run_detection_mode();
    }
    
    std::cout << "=== Wheel Emulator ===" << std::endl;
    std::cout << "Transform keyboard+mouse into Logitech G29 Racing Wheel for racing games" << std::endl;
    std::cout << std::endl;
    
    // Setup signal handler
    signal(SIGINT, signal_handler);
    
    // Load configuration
    Config config;
    if (!config.Load()) {
        std::cerr << "Failed to load configuration" << std::endl;
        return 1;
    }
    std::cout << "Sensitivity: " << config.sensitivity << std::endl;
    std::cout << std::endl;
    
    // Create virtual gamepad
    GamepadDevice gamepad;
    if (!gamepad.Create()) {
        std::cerr << "Failed to create virtual gamepad" << std::endl;
        return 1;
    }
    std::cout << std::endl;
    
    // Discover input devices
    Input input;
    if (!input.DiscoverKeyboard(config.keyboard_device)) {
        std::cerr << "Failed to discover keyboard" << std::endl;
        return 1;
    }
    
    if (!input.DiscoverMouse(config.mouse_device)) {
        std::cerr << "Failed to discover mouse" << std::endl;
        return 1;
    }
    std::cout << std::endl;
    
    std::cout << std::endl;
    std::cout << "Emulation is OFF. Press Ctrl+M to enable." << std::endl;
    std::cout << std::endl;

    // Main loop
    while (running) {
        int mouse_dx = 0;

        // Read input events
        input.Read(mouse_dx);

        // Check for toggle (Ctrl+M)
        if (input.CheckToggle()) {
            gamepad.ToggleEnabled(input);
        }

        if (gamepad.IsEnabled()) {
            // Update gamepad state
            gamepad.UpdateSteering(mouse_dx, config.sensitivity);
            gamepad.UpdateThrottle(input.IsKeyPressed(KEY_W));
            gamepad.UpdateBrake(input.IsKeyPressed(KEY_S));
            gamepad.UpdateButtons(input);
            gamepad.UpdateDPad(input);
            gamepad.SendState();
        }
        // Don't send any reports when disabled - let device stay at last state

        // Process UHID events (for FFB and kernel requests)
        gamepad.ProcessUHIDEvents();

        // Sleep for 8ms (125 Hz update rate, matching real racing wheels)
        usleep(8000);
    }
    
    // Cleanup
    std::cout << "Cleaning up..." << std::endl;
    input.Grab(false);
    
    std::cout << "Goodbye!" << std::endl;
    return 0;
}
