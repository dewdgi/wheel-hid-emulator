---

## Full Logical Structure: Loops, Conditionals, and Control Flow


This section documents all logical constructs in the codebase, including every loop, conditional, function, and major control flow structure, for strict audit and traceability.

**Critical Note (as of Nov 2025):**
All input reading and device I/O in the main loop (e.g., `input.Read()`) must be performed in non-blocking mode or be interruptible by signals (handle `EINTR`), to ensure the main loop remains responsive to both Ctrl+M (enable/disable) and Ctrl+C (shutdown). Blocking I/O without signal interruption can cause the main loop to hang, making the emulator unresponsive to user input and shutdown requests.

**Thread Shutdown and Device Grabbing:**
- All background threads (FFB, USB Gadget) must check both their own running flag and the global `running` flag, and use short poll/sleep intervals to ensure prompt shutdown on Ctrl+C or exit.
- Device grabbing (`EVIOCGRAB`) must only be attempted on valid, open file descriptors. Always check file descriptor validity before grabbing or ungrabbing devices, and log errors if the descriptor is invalid or closed.

### `src/main.cpp`

- **Functions:**
  - `main()`
    - `if (!check_root()) return 1;`
    - `if (argc > 1 && strcmp(argv[1], "--detect") == 0) return run_detection_mode();`
    - `if (!config.Load()) return 1;`
    - `if (!gamepad.Create()) return 1;`
    - `if (!input.DiscoverKeyboard(...)) return 1;`
    - `if (!input.DiscoverMouse(...)) return 1;`
    - `while (running)` (main loop)
      - `input.Read(mouse_dx);`
      - `if (input.CheckToggle()) gamepad.ToggleEnabled(input);`
      - `if (gamepad.IsEnabled())` (enabled block)
        - `gamepad.UpdateSteering(...)`
        - `gamepad.UpdateThrottle(...)`
        - `gamepad.UpdateBrake(...)`
        - `gamepad.UpdateClutch(...)`
        - `gamepad.UpdateButtons(...)`
        - `gamepad.UpdateDPad(...)`
        - `gamepad.SendState();`
      - `gamepad.ProcessUHIDEvents();`
      - `usleep(8000);`
    - `input.Grab(false);` (cleanup)
  - `signal_handler(int)`
    - `if (signal == SIGINT) running = false;`
  - `check_root()`
    - `if (geteuid() != 0) return false;`
  - `run_detection_mode()`
    - `if (!dir) return ...;`
    - `while ((entry = readdir(dir)) != nullptr)`
    - `if (strncmp(entry->d_name, "event", 5) != 0) continue;`
    - ... (device detection logic)

### `src/gamepad.cpp`/`.h`

- **Class:** `GamepadDevice`
  - **Functions:**
    - `GamepadDevice()`, `~GamepadDevice()`
      - `if (ffb_running) ...` (thread join)
      - `if (gadget_running) ...` (thread join)
      - `if (fd >= 0) ...` (device destroy)
    - `SetEnabled(bool, Input&)`
      - `if (enabled == enable) return;`
      - `input.Grab(enable);`
    - `IsEnabled()`
    - `ToggleEnabled(Input&)`
      - `enabled = !enabled;`
      - `input.Grab(enabled);`
    - `Create()`, `CreateUSBGadget()`, `CreateUHID()`, `CreateUInput()`
      - `if (CreateUSBGadget()) return true;`
      - `if (CreateUHID()) return true;`
      - `if (fd < 0) return false;`
      - `if (ret != 0) return false;`
      - `while ((entry = readdir(dir)) != nullptr)`
      - `if (strncmp(entry->d_name, "event", 5) == 0)`
      - ... (USB gadget setup logic)
    - `UpdateSteering(int, int)`
      - `if (delta > -2 && delta < 2) delta = 0;`
      - `user_torque = ...;`
    - `UpdateThrottle(bool)`, `UpdateBrake(bool)`, `UpdateClutch(bool)`
      - `if (pressed) ... else ...` (ramp logic)
    - `UpdateButtons(const Input&)`
      - `buttons["BTN_SOUTH"] = input.IsKeyPressed(KEY_Q);` (and all 26 buttons)
    - `UpdateDPad(const Input&)`
      - `int right = input.IsKeyPressed(KEY_RIGHT) ? 1 : 0;` (and similar for left, up, down)
      - `dpad_x = right - left; dpad_y = down - up;`
    - `SendState()`
      - `if (fd < 0) return;`
      - `if (use_uhid) { SendUHIDReport(); return; }`
      - `std::lock_guard<std::mutex> lock(state_mutex);`
    - `SendUHIDReport()`, `BuildHIDReport()`
      - `std::lock_guard<std::mutex> lock(state_mutex);`
    - `ProcessUHIDEvents()`
      - `while (poll(...))` (event loop)
      - `if (revents & POLLIN)`
      - `if (revents & POLLOUT)`
    - `ParseFFBCommand(const uint8_t*, size_t)`
      - `switch (data[0]) { ... }` (FFB command parsing)
    - `FFBUpdateThread()`
      - `while (ffb_running)` (physics loop)
        - `std::lock_guard<std::mutex> lock(state_mutex);`
        - Physics: `velocity += ...; velocity *= ...; steering += ...;`
        - `if (steering < -32768) steering = -32768;` (clamp)
    - `USBGadgetPollingThread()`
      - `while (gadget_running && running)` (polling loop)
        - `poll(...);` (wait for host)
        - `if (revents & POLLIN)`
        - `if (revents & POLLOUT)`

