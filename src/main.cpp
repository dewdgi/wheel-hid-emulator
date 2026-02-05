#include <algorithm>
#include <atomic>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "wheel_device.h"
#include "input/input_manager.h"
#include "logging/logger.h"

// Windows specifics
#include <windows.h>
#include <mmsystem.h>

int ParseLogLevelFromArgs(int argc, char* argv[]);

std::atomic<bool> running{true};

// Windows Console Control Handler
BOOL WINAPI CtrlHandler(DWORD fdwCtrlType) {
    switch (fdwCtrlType) {
    case CTRL_C_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        running.store(false, std::memory_order_relaxed);
        return TRUE;
    default:
        return FALSE;
    }
}

int main(int argc, char* argv[]) {
    // CRITICAL: Set timer resolution to 1ms for Physics Loop
    timeBeginPeriod(1);

    int log_level = ParseLogLevelFromArgs(argc, argv);
    logging::InitLogger(log_level);
    LOG_INFO("main", "Starting wheel emulator (Windows vJoy version) (log level=" << log_level << ")");

    if (!SetConsoleCtrlHandler(CtrlHandler, TRUE)) {
        LOG_ERROR("main", "Could not set control handler"); 
        return 1;
    }

    // Load configuration
    Config config;
    config.Load();

    WheelDevice wheel_device;
    wheel_device.SetFFBGain(config.ffb_gain);
    if (!wheel_device.Create()) {
        std::cerr << "Failed to create virtual wheel device (vJoy issue?)" << std::endl;
        timeEndPeriod(1);
        return 1;
    }

    InputManager input_manager;
    if (!input_manager.Initialize("", "")) {
        std::cerr << "Failed to initialize input manager" << std::endl;
        timeEndPeriod(1);
        return 1;
    }

    std::cout << "All systems ready. Toggle to enable." << std::endl;
    // Force enable on start for testing if desired? No, stick to toggle.

    InputFrame frame;
    while (running) {
        if (!input_manager.WaitForFrame(frame)) {
            if (!running) break;
            continue;
        }

        if (wheel_device.IsEnabled() && !input_manager.AllRequiredGrabbed()) {
             // On Windows, losing focus might not mean losing device, but strict grab logic applies
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

    wheel_device.SetEnabled(false, input_manager);
    wheel_device.NotifyAllShutdownCVs();
    input_manager.Shutdown();
    wheel_device.ShutdownThreads();
    
    timeEndPeriod(1);
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
            }
            continue;
        }
        if (arg.rfind(prefix, 0) == 0) {
            try {
                level = std::stoi(arg.substr(prefix.size()));
            } catch (...) {
            }
        }
    }
    if (level < 0) level = 0;
    if (level > 3) level = 3;
    return level;
}
