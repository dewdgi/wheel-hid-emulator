---


# Wheel HID Emulator: Up-to-date Logical Structure (as of November 2025)

## Main Components

### src/main.cpp
- **Global:** `std::atomic<bool> running` (controls shutdown for all threads)
- **Functions:**
  - `signal_handler(int)` — sets `running = false` on SIGINT (Ctrl+C)
  - `check_root()` — ensures root privileges
  - `main()`
    - Checks root, sets up signal handler
    - Loads config
    - Creates `GamepadDevice` (tries USB Gadget, then UHID, then uinput)
    - Seeds `Input` with optional manual overrides from config
    - **Main loop (event-driven):**
      - While `running`:
        - `input.WaitForEvents(8)` multiplexes all tracked devices (hotplug aware)
        - `input.Read(mouse_dx)` — drains every available device, updates key aggregator and mouse delta
        - `input.CheckToggle()` — edge-detects Ctrl+M, toggles enabled state
        - If enabled:
          - `gamepad.UpdateSteering(mouse_dx, config.sensitivity)`
          - `gamepad.UpdateThrottle(input.IsKeyPressed(KEY_W))`
          - `gamepad.UpdateBrake(input.IsKeyPressed(KEY_S))`
          - `gamepad.UpdateClutch(input.IsKeyPressed(KEY_A))`
          - `gamepad.UpdateButtons(input)`
          - `gamepad.UpdateDPad(input)`
          - `gamepad.SendState()`
        - `gamepad.ProcessUHIDEvents()`
    - On exit: `input.Grab(false)`

### src/gamepad.h / src/gamepad.cpp
- **Class:** `GamepadDevice`
  - **State:**
    - `fd` (device file descriptor)
    - `use_uhid`, `use_gadget` (mode flags)
    - `gadget_thread`, `gadget_running` (USB Gadget polling thread)
    - `ffb_thread`, `ffb_running` (FFB physics thread)
    - `state_mutex` (protects all state)
    - `enabled`, `steering`, `throttle`, `brake`, `clutch`, `buttons`, `dpad_x`, `dpad_y`, `ffb_force`, `ffb_autocenter`, `ffb_enabled`, `user_torque`
  - **Methods:**
    - `Create()`, `CreateUSBGadget()`, `CreateUHID()`, `CreateUInput()` — device creation
    - `UpdateSteering(int, int)`, `UpdateThrottle(bool)`, `UpdateBrake(bool)`, `UpdateClutch(bool)`, `UpdateButtons(const Input&)`, `UpdateDPad(const Input&)`
    - `SendState()`, `SendUHIDReport()`, `BuildHIDReport()`, `SendNeutral()`
    - `ProcessUHIDEvents()` — handles FFB and state requests
    - `ParseFFBCommand(const uint8_t*, size_t)` — parses FFB commands
    - `FFBUpdateThread()` — physics simulation, runs while `ffb_running && running`
    - `USBGadgetPollingThread()` — USB comms, runs while `gadget_running && running`
    - `SetEnabled(bool, Input&)`, `IsEnabled()`, `ToggleEnabled(Input&)` — enable/disable logic, grabs/ungrabs devices
    - `EmitEvent(...)`, `ClampSteering(...)`

### src/input.h / src/input.cpp
- **Class:** `Input`
  - **State:**
    - `devices` (vector of all open `/dev/input/event*` descriptors with capabilities, per-device key shadows, manual flag)
    - `keyboard_override`, `mouse_override` (optional config pins)
    - `keys[KEY_MAX]` (aggregated key state) and `key_counts[KEY_MAX]` (per-key active device count)
    - `prev_toggle` (for Ctrl+M edge detection)
  - **Methods:**
    - `DiscoverKeyboard(const std::string&)`, `DiscoverMouse(const std::string&)` — store overrides and trigger refresh
    - `RefreshDevices()` — rescans `/dev/input` and hotplugs active devices (auto + manual)
      - When both keyboard and mouse overrides are supplied, auto-detected devices are immediately closed and skipped so only the pinned descriptors remain
    - `WaitForEvents(int timeout_ms)` — polls all tracked descriptors (returns early on activity)
    - `Read(int&)` — drains device queues, updates aggregated key/mouse state, drops disconnected devices
    - `CheckToggle()` — returns true on Ctrl+M press edge
    - `Grab(bool)` — grabs/ungrabs every tracked keyboard/mouse-capable fd via `EVIOCGRAB`
    - `IsKeyPressed(int) const` — returns aggregated key state