### `src/input.cpp`/`.h`

- **Class:** `Input`
  - **Functions:**
    - `Input()`, `~Input()`
    - `DiscoverKeyboard(const std::string&)`
      - `if (!device_path.empty()) ... else ...`
      - `while ((entry = readdir(dir)) != nullptr)`
      - `if (name_lower.find("keyboard") != std::string::npos)`
      - `if (candidates.empty()) return false;`
    - `DiscoverMouse(const std::string&)`
      - `if (!device_path.empty()) ... else ...`
      - `while ((entry = readdir(dir)) != nullptr)`
      - `if (test_bit(REL_X, rel_bitmask)) ...`
      - `if (candidates.empty()) return false;`
    - `Read(int&)`
      - `if (kbd_fd >= 0) while (read(...))` (keyboard event loop, **must be non-blocking or handle EINTR**)
      - `if (mouse_fd >= 0) while (read(...))` (mouse event loop, **must be non-blocking or handle EINTR**)
      - `if (ev.type == EV_KEY && ev.code < KEY_MAX)`
      - `if (ev.type == EV_REL && ev.code == REL_X)`
    - `CheckToggle()`
      - `bool ctrl = keys[KEY_LEFTCTRL] || keys[KEY_RIGHTCTRL];`
      - `bool m = keys[KEY_M];`
      - `bool both = ctrl && m;`
      - `if (both && !prev_toggle) toggled = true;`
      - `prev_toggle = both;`
      - `return toggled;`
    - `Grab(bool)`
      - `if (kbd_fd >= 0) ... if (ioctl(...) < 0) ... else ...`
      - `if (mouse_fd >= 0) ... if (ioctl(...) < 0) ... else ...`
    - `IsKeyPressed(int) const`
      - `if (keycode >= 0 && keycode < KEY_MAX) return keys[keycode];`

### `src/config.cpp`/`.h`

- **Class:** `Config`
  - **Functions:**
    - `Load()`
      - `if (LoadFromFile(...)) return true;`
      - `SaveDefault(...);`
    - `LoadFromFile(const char*)`
      - `if (!file.is_open()) return false;`
    - `ParseINI(const std::string&)`
      - `while (std::getline(stream, line))` (line loop)
      - `if (line.empty() || line[0] == '#' || line[0] == ';') continue;`
      - `if (line[0] == '[' && line[line.length() - 1] == ']') ... continue;`
      - `if (eq_pos == std::string::npos) continue;`
      - `if (section == "devices") ... else if (section == "sensitivity") ... else if (section == "button_mapping") ...`
    - `SaveDefault(const char*)`
      - `if (!file.is_open()) return;`
    - `UpdateDevices(const std::string&, const std::string&)`
      - `if (!LoadFromFile(...)) return false;`

---

**Summary:**
- All logical constructs (loops, if/else, switches, function definitions, mutexes, threads, and major control flow) are now explicitly documented and mapped to their source files and line-level logic. This enables full traceability and strict audit compliance.
- **All input reading and device I/O in the main loop must be non-blocking or signal-interruptible (handle EINTR) to guarantee responsiveness to Ctrl+M and Ctrl+C.**

# Wheel HID Emulator: Logic & Architecture

---

## File Structure (as of November 19, 2025)

```
wheel-hid-emulator/
├── Makefile
├── README.md
├── logics.md
├── cleanup_gadget.sh
├── test_descriptor.py
├── wheel-emulator (binary, built)
└── src/
  ├── main.cpp
  ├── gamepad.cpp
  ├── gamepad.h
  ├── input.cpp
  ├── input.h
  ├── config.cpp
  └── config.h
```

---

## Code Structure: Loops, Threads, and Function Calls


### Main Loop (`src/main.cpp`)

**Location:** `int main()`

