
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

    // Discover input devices
    Input input;
    std::cout << "[DEBUG][main] Calling input.DiscoverKeyboard with '" << config.keyboard_device << "'" << std::endl;
    bool kbd_ok = input.DiscoverKeyboard(config.keyboard_device);
    std::cout << "[DEBUG][main] input.DiscoverKeyboard returned " << kbd_ok << std::endl;
    std::cout << "[DEBUG][main] Calling input.DiscoverMouse with '" << config.mouse_device << "'" << std::endl;
    bool mouse_ok = input.DiscoverMouse(config.mouse_device);
    std::cout << "[DEBUG][main] input.DiscoverMouse returned " << mouse_ok << std::endl;

    int loop_counter = 0;
    while (running) {
        std::cout << "[DEBUG][main] LOOP START, count=" << loop_counter << ", running=" << running << std::endl;
        int mouse_dx = 0;
        std::cout << "[DEBUG][main] calling input.Read(), count=" << loop_counter << std::endl;
        input.Read(mouse_dx);
        std::cout << "[DEBUG][main] after input.Read, running=" << running << ", count=" << loop_counter << std::endl;
        bool toggle = input.CheckToggle();
        std::cout << "[DEBUG][main] after CheckToggle, toggle=" << toggle << ", running=" << running << ", count=" << loop_counter << std::endl;
        bool enabled = gamepad.IsEnabled();
        std::cout << "[DEBUG][main] after IsEnabled, enabled=" << enabled << ", running=" << running << ", count=" << loop_counter << std::endl;
        if (toggle) {
            std::cout << "[DEBUG][main] Toggle detected! count=" << loop_counter << std::endl;
            gamepad.ToggleEnabled(input);
            std::cout << "[DEBUG][main] after ToggleEnabled, running=" << running << ", count=" << loop_counter << std::endl;
        }
        if (enabled) {
            std::cout << "[DEBUG][main] updating steering, running=" << running << ", count=" << loop_counter << std::endl;
            gamepad.UpdateSteering(mouse_dx, config.sensitivity);
            std::cout << "[DEBUG][main] updating throttle, running=" << running << ", count=" << loop_counter << std::endl;
            gamepad.UpdateThrottle(input.IsKeyPressed(KEY_W));
            std::cout << "[DEBUG][main] updating brake, running=" << running << ", count=" << loop_counter << std::endl;
            gamepad.UpdateBrake(input.IsKeyPressed(KEY_S));
            std::cout << "[DEBUG][main] updating clutch, running=" << running << ", count=" << loop_counter << std::endl;
            gamepad.UpdateClutch(input.IsKeyPressed(KEY_A));
            std::cout << "[DEBUG][main] updating buttons, running=" << running << ", count=" << loop_counter << std::endl;
            gamepad.UpdateButtons(input);
            std::cout << "[DEBUG][main] updating dpad, running=" << running << ", count=" << loop_counter << std::endl;
            gamepad.UpdateDPad(input);
            std::cout << "[DEBUG][main] sending state, running=" << running << ", count=" << loop_counter << std::endl;
            gamepad.SendState();
        }
        std::cout << "[DEBUG][main] processing UHID events, running=" << running << ", count=" << loop_counter << std::endl;
        gamepad.ProcessUHIDEvents();
        std::cout << "[DEBUG][main] before sleep, running=" << running << ", count=" << loop_counter << std::endl;
        usleep(10000);
        std::cout << "[DEBUG][main] after sleep, running=" << running << ", count=" << loop_counter << std::endl;
        ++loop_counter;
    }
        std::cout << "[DEBUG][main] Main loop exited, running=" << running << std::endl;
    input.Grab(false);
    std::cout << "[DEBUG][main] Goodbye!" << std::endl;
    return 0;
}
