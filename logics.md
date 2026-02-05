## Wheel HID Emulator — Architecture & Logic

Windows-only steering wheel emulator using vJoy. Turns keyboard + mouse into a virtual Logitech G29.

---

## Source Tree

```
src/
├── main.cpp                    — Entry point, console setup, signal handlers
├── config.{h,cpp}              — INI parser for wheel-emulator.conf
├── input_defs.h                — Key code definitions (VK → Linux keycode mapping)
├── wheel_types.h               — Shared type definitions (WheelState, InputFrame)
├── wheel_device.{h,cpp}        — Core wheel logic, FFB physics, vJoy report submission
├── hid/
│   ├── hid_device.{h,cpp}      — vJoy device lifecycle (acquire, release, FFB callback)
│   ├── vjoy_loader.{h,cpp}     — Dynamic loading of embedded vJoyInterface.dll
│   └── vjoy_dll.rc             — Resource script embedding the DLL into the EXE
├── input/
│   ├── device_scanner.{h,cpp}  — Raw Input API: keyboard/mouse capture, Ctrl+M toggle
│   ├── input_manager.{h,cpp}   — Aggregates input frames, bridges scanner → wheel_device
│   └── wheel_input.h           — Input event structures
├── logging/
│   └── logger.{h,cpp}          — Console logging
└── vjoy_sdk/inc/               — vJoy SDK headers (public.h, vjoyinterface.h)
```

---

## Threading Model (3 threads)

| Thread | Function | Rate | Purpose |
| :--- | :--- | :--- | :--- |
| **Reader Loop** | `DeviceScanner::ReaderLoop()` | Event-driven | Windows Raw Input message pump. Captures keyboard/mouse via hidden HWND. |
| **vJoy Polling** | `WheelDevice::VJoyPollingThread()` | ~60 Hz | Sends `JOYSTICK_POSITION_V2` reports to vJoy via `UpdateVJD()`. |
| **FFB Update** | `WheelDevice::FFBUpdateThread()` | ~1 kHz | Physics simulation: spring, friction, constant force → steering axis resistance. |

The FFB callback (`OnFFBPacket`) runs on the vJoy driver's thread — it only writes `ffb_force` atomically, consumed by the FFB Update thread.

---

## Startup Sequence

1. `main.cpp` initializes console, loads `wheel-emulator.conf`, sets up Ctrl+C handler.
2. `WheelDevice::Create()` → `hid::HidDevice::Create()`:
   - `vjoy_loader` extracts `vJoyInterface.dll` from EXE resources to `%TEMP%` and `LoadLibrary()`s it.
   - Checks vJoy is enabled, Device 1 exists with correct config.
   - Calls `AcquireVJD(1)`, registers FFB callback via `FfbRegisterGenCB`.
3. `InputManager::Initialize()` → `DeviceScanner` registers for Raw Input (keyboard + mouse) via a message-only window.
4. Main loop: `InputManager::WaitForFrame()` → `WheelDevice::ProcessInputFrame()` → state update → `VJoyPollingThread` sends report.

---

## Key Modules

### `wheel_device.{h,cpp}` — Core Logic
Owns the wheel state (steering angle, pedals, buttons) and the FFB physics engine.

- **`ProcessInputFrame()`** — Converts mouse delta → steering angle, key states → pedals/buttons.
- **`VJoyPollingThread()`** — Wakes on state change, calls `SendReport()` → `hid_device.SetAxes()`/`SetButtons()` → `UpdateVJD()`.
- **`FFBUpdateThread()`** — ~1kHz physics loop: reads `ffb_force`, computes spring + constant + friction torque, applies to steering axis.
- **`OnFFBPacket()`** — Static callback invoked by vJoy driver. Parses `FFB_DATA`, extracts Magnitude with `int16_t` cast to prevent overflow, scales and inverts force.

**FFB Overflow Fix (Critical):**
vJoy sends `Magnitude` as a 32-bit int, but the raw data is a 16-bit signed value. We cast `Magnitude & 0xFFFF` to `int16_t`. Without this, `-1` (0xFFFF = 65535 unsigned) was interpreted as `+65535`, causing violent wheel snap.

### `hid/hid_device.{h,cpp}` — vJoy Interface
- **`Create()`** — Acquires vJoy Device 1, validates axis/button configuration.
- **`SetAxes()`/`SetButtons()`** — Populates `JOYSTICK_POSITION_V2`, calls `UpdateVJD()`.
- **`RegisterFFBCallback()`** — Wraps `FfbRegisterGenCB()`.
- **`Release()`** — Calls `RelinquishVJD()`.

### `hid/vjoy_loader.{h,cpp}` — DLL Extraction & Loading
- Extracts `vJoyInterface.dll` from the EXE's embedded resource (`vjoy_dll.rc`) to `%TEMP%`.
- Uses `LoadLibrary` + `GetProcAddress` to resolve all vJoy API functions at runtime.
- Makes the EXE fully portable — no vJoy SDK DLLs needed alongside it.

### `input/device_scanner.{h,cpp}` — Raw Input Capture
- Creates a hidden message-only `HWND` and registers for `RAWINPUT` (keyboard + mouse).
- **`ReaderLoop()`** — `MsgWaitForMultipleObjects` pump. Translates `VK_*` codes → Linux keycodes via lookup table.
- **`Ctrl+M toggle`** — Captures/releases mouse cursor, hides/shows cursor, enables/disables emulation.
- **Cursor lock** — `ClipCursor()` re-applied every frame to prevent escape on focus loss.

### `input/input_manager.{h,cpp}` — Frame Aggregation
- Bridges `DeviceScanner` → `WheelDevice`.
- `WaitForFrame()` blocks until input arrives, returns accumulated `InputFrame` (mouse deltas + key states).

### `config.{h,cpp}` — Configuration
- Parses `wheel-emulator.conf` INI: `[sensitivity]` and `[ffb]` sections.
- `SaveDefault()` generates a documented default config file.

---

## Force Feedback Pipeline

```
Game (e.g. Assetto Corsa)
  → vJoy Driver
    → OnFFBPacket() callback [vJoy thread]
      → Parse PT_CONSTREP, extract Magnitude
      → Cast: int16_t(Magnitude & 0xFFFF)     [overflow fix]
      → Scale: vJoy range (10000) → internal (6096)
      → Invert: force = -raw                   [stability]
      → Store: ffb_force (atomic)
    → FFBUpdateThread() [~1kHz]
      → Read ffb_force
      → Compute: spring + constant + friction torque
      → Apply to steering axis (resist mouse movement)
```

---

## Configuration

`wheel-emulator.conf` (auto-generated with defaults if missing):

```ini
[sensitivity]
sensitivity=50    # 1-100. Higher = faster steering.

[ffb]
gain=1.0          # 0.1-4.0. Force Feedback strength multiplier.
```

---

## Build

**Requirements:** MinGW-w64 (g++) on PATH.

```
build_with_g++.bat
```

Produces `wheel-emulator.exe` (~1.6 MB). The vJoy DLL is embedded — no external DLLs needed.