**Flow:**
1. Check for root privileges
2. Load config (`Config::Load()`)
3. Create virtual device (`GamepadDevice::Create()`)
4. Discover keyboard/mouse (`Input::DiscoverKeyboard()`, `Input::DiscoverMouse()`)
5. Print status, wait for enable
6. **Main loop (while running):**
  - `input.Read(mouse_dx)` (**must be non-blocking or handle EINTR to remain responsive to signals and toggles**)
  - `input.CheckToggle()` (Ctrl+M toggles enabled state)
  - If enabled:
    - `gamepad.UpdateSteering(mouse_dx, config.sensitivity)`
    - `gamepad.UpdateThrottle(input.IsKeyPressed(KEY_W))`
    - `gamepad.UpdateBrake(input.IsKeyPressed(KEY_S))`
    - `gamepad.UpdateClutch(input.IsKeyPressed(KEY_A))`
    - `gamepad.UpdateButtons(input)`
    - `gamepad.UpdateDPad(input)`
    - `gamepad.SendState()`
  - `gamepad.ProcessUHIDEvents()`
  - `usleep(8000)` (8ms, 125Hz)
7. On exit: cleanup, ungrab devices

---

### Gamepad Device (`src/gamepad.cpp`/`.h`)

**Class:** `GamepadDevice`

**Key Methods:**
- `Create()`: Tries USB Gadget, then UHID, then uinput
- `UpdateSteering(int delta, int sensitivity)`: Sets user torque from mouse
- `UpdateThrottle(bool)`, `UpdateBrake(bool)`, `UpdateClutch(bool)`: Ramping logic for pedals
- `UpdateButtons(const Input&)`: Maps 26 buttons from keyboard
- `UpdateDPad(const Input&)`: Maps D-Pad from arrow keys
- `SendState()`: Sends HID report (mutex-protected)
- `ProcessUHIDEvents()`: Handles FFB/OUTPUT events
- `SetEnabled(bool, Input&)`, `ToggleEnabled(Input&)`, `IsEnabled()`: Atomic enable/disable, grabs/ungrabs devices

**Threads:**
- `FFBUpdateThread()`: Physics simulation at 125Hz (mutex-protected)
- `USBGadgetPollingThread()`: Handles USB host polling (event-driven)

**Mutex Discipline:**
- All state updates and report sending are protected by `state_mutex`.

---

### Input System (`src/input.cpp`/`.h`)

**Class:** `Input`

**Key Methods:**
- `DiscoverKeyboard()`, `DiscoverMouse()`: Device detection/selection
- `Read(int&)`: Reads events, updates key/mouse state
- `CheckToggle()`: Detects Ctrl+M edge for enable/disable
- `Grab(bool)`: Grabs/ungrabs devices with `EVIOCGRAB`
- `IsKeyPressed(int)`: Returns key state

---

### Config System (`src/config.cpp`/`.h`)

**Class:** `Config`

**Key Methods:**
- `Load()`: Loads config from `/etc/wheel-emulator.conf`
- `SaveDefault(const char*)`: Writes default config
- `UpdateDevices()`: Updates device paths
- `ParseINI(const std::string&)`: INI parsing logic

---

## Threading and Synchronization

- **Main Thread:** Runs main loop, handles input, state update, and report sending
- **FFB Physics Thread:** Runs `FFBUpdateThread()` in `GamepadDevice` (125Hz)
- **USB Gadget Polling Thread:** Runs `USBGadgetPollingThread()` if in USB Gadget mode
- **Mutex:** `state_mutex` in `GamepadDevice` protects all shared state (steering, pedals, buttons, enabled, etc.)

---

## Function/Call Reference (by file)

### `src/main.cpp`
- `main()`
  - `signal_handler()`
  - `check_root()`
  - `run_detection_mode()`
  - `Config::Load()`
  - `GamepadDevice::Create()`
  - `Input::DiscoverKeyboard()`
  - `Input::DiscoverMouse()`
  - `Input::Read()`
  - `Input::CheckToggle()`
  - `GamepadDevice::ToggleEnabled()`
  - `GamepadDevice::IsEnabled()`
  - `GamepadDevice::UpdateSteering()`
  - `GamepadDevice::UpdateThrottle()`
  - `GamepadDevice::UpdateBrake()`
  - `GamepadDevice::UpdateClutch()`
  - `GamepadDevice::UpdateButtons()`
  - `GamepadDevice::UpdateDPad()`
  - `GamepadDevice::SendState()`
  - `GamepadDevice::ProcessUHIDEvents()`

### `src/gamepad.cpp`/`.h`
- `GamepadDevice::Create()`
- `GamepadDevice::CreateUSBGadget()`
- `GamepadDevice::CreateUHID()`
- `GamepadDevice::CreateUInput()`
- `GamepadDevice::UpdateSteering()`
- `GamepadDevice::UpdateThrottle()`
- `GamepadDevice::UpdateBrake()`
- `GamepadDevice::UpdateClutch()`
- `GamepadDevice::UpdateButtons()`
- `GamepadDevice::UpdateDPad()`
- `GamepadDevice::SendState()`
- `GamepadDevice::SendUHIDReport()`
- `GamepadDevice::BuildHIDReport()`
- `GamepadDevice::ProcessUHIDEvents()`
- `GamepadDevice::ParseFFBCommand()`
- `GamepadDevice::FFBUpdateThread()`
- `GamepadDevice::USBGadgetPollingThread()`
- `GamepadDevice::SetEnabled()`
- `GamepadDevice::ToggleEnabled()`
- `GamepadDevice::IsEnabled()`

