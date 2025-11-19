
#include <iostream>
#include <atomic>
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

std::atomic<bool> running{true};

void signal_handler(int signal) {
    if (signal == SIGINT) {
        std::cout << "\n[DEBUG][signal_handler] Received Ctrl+C, shutting down..." << std::endl;
        running = false;
        std::cout << "[DEBUG][signal_handler] set running=" << running << std::endl;
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

#define BITS_PER_LONG (sizeof(long) * 8)
#define NBITS(x) ((((x)-1)/BITS_PER_LONG)+1)
#define OFF(x)  ((x)%BITS_PER_LONG)
#define BIT(x)  (1UL<<OFF(x))
#define LONG(x) ((x)/BITS_PER_LONG)
#define test_bit(bit, array) ((array[LONG(bit)] >> OFF(bit)) & 1)

int run_detection_mode() {
    // ...existing code for run_detection_mode...
    return 0;
}

// --- main() at very end of file ---
int main(int, char*[]) {
    if (!check_root()) {
        return 1;
    }

    // Setup signal handler
    signal(SIGINT, signal_handler);

    // Load configuration
    Config config;
    config.Load();

    GamepadDevice gamepad;
    if (!gamepad.Create()) {
        std::cerr << "Failed to create virtual gamepad" << std::endl;
        return 1;
    }
    std::cout << "[DEBUG][main] After gamepad.Create(), ffb_thread joinable=" << gamepad.IsFFBThreadJoinable() << std::endl;

    // Discover input devices
    Input input;
    std::cout << "[DEBUG][main] Calling input.DiscoverKeyboard with '" << config.keyboard_device << "'" << std::endl;
    bool kbd_ok = input.DiscoverKeyboard(config.keyboard_device);
    std::cout << "[DEBUG][main] input.DiscoverKeyboard returned " << kbd_ok << std::endl;
    std::cout << "[DEBUG][main] Calling input.DiscoverMouse with '" << config.mouse_device << "'" << std::endl;
    bool mouse_ok = input.DiscoverMouse(config.mouse_device);
    std::cout << "[DEBUG][main] input.DiscoverMouse returned " << mouse_ok << std::endl;

    bool printed_main_loop = false;
    while (running) {
        if (!printed_main_loop) {
            std::cout << "[DEBUG][main] Loop running, running=" << running.load() << std::endl;
            printed_main_loop = true;
        }
        // ...main loop logic...
    }
    std::cout << "[DEBUG][main] Main loop exited, running=" << running << std::endl;
    // Signal threads to exit before destruction
    std::cout << "[DEBUG][main] Calling gamepad.ShutdownThreads()" << std::endl;
    gamepad.ShutdownThreads();
    std::cout << "[DEBUG][main] Called input.Grab(false)" << std::endl;
    input.Grab(false);
    std::cout << "[DEBUG][main] Goodbye!" << std::endl;
    return 0;
}
