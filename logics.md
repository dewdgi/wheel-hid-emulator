## Wheel HID Emulator Logic (December 2025)

The project now consists of a single, gadget-first pipeline: a keyboard+mouse input stack feeds a persistent Logitech-compatible USB gadget. Legacy UHID/uinput code paths are gone.

---

## High-Level Flow

1. **Startup**
   - `main.cpp` checks root, installs the SIGINT handler, and loads `/etc/wheel-emulator.conf`.
   - `WheelDevice::Create()` creates or reuses the `g29wheel` ConfigFS gadget, binds a UDC, opens `/dev/hidg0`, stages a neutral frame, and launches helper threads.
   - `InputManager::Initialize()` configures the `DeviceScanner`, honoring optional keyboard/mouse overrides.
2. **Main Loop**
   - `InputManager::WaitForFrame()` blocks until the scanner reports activity. Each `InputFrame` carries mouse delta X, key-derived state, and the Ctrl+M edge.
   - Loss of a grabbed device forces `WheelDevice::SetEnabled(false)` so the host only sees valid data.
   - When enabled, `WheelDevice::ProcessInputFrame()` applies steering delta and button/pedal snapshots, then wakes the gadget writer thread.
3. **Shutdown**
   - SIGINT flips `running=false`, waking all threads. The loop disables output, joins helper threads, and releases device grabs.
   - The ConfigFS gadget remains on disk (and typically stays bound) so the next run reuses it instantly. Manual cleanup is only required when switching gadget stacks.

---

## Modules & Responsibilities

### `src/main.cpp`
Ties everything together: config loading, gadget creation, InputManager lifetime, Ctrl+M toggling, and graceful shutdown.

### `src/input/device_scanner.{h,cpp}` — DeviceScanner
Low-level discovery and event ingestion for `/dev/input/event*` devices.
- Maintains device records (fd, caps, grab state, per-device key shadows).
- Auto-discovers keyboard/mouse devices unless overrides are pinned.
- Provides `WaitForEvents`, `Read(int& mouse_dx)`, `IsKeyPressed`, `Grab`, `ResyncKeyStates`, and health helpers like `AllRequiredGrabbed`.
- Runs a scanner thread that rescans `/dev/input`, hotplugs hardware, and triggers resyncs when a new keyboard appears.

### `src/input/input_manager.{h,cpp}` — InputManager
Bridges DeviceScanner to the rest of the app.
- Dedicated reader thread waits on `DeviceScanner::WaitForEvents`, drains events, detects Ctrl+M, and builds `InputFrame` snapshots (mouse delta, `WheelInputState`, timestamp, toggle flag).
- Frames are published via condition variable; consumers call `WaitForFrame` or `TryGetFrame`.
- Exposes `GrabDevices`, `AllRequiredGrabbed`, `ResyncKeyStates`, and `LatestLogicalState` for `WheelDevice` to coordinate enable/disable handshakes.

### `src/wheel_device.{h,cpp}` — WheelDevice
Owns wheel state (steering, pedals, 26 buttons, hat, FFB state, enable flag) and orchestrates HID I/O.
- `SetEnabled(true/false)` handles grabbing devices, synchronizing key state, priming neutral/snapshot reports, enabling gadget threads, and releasing hardware.
- `ProcessInputFrame` applies steering delta (scaled by sensitivity) and button/pedal snapshots when `output_enabled` is true.
- Helper threads:
  - `USBGadgetPollingThread`: sole HID writer; emits 13-byte reports whenever `state_dirty` or `warmup_frames` is set.
   - `USBGadgetOutputThread`: drains `/dev/hidg0` for 7-byte OUTPUT packets and forwards commands to the FFB pipeline.
   - `FFBUpdateThread`: ~125 Hz torque loop that blends host force, autocenter, gain, and damping, then nudges steering via `ApplySteeringLocked`.

### `src/hid/hid_device.{h,cpp}` — `hid::HidDevice`
Encapsulates ConfigFS and `/dev/hidg0`.
- Loads `libcomposite`/`dummy_hcd` (best effort), ensures `/sys/kernel/config` is mounted, and builds the Logitech G29 descriptor tree if missing.
- Handles UDC binding/unbinding, endpoint open/close, and exposes blocking report writes used by `WheelDevice`.

### `src/logging/logger.{h,cpp}`
Mutexed logging with stream-style macros (`LOG_ERROR/WARN/INFO/DEBUG`). Tags like `hid`, `input_manager`, and `wheel_device` keep traces readable.

### `src/config.{h,cpp}`
Reads `/etc/wheel-emulator.conf`, generating a documented default when absent. Keys: `[devices] keyboard/mouse`, `[sensitivity] sensitivity` (1-100), `[ffb] gain` (0.1-4.0). Values are clamped before use.

---

## Thread Model