### `src/input.cpp`/`.h`
- `Input::DiscoverKeyboard()`
- `Input::DiscoverMouse()`
- `Input::Read()`
- `Input::CheckToggle()`
- `Input::Grab()`
- `Input::IsKeyPressed()`

### `src/config.cpp`/`.h`
- `Config::Load()`
- `Config::SaveDefault()`
- `Config::UpdateDevices()`
- `Config::ParseINI()`
- `Config::LoadFromFile()`

---

## Notes
- All loops, threads, and function calls are now fully documented and mapped to their source files and responsibilities.
- This section must be kept up to date with any code or architectural changes for strict audit compliance.

**Last Updated:** November 19, 2025  
**Version:** 2.0 (G29-accurate, all race fixes applied)

---

## Overview

This project fully emulates a Logitech G29 wheel, including all axes, pedals, and buttons, matching the real device in every detail. The emulator presents itself as a G29-compatible HID device, ensuring maximum compatibility with games and drivers.

- **Axes:** Steering, Accelerator, Brake, Clutch (all inverted pedals as per G29 spec)
- **Button count:** 26 (matching the real G29: 13 base, 1 dead, 12 trigger-happy)
- **Force Feedback:** Supported, with physics loop
- **HID Descriptor:** Matches Linux kernel `hid-lg4ff` driver for G29

---

## HID Descriptor

The HID report descriptor and axis ranges are modeled after the real G29 as implemented in the Linux kernel (`drivers/hid/hid-lg4ff.c`):

- **Steering (ABS_X):** 0–65535, center at 32767/32768 (unsigned 16-bit)
- **Accelerator (ABS_Z):** 0–65535, inverted (0 = pressed, 65535 = released)
- **Brake (ABS_RZ):** 0–65535, inverted (0 = pressed, 65535 = released)
- **Clutch (ABS_Y):** 0–65535, inverted (0 = pressed, 65535 = released)
- **All axes:** unsigned 16-bit, as per G29 HID

**Note:** Inversion of pedals is by design and matches the real G29 hardware and Linux driver.

---

## Button Mapping

The G29 has 26 buttons, mapped as follows (matching Linux input codes):

| Button Name         | Linux Code    | HID Report Index |
|---------------------|--------------|------------------|
| Cross               | BTN_SOUTH    | 0                |
| Circle              | BTN_EAST     | 1                |
| Square              | BTN_WEST     | 2                |
| Triangle            | BTN_NORTH    | 3                |
| L1                  | BTN_TL       | 4                |
| R1                  | BTN_TR       | 5                |
| L2                  | BTN_TL2      | 6                |
| R2                  | BTN_TR2      | 7                |
| Share               | BTN_SELECT   | 8                |
| Options             | BTN_START    | 9                |
| L3                  | BTN_THUMBL   | 10               |
| R3                  | BTN_THUMBR   | 11               |
| PS                  | BTN_MODE     | 12               |
| Dead                | BTN_DEAD     | 13               |
| D-pad Up            | BTN_TRIGGER_HAPPY1 | 14         |
| D-pad Down          | BTN_TRIGGER_HAPPY2 | 15         |
| D-pad Left          | BTN_TRIGGER_HAPPY3 | 16         |
| D-pad Right         | BTN_TRIGGER_HAPPY4 | 17         |
| Red 1               | BTN_TRIGGER_HAPPY5 | 18         |
| Red 2               | BTN_TRIGGER_HAPPY6 | 19         |
| Red 3               | BTN_TRIGGER_HAPPY7 | 20         |
| Red 4               | BTN_TRIGGER_HAPPY8 | 21         |
| Red 5               | BTN_TRIGGER_HAPPY9 | 22         |
| Red 6               | BTN_TRIGGER_HAPPY10| 23         |
| Rotary Left         | BTN_TRIGGER_HAPPY11| 24         |
| Rotary Right        | BTN_TRIGGER_HAPPY12| 25         |

**Total:** 26 buttons, matching the real G29.

---

## Technical Specifications

| Feature         | Value (matches G29)      |
|-----------------|-------------------------|
| Steering        | ABS_X, 0–65535 unsigned |
| Accelerator     | ABS_Z, 0–65535 unsigned, inverted |
| Brake           | ABS_RZ, 0–65535 unsigned, inverted |
| Clutch          | ABS_Y, 0–65535 unsigned, inverted |
| Buttons         | 26                      |
| Force Feedback  | Yes                     |

