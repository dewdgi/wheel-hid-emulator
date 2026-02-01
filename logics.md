## Wheel HID Emulator Logic (Windows Port - February 2026)

This branch implements the **Windows Port** of the Wheel HID Emulator. Instead of creating a USB Gadget (Linux), it acts as a **feeder** for the **vJoy (Virtual Joystick)** driver.

---

## High-Level Flow

1.  **Startup**
    - `main.cpp` initializes the console, loads `wheel-emulator.conf`, and setups signal handlers.
    - `WheelDevice::Create()` initializes the vJoy interface (`hid::HidDevice`).
        - **DLL Injection:** The program extracts `vJoyInterface.dll` from its resources to a temp folder and loads it dynamically. This makes the executable portable (single file).
        - Checks if vJoy is installed and enabled (Target Device ID: 1).
        - Registers the **FFB Callback** to receive Force Feedback commands from games.
        - Acquires the vJoy device.
    - `InputManager::Initialize()` starts scanning for keyboard and mouse input.

2.  **Main Loop**
    - `InputManager` captures keyboard/mouse state (using standard Windows APIs or Raw Input via the scanner).
    - `WheelDevice::ProcessInputFrame()` calculates steering angles and button states.
    - **Output:** The `USBGadgetPollingThread` (kept for structural consistency) pushes updates to vJoy via `UpdateVJD`.

3.  **Force Feedback (FFB) Loop**
    - The vJoy driver invokes our callback (`OnFFBPacket`) whenever the game sends an FFB command.
    - We parse the packet, extract the force **Magnitude**, and update the internal physics model.
    - `FFBUpdateThread` runs at ~100Hz, calculating the final torque (Spring + Constant Force + Friction) and applying it to the steering axis.

---

## Key Modules

### `src/hid/hid_device.{h,cpp}` (vJoy Adaptation)
This module replaces the Linux USB Gadget implementation.
- **Dynamic Loading:** Uses `vjoy_loader.h` to load functions from the embedded DLL.
- **Initialization:** Calls `AcquireVJD(1)` to take control of the virtual device.
- **Reporting:** `WriteReportBlocking` translates our internal 13-byte HID report into a `JOYSTICK_POSITION_V2` structure and sends it to vJoy.
- **FFB:** Registers a global callback via `FfbRegisterGenCB` to intercept effects from the game.

### `src/wheel_device.{h,cpp}` — Core Logic
Owns the wheel state (steering, pedals, buttons) and the Physics Engine.
- **FFB Mathematics (Crucial):**
    - The physics model is **1:1 identical to the Linux version**.
    - **Input Conversion:** vJoy sends `Magnitude` as a 32-bit integer, but the raw data is actually a **16-bit signed integer**.
    - **Fix:** We cast `Magnitude & 0xFFFF` to `int16_t`. This prevents integer overflow where `-1` (65535) was interpreted as `+65535`.
    - **Direction:** Positive input from vJoy (Turn Right) is mapped to **Negative Internal Force**, matching the Linux "Negative Feedback" stability loop.

### `src/input/*` — Input System
- **DeviceScanner:** Enumerates Windows input devices.
- **InputManager:** Grabs exclusive access (where possible) or reads state to feed the emulator.
- **Toggling:** `LCTRL + M` captures/releases the mouse cursor and enables/disables the virtual wheel.

---

## Force Feedback Pipeline (Windows Specific)

1.  **Game** (e.g., Assetto Corsa) sends FFB command (e.g., "Turn Right with force 50%").
2.  **vJoy Driver** receives the command and calls our registered function.
3.  **Callback (`OnFFBPacket`):**
    - Identifies packet type (`PT_CONSTREP` for Constant Force).
    - Extracts `Magnitude` (Range: -10000 to +10000).
    - **Overflow Fix:** Casts raw value to `int16_t`.
    - **Scaling:** Converts vJoy range (10000) to internal physics range (6096).
    - **Inversion:** `InternalForce = -RawForce` (Essential for stability).
4.  **Physics Loop (`FFBUpdateThread`):**
    - Applies the calculated force to the steering axis.
    - Simulates inertia, friction, and return-to-center springs.
    - Updates the virtual "Mouse" position to resist the user's hand.

---

## Configuration

- **`wheel-emulator.conf`**:
    - `[sensitivity]`: Controls steering sensitivity (1-100).
    - `[ffb]`: Global gain multiplier for force feedback effects.

---

## Comparison with Linux Version

| Feature | Linux (Original) | Windows (Port) |
| :--- | :--- | :--- |
| **Backend** | ConfigFS USB Gadget (`/dev/hidg0`) | vJoy Driver (`vJoyInterface.dll`) |
| **FFB Source** | Raw USB HID Packets | vJoy `FFB_DATA` Structures |
| **Force Logic** | `(Byte - 128) * -48` | `(int16_t)Magnitude * -0.6` |
| **Physics** | **Identical** | **Identical** |
| **Build** | `make` (GCC) | `build_with_g++.bat` (MinGW) |
| **Distribution** | Binary + Config | **Single Executable** (DLL Embedded) |

This port ensures that the **driving feel** remains consistent across platforms by strictly adhering to the original mathematical models while adapting the I/O layer.
