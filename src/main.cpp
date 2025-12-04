#include <algorithm>
#include <atomic>
#include <cstring>
#include <iostream>
#include <string>
#include <signal.h>
#include <unistd.h>

#include "config.h"
#include "wheel_device.h"
#include "input/input_manager.h"
#include "logging/logger.h"

int ParseLogLevelFromArgs(int argc, char* argv[]);

std::atomic<bool> running{true};

void signal_handler(int signal) {
    if (signal == SIGINT) {
        const char msg[] = "\n[signal_handler] Received Ctrl+C, shutting down...\n";
        ssize_t ignored = write(STDERR_FILENO, msg, sizeof(msg) - 1);
        (void)ignored;
        running.store(false, std::memory_order_relaxed);
    }
}

bool check_root() {
    if (geteuid() != 0) {
        std::cerr << "This program must be run as root to configure the USB gadget and grab input devices." << std::endl;
        std::cerr << "Please run with: sudo ./wheel-emulator" << std::endl;
        return false;
    }
    return true;
}

// --- main() at very end of file ---
int main(int argc, char* argv[]) {
    int log_level = ParseLogLevelFromArgs(argc, argv);
    logging::InitLogger(log_level);
    LOG_INFO("main", "Starting wheel emulator (log level=" << log_level << ")");

    if (!check_root()) {
        return 1;
    }

    // Setup signal handler
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);

    // Load configuration
    Config config;
    config.Load();

    WheelDevice wheel_device;
    wheel_device.SetFFBGain(config.ffb_gain);
    if (!wheel_device.Create()) {
        std::cerr << "Failed to create virtual wheel device" << std::endl;
        return 1;
    }

    InputManager input_manager;
    if (!input_manager.Initialize(config.keyboard_device, config.mouse_device)) {
        std::cerr << "Failed to initialize input manager" << std::endl;
        return 1;
    }

    std::cout << "All systems ready. Toggle to enable." << std::endl;

    InputFrame frame;
    while (running) {
        if (!input_manager.WaitForFrame(frame)) {
            if (!running) {
                break;
            }
            continue;
        }

        if (wheel_device.IsEnabled() && !input_manager.AllRequiredGrabbed()) {
            std::cerr << "Required input device lost; disabling emulator" << std::endl;
            wheel_device.SetEnabled(false, input_manager);
            continue;
        }

        if (frame.toggle_pressed) {
            if (!input_manager.DevicesReady()) {
                LOG_WARN("main", "Toggle pressed before devices ready; ignoring request");
            } else {
                wheel_device.ToggleEnabled(input_manager);
            }
        }

        if (wheel_device.IsEnabled()) {
            wheel_device.ProcessInputFrame(frame, config.sensitivity);
        }

    }
    // On shutdown, notify all threads to wake up and exit
    wheel_device.SetEnabled(false, input_manager);
    wheel_device.NotifyAllShutdownCVs();
    input_manager.Shutdown();
    // Signal threads to exit before destruction
    wheel_device.ShutdownThreads();
    return 0;

}

int ParseLogLevelFromArgs(int argc, char* argv[]) {
    int level = 1;  // Default to warnings/info
    const std::string prefix = "--log-level=";
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--log-level" && i + 1 < argc) {
            ++i;
            try {
                level = std::stoi(argv[i]);
            } catch (...) {
                // Ignore malformed values; keep previous level
            }
            continue;
        }
        if (arg.rfind(prefix, 0) == 0) {
            try {
                level = std::stoi(arg.substr(prefix.size()));
            } catch (...) {
                // Ignore malformed inline value
            }
        }
    }
    if (level < 0) level = 0;
    if (level > 3) level = 3;
    return level;
}