---

## FFB Physics System

The force feedback (FFB) physics loop operates in signed 16-bit integer space (−32768…32767) for internal calculations. Before sending values to the HID report, these are mapped to the unsigned 0–65535 range as required by the G29 HID protocol.

- **Steering:** Internal signed value mapped to unsigned HID range.
- **Pedals:** Internal logic inverts and scales to match G29 pedal inversion.

**Conversion Example:**
```c
// Internal: int16_t steering; // -32768 ... 32767
// HID:      uint16_t hid_steering = (uint16_t)(steering + 32768);
```

---

## State Synchronization & Ungrab Guarantee

The race condition in state synchronization has been fully resolved as of the current version. The emulator guarantees atomic updates to all state variables, matching the G29's behavior.

---

## Code Version

Current (FFB physics improvements + race condition fix **applied**)

---

## References

- Linux kernel source: `drivers/hid/hid-lg4ff.c`
- Logitech G29 hardware documentation
**Key Features:**
- Real USB device emulation (USB Gadget ConfigFS)
- Full Force Feedback (FFB) support with bidirectional communication
- Physics-based steering simulation
- 25 buttons + D-Pad + 4 axes (steering, brake, throttle, unused Y)
- Configurable sensitivity and device paths
- Interactive device detection mode (`--detect`) for user identification

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────┐
│                         Main Loop                            │
│                        (125 Hz)                              │
│  ┌────────────────────────────────────────────────────────┐ │
│  │ 1. Read Keyboard/Mouse Input (evdev)                   │ │
│  │ 2. Check Ctrl+M Toggle (Enable/Disable)                │ │
│  │ 3. Update Gamepad State (buttons, pedals, dpad)        │ │
│  │ 4. Update Mouse Input → user_torque                    │ │
│  │ 5. Send HID Report (steering, pedals, buttons)         │ │
│  │ 6. Process UHID Events (FFB commands from game)        │ │
│  └────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
                              ↕
┌─────────────────────────────────────────────────────────────┐
│                      FFB Physics Thread                      │
│                        (125 Hz)                              │
│  ┌────────────────────────────────────────────────────────┐ │
│  │ 1. Calculate total_torque:                             │ │
│  │    = ffb_force (from game)                             │ │
│  │    + user_torque (from mouse)                          │ │
│  │    + autocenter_spring (pull to center)                │ │
│  │ 2. Update velocity: velocity += torque * 0.001         │ │
│  │ 3. Apply damping: velocity *= 0.98                     │ │
│  │ 4. Update position: steering += velocity               │ │
│  │ 5. Clamp steering to [-32768, 32767]                   │ │
│  └────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
                              ↕
┌─────────────────────────────────────────────────────────────┐
│                  USB Gadget Polling Thread                   │
│                        (Event-Driven)                        │
│  ┌────────────────────────────────────────────────────────┐ │
│  │ - Waits for host polls using poll(POLLIN|POLLOUT)     │ │
│  │ - Sends INPUT reports (wheel state) on POLLOUT         │ │
│  │ - Receives OUTPUT reports (FFB commands) on POLLIN     │ │
│  │ - Bidirectional USB HID communication                  │ │
│  └────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
```

---

## Component Details

### 1. Input System (`input.cpp`)

**Purpose:** Read raw keyboard and mouse events from `/dev/input/eventX` devices.

**Device Discovery:**
- Priority-based auto-detection of keyboards and mice
- Filters out unwanted devices (consumer control, touchpads, etc.)
- Supports explicit device paths from config
- Interactive detection mode (`--detect`) for user identification

**Key State Tracking:**
- Maintains boolean array `keys[KEY_MAX]` for all key states
- Accumulates mouse X delta per frame (steering input)
- Edge detection for Ctrl+M toggle (enable/disable emulation)

**Device Grabbing:**
- `EVIOCGRAB` for exclusive device access when emulation enabled
- Prevents desktop from receiving input during gameplay
- Released on Ctrl+M toggle or Ctrl+C exit

---

### 2. Virtual Device Creation (`gamepad.cpp`)

**Three Methods (Priority Order):**

#### Method 1: USB Gadget ConfigFS (Preferred)
- **Path:** `/dev/hidg0`
- **Creates:** Real USB HID device via Linux kernel's USB Gadget framework
- **Driver Binding:** Kernel's `hid-lg` driver binds automatically (VID:046d PID:c24f)
- **FFB Support:** Full bidirectional communication via OUTPUT reports
- **Requirements:** `CONFIG_USB_CONFIGFS=y`, `dummy_hcd` or real UDC
- **Advantages:** 
  - Games see authentic USB device on bus
  - Proper USB interface enumeration
  - Windows/Wine compatibility
  - Native driver support

#### Method 2: UHID (Fallback)
- **Path:** `/dev/uhid`
- **Creates:** Userspace HID device via UHID kernel interface
- **Driver Binding:** Creates hidraw device, but not proper USB device
- **FFB Support:** OUTPUT reports via `UHID_OUTPUT` events
- **Requirements:** `uhid` kernel module
- **Advantages:**
  - Provides hidraw interface (better than uinput)
  - FFB communication possible
  - No USB Gadget hardware needed

#### Method 3: UInput (Legacy)
- **Path:** `/dev/uinput`
- **Creates:** Input device via uinput subsystem
- **Driver Binding:** Generic joystick/gamepad, no hid-lg
- **FFB Support:** Registers capabilities but no actual FFB
- **Advantages:**
  - Always available
  - Simple API
- **Disadvantages:**
  - No HIDRAW device
  - No FFB communication
  - Games may not recognize as G29

---

### 3. HID Descriptor

**Logitech G29 HID Report Descriptor:**
```
Application: Joystick (0x04)
- INPUT Report (13 bytes, no report ID):
  - ABS_X (16-bit): Steering wheel (0-65535, center=32768)
  - ABS_Y (16-bit): Unused, constant 65535
  - ABS_Z (16-bit): Brake pedal (inverted: 65535=rest, 0=pressed)
  - ABS_RZ (16-bit): Throttle pedal (inverted: 65535=rest, 0=pressed)
  - HAT0 (4-bit): D-Pad (8 directions + neutral)
  - Buttons (25-bit): BTN_1 through BTN_25
  