### src/config.h / src/config.cpp
- **Class:** `Config`
  - **State:**
    - `sensitivity`, `keyboard_device`, `mouse_device`, `button_map`
  - **Methods:**
    - `Load()`, `LoadFromFile(const char*)`, `SaveDefault(const char*)`, `ParseINI(const std::string&)`

### Threading Model
- **Main thread:** runs main loop, handles input, state update, and report sending
- **FFB Physics thread:** runs `FFBUpdateThread()` in `GamepadDevice` (125Hz)
- **USB Gadget polling thread:** runs `USBGadgetPollingThread()` if in USB Gadget mode
- **Mutex:** `state_mutex` in `GamepadDevice` protects all shared state

### Device Grabbing and Shutdown
- `input.Grab(bool)` is called on enable/disable and at shutdown
- All device I/O is non-blocking or signal-interruptible
- All threads check both their own running flag and global `running`

### Control Flow Summary
- Main loop: checks for Ctrl+M (toggle), Ctrl+C (shutdown), and updates state at 100Hz
- All input and output is non-blocking, and all state is protected by mutexes where needed

---

**This document is now fully synchronized with the actual codebase as of November 2025.**

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
  - `Config::Load()`
  - `GamepadDevice::Create()`
  - `Input::DiscoverKeyboard()`
  - `Input::DiscoverMouse()`
  - `Input::WaitForEvents()`
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
- `Input::RefreshDevices()`
- `Input::WaitForEvents()`
- `Input::Read()`
- `Input::CheckToggle()`
- `Input::Grab()`
- `Input::IsKeyPressed()`

### `src/config.cpp`/`.h`
- `Config::Load()`
- `Config::SaveDefault()`
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
- Live keyboard/mouse hotplug detection (auto-discovery, no CLI wizard)

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
│  │ 1. Shape/log filter ffb_force (kills chatter)          │ │
│  │ 2. Add autocenter_spring from driver                   │ │
│  │ 3. Scale by `[ffb] gain`                               │ │
│  │ 4. Feed critically damped 2nd-order (offset/velocity)  │ │
│  │ 5. Apply offset to steering & clamp to wheel limits    │ │
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
- Continuously rescans `/dev/input` (500ms cadence) for new `event*` files
- Opens any device advertising keyboard (`EV_KEY` with alpha keys) and/or mouse (`REL_X`) capability
- Accepts manual overrides from config (kept open even if auto-scan would skip them)
- Hotplug-aware: newly attached keyboards/mice become usable as soon as they emit events; removed devices are dropped automatically

**Key State Tracking:**
- Maintains aggregated boolean array `keys[KEY_MAX]` plus per-key device reference counts
- Each device stores its own `key_shadow` vector so unplugging clears any pressed keys (prevents stuck inputs)
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

#### Method 3: UInput (Fallback)
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
- `ffb_force` (int16_t): Raw constant-force magnitude from the game
- `ffb_autocenter` (int16_t): Spring coefficient requested by driver
- `ffb_offset` (float): Current steering deflection contributed by FFB
- `ffb_velocity` (float): Rate-of-change term for the second-order response
- `ffb_gain` (float): User-configurable multiplier loaded from `[ffb] gain` in the config
- `steering`, `user_steering`: Combined with `ffb_offset` inside `ApplySteeringLocked()`

**Physics Loop (125 Hz):**
```cpp
// 1. Shape game torque for road feel (nonlinear gain curve)
commanded = ShapeFFBTorque(ffb_force)

// 2. Low-pass filter to kill chatter (one-pole @ force_filter_hz)
filtered += (commanded - filtered) * alpha

// 3. Add autocenter spring and apply user gain
spring = -(steering * ffb_autocenter) / 32768.0f
target = (filtered + spring) * ffb_gain

// 4. Critically damped 2nd-order response
error = target - ffb_offset
ffb_velocity += error * stiffness * dt
ffb_velocity *= exp(-damping * dt)
ffb_velocity = clamp(ffb_velocity, ±max_velocity)
ffb_offset += ffb_velocity * dt (clamped to ±offset_limit)

// 5. Apply combined steering
ApplySteeringLocked(user_steering + ffb_offset)
```