| Thread | Entry Point | Purpose |
|--------|-------------|---------|
| Main | `main()` | Consumes `InputFrame`, toggles emulation, forwards frames to `WheelDevice`, coordinates shutdown |
| Scanner | `DeviceScanner::ScannerThreadMain()` | Periodically rescans `/dev/input`, hotplugs devices, honors overrides |
| Input Reader | `InputManager::ReaderLoop()` | Waits for events, builds logical frames, detects toggles |
| Gadget Writer | `WheelDevice::USBGadgetPollingThread()` | Sole HID IN writer (13-byte reports, warmup burst) |
| Gadget Output | `WheelDevice::USBGadgetOutputThread()` | Reads 7-byte OUTPUT packets and forwards FFB commands |
| FFB Physics | `WheelDevice::FFBUpdateThread()` | 125 Hz torque shaping and steering offset application |

`WheelDevice` owns the shared wheel state protected by `state_mutex`, `state_cv`, and `ffb_cv`. DeviceScanner keeps its own locks around device vectors and scanner flags.

---

## USB Gadget Lifecycle

1. **Creation** — `hid::HidDevice::Initialize()` ensures ConfigFS is mounted, removes incomplete gadget remnants, writes the Logitech G29 descriptor/strings/config, and links `functions/hid.usb0` into `configs/c.1`.
2. **Binding** — The detected UDC is written to `/sys/kernel/config/usb_gadget/g29wheel/UDC`. `/dev/hidg0` opens in non-blocking mode.
3. **Reuse** — Subsequent launches reuse the existing gadget directory/VIP/serial. Only `/dev/hidg0` and helper threads restart.
4. **Manual teardown** — Run:
   ```bash
   echo '' | sudo tee /sys/kernel/config/usb_gadget/g29wheel/UDC
   sudo rm -rf /sys/kernel/config/usb_gadget/g29wheel
   ```
   Use only when switching gadget stacks; the emulator intentionally leaves the gadget intact.

---

## HID Report Layout

`WheelDevice::BuildHIDReportLocked()` serializes a 13-byte payload matching Logitech's descriptor:
1. Bytes 0-1: steering (signed 16-bit centered at 32768).
2. Bytes 2-3: clutch (ABS_Y) inverted so released = 65535.
3. Bytes 4-5: throttle (ABS_Z) inverted.
4. Bytes 6-7: brake (ABS_RZ) inverted.
5. Byte 8 low nibble: hat value (0-7) or 0x0F when idle.
6. Bytes 9-12: 26 little-endian button bits (see mapping below).

`SendNeutral(reset_ffb)` zeroes axes/hat/buttons and optionally clears the FFB state so the host always receives a clean frame after toggles or shutdown.

---

## Force Feedback Pipeline

1. `USBGadgetOutputThread` reads 7-byte OUTPUT reports from `/dev/hidg0`.
2. `ParseFFBCommand` handles Logitech opcodes (constant force slots, enable/disable, autocenter strength, etc.).
3. `FFBUpdateThread` wakes via `ffb_cv`, clamps `dt` to 10 ms, shapes torque (`ShapeFFBTorque`), applies gain/autocenter, feeds the damped spring model (stiffness 120, damping 8, velocity clamp 90k/s, offset clamp ±22k), and calls `ApplySteeringLocked`.
4. If steering changed, `state_dirty` triggers the gadget writer to emit a fresh HID frame immediately.

---

## Controls & Mapping

| Control | Input | HID Field |
|---------|-------|-----------|
| Steering | Mouse X delta | ABS_X |
| Throttle | `KEY_W` | ABS_Z (inverted) |
| Brake | `KEY_S` | ABS_RZ (inverted) |
| Clutch | `KEY_A` | ABS_Y (inverted) |
| D-pad | Arrow keys | Hat switch |

Buttons map to `Q,E,F,G,H,R,T,Y,U,I,O,P,1,2,3,4,5,6,7,8,9,0,LeftShift,Space,Tab,Enter` (in that order). See README for user-friendly descriptions.

---

## Configuration & Tuning

- `[devices] keyboard/mouse`: blank for auto-detect; otherwise provide absolute `/dev/input/eventX` paths.
- `[sensitivity] sensitivity`: integer 1-100 (default 50). WheelDevice multiplies mouse delta by `sensitivity * 0.05` and clamps per-frame steps to ±2000 counts before clamping steering to ±32767.
- `[ffb] gain`: float 0.1-4.0. Both the parser and `WheelDevice::SetFFBGain` clamp it to keep the physics loop stable.

---

## Reliability Notes

- If `DeviceScanner` loses a grabbed keyboard/mouse, `InputManager` triggers an automatic disable so the host never receives partially updated frames.
- `DeviceScanner::ReleaseDeviceKeys` clears pressed keys for disappearing devices, preventing stuck buttons.
- Logging tags (`hid`, `input_manager`, `wheel_device`, etc.) make journald/console traces easy to follow.
- `lsusb | grep 046d:c24f` should show the gadget even when emulation is disabled because the device stays enumerated and streams neutral frames.

_Last reviewed: commit f6cc1df (December 2025), the DeviceScanner/InputManager refactor and WheelDevice rename._