- OUTPUT Report (7 bytes, no report ID):
  - FFB command buffer for hid-lg driver
  - Commands: constant force, autocenter, range, LEDs, etc.
```

**Critical Design:**
- **No Report IDs:** G29 uses simple descriptor without report IDs
- **Inverted Pedals:** Real G29 firmware uses inverted axes (32767=rest)
- **OUTPUT Report Required:** Must be present for hid-lg driver binding

---

### 4. Force Feedback System

**Architecture:** Physics-based simulation running in dedicated thread.

#### FFB Command Processing

**Game → Driver → Emulator:**
1. Game sends FFB effect via kernel FFB API
2. Kernel's `hid-lg` driver translates to G29 protocol
3. Driver sends 7-byte OUTPUT report to device
4. Emulator parses command in `ParseFFBCommand()`

**Supported Commands:**
- `0x11`: Constant force effect (main steering forces)
- `0x13`: Stop effect / disable force
- `0x14`: Enable autocenter spring
- `0xf5`: Disable autocenter spring
- `0xfe`: Set autocenter parameters (strength, spring rate)
- `0xf8`: Extended commands (wheel range, LEDs, mode switching)

#### Physics Model

**State Variables:**
- `steering` (float): Current wheel position [-32768, 32767]
- `velocity` (float): Current rotation speed (units per frame)
- `ffb_force` (int16_t): Force from game [-32768, 32767]
- `user_torque` (float): Force from mouse input
- `ffb_autocenter` (int16_t): Spring force pulling to center

**Physics Loop (125 Hz):**
```cpp
// 1. Calculate total torque
total_torque = ffb_force + user_torque + autocenter_spring

// 2. Torque → Velocity (F=ma)
velocity += total_torque * 0.001f

// 3. Apply damping (friction)
velocity *= 0.98f

// 4. Velocity → Position
steering += velocity

// 5. Clamp to limits
steering = clamp(steering, -32768, 32767)
```

**Key Properties:**
- **Linear:** All forces add linearly, no exponential effects
- **Damping:** 2% velocity loss per frame (98% retention)
- **Responsive:** User can override FFB forces with mouse input
- **Realistic:** Autocenter provides gentle pull to center
- **Race-Free:** Mutex-protected state access

---

### 5. Input Mapping

#### Steering (Mouse X Axis)
```cpp
// Mouse delta → User torque
if (abs(delta) >= 2) {  // 2-pixel deadzone for jitter
    user_torque = delta * sensitivity * 20.0f
}
```

**Characteristics:**
- **Linear scaling:** Proportional to sensitivity setting
- **Deadzone:** ±2 pixels filters sensor jitter
- **Force-based:** Adds to physics simulation, not direct position
- **Balanced:** Scaled to match FFB force magnitude (20x vs old 200x)

**Example:**
- Sensitivity = 50, Mouse = 10 pixels
- `user_torque = 10 * 50 * 20 = 10,000`
- Comparable to max FFB force (32,767)

#### Pedals (W/S Keys)
```cpp
// Ramping (3% per frame at 125Hz)
throttle += (W_pressed ? 3.0f : -3.0f)
brake += (S_pressed ? 3.0f : -3.0f)
throttle = clamp(throttle, 0.0f, 100.0f)
brake = clamp(brake, 0.0f, 100.0f)