**Key Properties:**
- **Filtered:** High-frequency impulses from some games no longer cause audible wheel chatter
- **Gain knob:** `[ffb] gain` in `/etc/wheel-emulator.conf` scales the final torque (0.1–4.0)
- **Second-order smoothing:** Prevents oscillations while still tracking large forces quickly
- **Spring-aware:** Autocenter from games stacks cleanly with user override and manual gain
- **Race-Free:** Still guarded by `state_mutex`

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
4. Seed `Input` overrides (optional `keyboard=` / `mouse=`); hotplug scanning begins immediately
5. Start FFB physics thread
6. Start USB Gadget polling thread (if applicable)
7. Begin in **disabled** state (devices not grabbed)

**Main Loop (125 Hz / 8ms):**
```
while (running) {
  // 1. Wait for any device activity (8 ms max)
  input.WaitForEvents(8);

  // 2. Drain input events
  input.Read(mouse_dx);
    
  // 3. Check Ctrl+M toggle
    if (input.CheckToggle()) {
        enabled = !enabled
        input.Grab(enabled)  // Exclusive access
    }
    
  // 4. Update state (if enabled)
    if (enabled) {
        gamepad.UpdateSteering(mouse_dx, sensitivity)
        gamepad.UpdateThrottle(W_pressed)
        gamepad.UpdateBrake(S_pressed)
        gamepad.UpdateClutch(A_pressed)
        gamepad.UpdateButtons(input)
        gamepad.UpdateDPad(input)
        gamepad.SendState()  // Send HID report
    }
    
    // 5. Process FFB events
    gamepad.ProcessUHIDEvents()
    
    // 6. Loop immediately; `WaitForEvents` bounds the cadence
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

**As of Nov 2025:**
- The `ToggleEnabled` method in `GamepadDevice` now toggles the `enabled` state under the mutex, but calls `input.Grab()` outside the lock. This prevents deadlocks if `input.Grab()` blocks or takes time, ensuring the main loop remains responsive to Ctrl+M and Ctrl+C.

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
# keyboard=/dev/input/event6
# mouse=/dev/input/event11
keyboard=
mouse=

[sensitivity]
sensitivity=50

[ffb]
gain=0.1
```

**Parameters:**
- `keyboard` / `mouse`: Leave blank for auto (hotplug) or uncomment to pin specific `eventX`
- `sensitivity`: Steering sensitivity 1-100 (default: 50) → multiplies mouse delta before feeding user torque
- `gain`: Overall FFB strength multiplier (0.1–4.0, default 0.1) applied after filtering/autocenter

---

## Runtime Device Tracking

- Every 500 ms the input layer scans `/dev/input` for `event*` nodes.
- Devices stay open as long as they advertise the needed capabilities (`EV_KEY` with alphanumeric keys and/or `REL_X`).
- Manual overrides from the config are reopened on demand and marked as `manual`, so they are never dropped by the auto-pruner.
- All open descriptors participate in a single `poll()` (`WaitForEvents`) and are drained each frame.
- When a device disappears (hot-unplug, suspend, etc.), its pressed keys are released automatically thanks to the per-device `key_shadow` + `key_counts` bookkeeping.
- Connecting a new keyboard or mouse mid-session requires no restart; the next scan will pick it up and steering/keys start working as soon as events arrive.

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

### Force Filter
- **Cutoff:** ~38 Hz (single-pole low-pass)
- **Effect:** Smooths harsh constant-force packets and prevents audible chatter

### Second-Order Response
- **Stiffness:** 120.0f, **Damping:** 8.0f
- **Behavior:** Critically damped; enforces ±22,000 offset limit and ±90,000 units/s velocity

### FFB Gain
- **Range:** 0.1 – 4.0 (configurable via `[ffb] gain`)
- **Default:** 0.1 to keep stock feel gentle; raise if your wheel can handle stronger torque

### Mouse Scaling
- **Formula:** `delta * sensitivity * 20.0f`
- **Deadzone:** ±2 pixels to avoid sensor jitter in neutral steering

---

### Build & Run

```bash
make clean
make

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
