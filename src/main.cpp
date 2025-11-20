#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <linux/input-event-codes.h>
#include <signal.h>
#include <unistd.h>

#include "config.h"
#include "gamepad.h"
#include "input.h"

void notify_all_shutdown(Input& input, GamepadDevice& gamepad);

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
int main(int, char*[]) {
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

    GamepadDevice gamepad;
    gamepad.SetFFBGain(config.ffb_gain);
    if (!gamepad.Create()) {
        std::cerr << "Failed to create virtual gamepad" << std::endl;
        return 1;
    }

    // Discover input devices
    Input input;
    input.DiscoverKeyboard(config.keyboard_device);
    input.DiscoverMouse(config.mouse_device);

    auto last_tick = std::chrono::steady_clock::now();
    while (running) {
        input.WaitForEvents(8);

        int mouse_dx = 0;
        input.Read(mouse_dx);

        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - last_tick).count();
        last_tick = now;
        if (dt < 0.0f) {
            dt = 0.0f;
        }
        if (dt > 0.05f) {
            dt = 0.05f;
        }

        if (input.CheckToggle()) {
            gamepad.ToggleEnabled(input);
        }

        if (gamepad.IsEnabled()) {
            gamepad.UpdateSteering(mouse_dx, config.sensitivity);
            gamepad.UpdateThrottle(input.IsKeyPressed(KEY_W), dt);
            gamepad.UpdateBrake(input.IsKeyPressed(KEY_S), dt);
            gamepad.UpdateClutch(input.IsKeyPressed(KEY_A), dt);
            gamepad.UpdateButtons(input);
            gamepad.UpdateDPad(input);
            gamepad.SendState();
        }

    }
    // On shutdown, notify all threads to wake up and exit
    gamepad.SetEnabled(false, input);
    notify_all_shutdown(input, gamepad);
    // Signal threads to exit before destruction
    gamepad.ShutdownThreads();
    input.Grab(false);
    return 0;

}

void notify_all_shutdown(Input& input, GamepadDevice& gamepad) {
    // Wake all threads waiting on condition variables
    input.input_cv.notify_all();
    gamepad.NotifyAllShutdownCVs();
}