// Convert to inverted G29 range
throttle_axis = 65535 - (throttle * 655.35f)
brake_axis = 65535 - (brake * 655.35f)
```

**Characteristics:**
- **Hold-to-accelerate:** Pressing W gradually increases throttle
- **Auto-release:** Releasing key gradually decreases
- **Independent:** Can press both simultaneously
- **Inverted output:** Matches real G29 hardware

#### Buttons (Keyboard Keys)
25 buttons mapped to keys Q, E, F, G, H, R, T, Y, U, I, O, P, 1-9, 0, LShift, Space, Tab

#### D-Pad (Arrow Keys)
8-directional HAT switch: Up, Down, Left, Right, and diagonals

---

### 6. Main Loop Flow

**Initialization:**
1. Check root privileges
2. Load config from `/etc/wheel-emulator.conf`
3. Create virtual G29 device (USB Gadget → UHID → uinput)
4. Discover keyboard and mouse (config paths or auto-detect)
5. Start FFB physics thread
6. Start USB Gadget polling thread (if applicable)
7. Begin in **disabled** state (devices not grabbed)

**Main Loop (125 Hz / 8ms):**
```
while (running) {
    // 1. Read input events
    input.Read(mouse_dx)
    
    // 2. Check Ctrl+M toggle
    if (input.CheckToggle()) {
        enabled = !enabled
        input.Grab(enabled)  // Exclusive access
    }
    
    // 3. Update state (if enabled)
    if (enabled) {
        gamepad.UpdateSteering(mouse_dx, sensitivity)
        gamepad.UpdateThrottle(W_pressed)
        gamepad.UpdateBrake(S_pressed)
        gamepad.UpdateClutch(A_pressed)
        gamepad.UpdateButtons(input)
        gamepad.UpdateDPad(input)
        gamepad.SendState()  // Send HID report
    }
    
    // 4. Process FFB events
    gamepad.ProcessUHIDEvents()
    
    // 5. Sleep to maintain 125Hz
    usleep(8000)  // 8ms
}
```

**Cleanup (Ctrl+C):**
1. Stop FFB thread
2. Stop USB Gadget thread
3. Ungrab input devices
4. Destroy virtual device
5. Close file descriptors

**Responsiveness Guarantee:**
- All input reading and device I/O in the main loop must be non-blocking or handle EINTR (signal interruption), so that Ctrl+M (enable/disable) and Ctrl+C (shutdown) are always processed promptly. Blocking I/O without signal handling can cause the emulator to become unresponsive.

---

## Threading Model

### Thread 1: Main Loop (125 Hz)
- Reads keyboard/mouse input
- Updates button/pedal state
- Sends HID reports
- Processes incoming FFB commands
- **Mutex:** Locks `state_mutex` when updating `user_torque`

### Thread 2: FFB Physics (125 Hz)
- Runs physics simulation
- Calculates steering position from forces
- **Mutex:** Locks `state_mutex` when updating `steering`, `velocity`

### Thread 3: USB Gadget Polling (Event-Driven)
- Only active in USB Gadget mode
- Waits for host polls using `poll()`
- Sends INPUT reports when host requests
- Receives OUTPUT reports (FFB commands) from host
- **Mutex:** Locks `state_mutex` when reading state for HID report

**Synchronization:**
- All shared state protected by `state_mutex`
- No deadlocks (short critical sections)
- Consistent 125Hz timing across threads

---

## State Synchronization & Ungrab Guarantee

### Mutex Discipline

`state_mutex` is the single writer/read lock for every shared signal (`steering`, `velocity`, `user_torque`, pedal ramps, button bitfields, and the `enabled` latch that drives grabbing). Any thread that touches those fields must hold the mutex, which guarantees the HID report is assembled from a single coherent snapshot and that the toggle edge cannot be torn by a concurrent physics update.

### Locked SendState Paths

```cpp
void GamepadDevice::SendState() {
  if (use_uhid) {
    SendUHIDReport();
    return;
  }

  std::lock_guard<std::mutex> lock(state_mutex);
  EmitEvent(EV_ABS, ABS_X, static_cast<int16_t>(steering));
  EmitEvent(EV_ABS, ABS_Z, brake_axis);
  EmitEvent(EV_ABS, ABS_RZ, throttle_axis);
  EmitButtons(button_mask);
}

void GamepadDevice::SendUHIDReport() {
  auto payload = BuildHIDReport();  // BuildHIDReport() also locks state_mutex internally
  WriteUHID(payload);
}

std::vector<uint8_t> GamepadDevice::BuildHIDReport() {
  std::lock_guard<std::mutex> lock(state_mutex);
  return PackReport(steering, brake_axis, throttle_axis, hat_value, button_mask);
}
```

Both delivery paths (UHID/USB Gadget and uinput) now obey the same snapshot rule, so whichever host transport is active always consumes identical state bytes.

### Deterministic Toggle / Ungrab Flow

1. `CheckToggle()` detects Ctrl+M press **and** release while holding `state_mutex`, flips `enabled`, and returns the new value.
2. The main loop immediately calls `input.Grab(enabled)` while still under the same loop iteration, so `enabled == false` guarantees `EVIOCGRAB` is released before the next batch of events is read.
3. Because `SendState()` can no longer race against `FFBUpdateThread()`, the HID report writer never clobbers the `enabled` latch mid-frame, so Ctrl+M release consistently ungrabs both the keyboard and mouse.

This structural contract removes the data race entirely and couples the grab/ungrab sequence to an atomic state transition, preventing the stuck-grab failure mode observed previously.

---

## Configuration File

**Location:** `/etc/wheel-emulator.conf`

**Format:**
```ini
[devices]
keyboard=/dev/input/event6
mouse=/dev/input/event11

[sensitivity]
sensitivity=50
```

**Parameters:**
- `keyboard`: Explicit keyboard device path (optional, auto-detects if empty)
- `mouse`: Explicit mouse device path (optional, auto-detects if empty)
- `sensitivity`: Steering sensitivity 1-100 (default: 50)
  - Higher = more responsive steering
  - Multiplier: `sensitivity * 20.0f` for user torque

---

## Device Detection Mode

**Usage:** `sudo ./wheel-emulator --detect`

**Process:**
1. Opens all `/dev/input/event*` devices
2. Filters by capabilities (EV_KEY for keyboard, EV_REL for mouse)
3. **Keyboard detection (5 seconds):**
   - User types on keyboard
   - Counts key press events per device
   - Selects device with most events
4. **Mouse detection (5 seconds):**
   - User moves mouse
   - Counts REL_X movement events per device
   - Selects device with most events
5. Automatically updates `/etc/wheel-emulator.conf`
6. Ready to run normally

---

## Technical Specifications

### Device Identity
- **Vendor ID:** 0x046d (Logitech, Inc.)
- **Product ID:** 0xc24f (G29 Racing Wheel)
- **Version:** 0x0111 (273 decimal)
- **Bus Type:** BUS_USB
- **Device Name:** "Logitech G29 Driving Force Racing Wheel"

### Axes (4 total)
- **ABS_X:** Steering wheel [-32768 to 32767, center=0]
- **ABS_Y:** Clutch pedal [32767=rest, -32768=pressed] **INVERTED**
- **ABS_Z:** Brake pedal [32767=rest, -32768=pressed] **INVERTED**
- **ABS_RZ:** Throttle pedal [32767=rest, -32768=pressed] **INVERTED**

### Buttons (26 total)
- BTN_SOUTH, BTN_EAST, BTN_WEST, BTN_NORTH, BTN_TL, BTN_TR, BTN_TL2, BTN_TR2, BTN_SELECT, BTN_START, BTN_THUMBL, BTN_THUMBR, BTN_MODE, BTN_DEAD, BTN_TRIGGER_HAPPY1-12 (see table above)

### D-Pad (HAT Switch)
- ABS_HAT0X: Horizontal [-1, 0, 1]
- ABS_HAT0Y: Vertical [-1, 0, 1]
- Combined: 8 directions + neutral

### Update Rate
- **Main Loop:** 125 Hz (8ms period)
- **FFB Physics:** 125 Hz (8ms period)
- **USB Polling:** Event-driven (host-initiated)

---

## Physics Parameters

### Torque Integration
- **Coefficient:** 0.001
- **Effect:** Converts torque to velocity change
- **Tuning:** Higher = more responsive, lower = more inertia

### Damping Factor
- **Value:** 0.98 (2% loss per frame)
- **Effect:** Simulates friction, prevents runaway
- **Tuning:** Higher (0.99) = less friction, lower (0.95) = more friction

### Mouse Scaling
- **Formula:** `delta * sensitivity * 20.0f`
- **Previous:** Was 200.0f (10x too strong)
- **Fix:** Reduced to 20.0f to balance with FFB forces

### Deadzone
- **Value:** ±2 pixels
- **Purpose:** Filter mouse sensor jitter
- **Effect:** Tiny movements don't fight FFB

---

## Build & Run

### Build
```bash
make clean
make
```

### First Run (Device Detection)
```bash
sudo ./wheel-emulator --detect
# Follow prompts: type on keyboard, move mouse
# Config auto-updated
```

### Normal Run
```bash
sudo ./wheel-emulator
# Press Ctrl+M to enable
# Press Ctrl+M to disable
# Press Ctrl+C to exit
```

### Cleanup USB Gadget
```bash
./cleanup_gadget.sh
# Removes USB Gadget if stuck
```

---

**Document Version:** 2.0  
**Code Version:** Current (FFB physics improvements + race condition fix needed)  
**Author:** dewdgi  
**Repository:** https://github.com/dewdgi/wheel-hid-emulator
