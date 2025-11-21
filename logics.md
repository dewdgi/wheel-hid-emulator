## Wheel HID Emulator Logic (November 2025)

This document matches the current gadget-only implementation. Every subsystem described below exists in the repository today, and any outdated UHID/uinput paths referenced in older notes have been removed from both code and documentation.

---

## Runtime Stages

1. **Startup**
   - `main.cpp` ensures root, installs SIGINT handler, loads `/etc/wheel-emulator.conf`.
   - `GamepadDevice::Create()` provisions the ConfigFS gadget, opens `/dev/hidg0`, and starts the three helper threads (FFB physics, gadget writer, gadget OUTPUT).
   - `Input` scans `/dev/input/event*`, honoring optional overrides from the config.
2. **Enabled Loop**
   - Main loop waits up to 8 ms for input, drains every keyboard/mouse fd, and checks Ctrl+M.
   - When enabled, the loop updates steering, pedals, buttons, and D-pad, then calls `SendState()` to wake the gadget writer thread.
   - The gadget writer serializes a 13-byte HID report that matches the Logitech G29 descriptor and writes it to `/dev/hidg0` until the host accepts it.
   - The gadget OUTPUT thread parses 7-byte OUTPUT reports for FFB, waking the physics loop as soon as torque data arrives.
3. **Shutdown**
   - Ctrl+C flips the global `running` flag, unblocks every condition variable, and joins all threads.
   - `GamepadDevice::DestroyUSBGadget()` unbinds the UDC and removes the ConfigFS tree so no stale devices remain.

---

## Modules & Responsibilities

### `src/main.cpp`
- Owns the lifecycle: config load, gadget creation, hotplug discovery, and the control loop.
- Maintains `running` and the `Ctrl+M` toggle state (via `Input::CheckToggle`).
- Computes frame delta for steering smoothing, clamps `dt` to 50 ms to prevent physics spikes, and enforces graceful shutdown.

### `src/input.cpp`
- Tracks every relevant `/dev/input/event*` device with capabilities, per-device key shadows, and grab state.
- Auto-detects keyboard/mouse devices unless both overrides are specified, in which case only the pinned fds stay open.
- Implements `WaitForEvents(timeout_ms)` via `poll`, so the main loop sleeps until activity or timeout.
- Aggregates key presses into `keys[KEY_MAX]`, accumulates mouse X delta, and exposes `IsKeyPressed(keycode)` lookups.
- `Grab(bool)` issues `EVIOCGRAB` for exclusive access while the emulator is enabled, but key state continues to be tracked even while ungrabbed so toggling never discards the user’s current pedal position.
- `ResyncKeyStates()` only does real work when `resync_pending` is true (i.e., a new keyboard was discovered). `GamepadDevice::SetEnabled(true)` still calls it so pending resyncs happen immediately, but steady-state toggles skip the expensive EVIOCGKEY sweep entirely.

### `src/gamepad.cpp`
- Holds the canonical wheel state behind `state_mutex`: steering, user steering, FFB offset/velocity, three pedals, D-pad axes, 26 button bits, enable flag, and FFB parameters.
- `CreateUSBGadget()` performs the full ConfigFS ritual (IDs, strings, hid.usb0 function, configs/c.1 link, and UDC bind) and opens `/dev/hidg0` non-blocking.
- `USBGadgetPollingThread()` is the only writer to `/dev/hidg0`. It waits on `state_cv`, builds a 13-byte report with `BuildHIDReport()`, then calls `WriteHIDBlocking()` until the transfer completes. A short "warmup" burst (25 frames) is pushed any time we re-enable so games that are sitting in input menus still see fresh data even if nothing is moving yet.
- `USBGadgetOutputThread()` polls for OUTPUT data, reassembles 7-byte packets, and hands them to `ParseFFBCommand()` for decoding.
- `FFBUpdateThread()` runs at ~125 Hz: shapes torque, blends autocenter springs, applies gain, feeds the damped spring model, and nudges steering by applying `ffb_offset` before clamping to ±32768.
- `ToggleEnabled()` flips the `enabled` flag under the mutex, grabs/ungrabs input, calls `ResyncKeyStates()` (which is a no-op unless a new device arrived), reapplies whatever pedals/buttons are currently held (snapshot), schedules the warmup burst, and logs the new mode so the host always sees a current frame when control changes.
- `SendNeutral(reset_ffb)` injects a neutral frame and, unless we explicitly ask for a reset, preserves the force-feedback state so quickly toggling Ctrl+M no longer clears road feel.
- Warmup burst: when we re-enable, `GamepadDevice` emits a few consecutive neutral frames followed by the freshly captured snapshot so ACC never sees half-pressed pedals or missing axes after a toggle.

### `src/config.cpp`
- Reads `/etc/wheel-emulator.conf`. If missing, writes a documented default that matches the code (including the KEY_ENTER button 26 binding and clutch axis description).
- Recognized keys: `[devices] keyboard/mouse`, `[sensitivity] sensitivity` (1–100), `[ffb] gain` (0.1–4.0). Optional `[button_mapping]` entries are informational only.

---

## USB Gadget Flow

1. `CreateUSBGadget()` loads `libcomposite`/`dummy_hcd` (best-effort) and ensures ConfigFS is mounted.
2. Any previous gadget with the same name is removed to avoid collisions.
3. The Logitech G29 descriptor (26 buttons, hat, four 16-bit axes, 7-byte OUTPUT report) is written to `functions/hid.usb0/report_desc` and `report_length` is set to 13.
4. The hid function is linked into `configs/c.1`, string tables are populated, and the first available UDC is bound.
5. `/dev/hidg0` opens in non-blocking read/write mode. All IN traffic goes through `WriteHIDBlocking`; all OUT traffic is drained in `ReadGadgetOutput` to keep the kernel queue empty.

Because UHID/uinput is gone, failure to create the gadget is fatal and surfaces clear instructions about ConfigFS/UDC requirements.

---

## Thread Model (All reference `running` and per-thread atomics)

| Thread | Entry Point | Purpose | Notes |
|--------|-------------|---------|-------|
| Main | `main()` loop | Poll input, update state, request report send | Calls into `Input` & `GamepadDevice` synchronously |
| Gadget Writer | `USBGadgetPollingThread()` | Sole HID IN writer | Waits on `state_cv`, honors `state_dirty` flag |
| Gadget OUTPUT | `USBGadgetOutputThread()` | Reads 7-byte OUTPUT frames | Feeds `ParseFFBCommand`, tolerates partial reads |
| FFB Physics | `FFBUpdateThread()` | Shapes torque, updates offsets | 125 Hz loop with damping/+gain math |

Synchronization is limited to `state_mutex` (covers everything serialized into HID reports and the enable flag) plus `state_cv`/`ffb_cv` for thread wakeups. No other locks exist, so there are no ordering surprises.

---

## HID State Layout

`BuildHIDReport()` produces the exact 13-byte payload expected by the kernel `hid-lg` driver:

1. Bytes 0–1: ABS_X steering (signed 16-bit shifted into unsigned space).
2. Bytes 2–3: ABS_Y clutch (KEY_A). Value is `65535 - clutch_pct * 655.35` so it rests at 65535 like the real G29.
3. Bytes 4–5: ABS_Z throttle (KEY_W) inverted the same way.
4. Bytes 6–7: ABS_RZ brake (KEY_S) inverted.
5. Byte 8 (low nibble): HAT encoded from arrow keys; high nibble padded.
6. Bytes 9–12: 26 button bits packed little-endian. Button order matches Logitech’s enumeration and the comments in `config.cpp` / README.

`SendNeutral()` zeros the analog fields, hats, and buttons before notifying the gadget thread, ensuring graceful enable/disable and shutdown flows.

---

## Force Feedback Pipeline

1. Host driver sends 7-byte OUTPUT packets (constant force, stop, autocenter, etc.).
2. `ParseFFBCommand()` decodes opcodes (0x11, 0x13, 0x14, 0xf5, 0xfe, 0xf8…) and updates `ffb_force` or `ffb_autocenter` under the mutex.
3. `ffb_cv.notify_all()` wakes the physics loop immediately after a state change.
4. Physics loop runs at ~125 Hz:
   - Shapes the raw force via `ShapeFFBTorque()` (softens low amplitudes, prevents saturation).
   - Blends in autocenter springs.
   - Applies config gain and a critically damped spring-mass model (stiffness 120, damping 8, offset clamp ±22 k).
   - Calls `ApplySteeringLocked()` to merge user input (mouse) and FFB offset, clamping to the ±32768 HID range.
5. If steering changed, `state_dirty` flips true so the gadget writer pushes a new frame instantly.

---

## Controls & Mapping

| Control | Source | HID Field |
|---------|--------|-----------|
| Steering | Mouse X delta | ABS_X |
| Throttle | `KEY_W` | ABS_Z (inverted) |
| Brake | `KEY_S` | ABS_RZ (inverted) |
| Clutch | `KEY_A` | ABS_Y (inverted) |
| D-Pad | Arrow keys | ABS_HAT0X/ABS_HAT0Y |
| Buttons | `Q,E,F,G,H,R,T,Y,U,I,O,P,1,2,3,4,5,6,7,8,9,0,LeftShift,Space,Tab,Enter` | 26 button bits |

All bindings are hardcoded in `GamepadDevice::UpdateButtons`. The README table mirrors these keys, including the new Enter binding for button 26.

---

## Configuration Impact

- `sensitivity` (1–100, default 50) scales mouse deltas by `sensitivity * 0.05` and clamps per-frame contributions to ±2000 counts so a runaway mouse cannot saturate the axis in one tick.
- `gain` (0.1–4.0, default 0.3) multiplies the torque target within the FFB loop; clamped inside `SetFFBGain()` to protect stability.
- Device overrides allow deterministic keyboard/mouse selection; leaving either empty keeps hotplug discovery active for that class.

`SaveDefault()` now describes the clutch axis, inverted pedals, and all 26 buttons (including KEY_ENTER). This keeps `/etc/wheel-emulator.conf` aligned with the executable without requiring the user to inspect code.

---

## Lifecycle Guarantees

- **Enable/Disable:** Ctrl+M grabs/ungrabs devices via `Input::Grab`, keeps the aggregated key state alive even while ungrabbed, only resyncs against hardware when `resync_pending` is set (new device or manual override change), reapplies those held controls to the HID state, runs a short warmup burst, and logs the new mode. This keeps modifiers responsive even if you hold Ctrl while toggling, avoids redundant ioctl spam during rapid toggles (100 Hz+ is fine), and guarantees games in input menus see fresh pedal data. Grabbing occurs outside the state mutex to avoid deadlocks if `EVIOCGRAB` blocks.
- **Signal Safety:** All blocking syscalls in threads treat `EINTR` as retryable. The SIGINT handler only toggles `running` and writes a message, so shutdown is safe even if the gadget threads are mid-transfer.
- **Hotplug Safety:** Each device’s `key_shadow` is flushed when the fd disconnects, releasing any held buttons so games never see stuck inputs after a keyboard unplug.

---

## Troubleshooting Hooks

- **ConfigFS cleanup:** rerunning the emulator always tears down/rebuilds the gadget tree (`DestroyUSBGadget()` mirrors the setup routine). If a crash leaves artifacts behind, manually echo an empty string into `/sys/kernel/config/usb_gadget/g29wheel/UDC` and remove the directories just like the code’s cleanup routine.
- `GamepadDevice::Create()` prints detailed guidance (ConfigFS mount, libcomposite, dummy_hcd, UDC availability) for the only supported backend: USB gadget.
- `lsusb` should show `046d:c24f` whenever the gadget is enabled. If not, repeat the ConfigFS cleanup sequence above or ensure a hardware/virtual UDC is loaded.

---

**Last verified:** November 2025 — matches commit where UHID/uinput fallbacks were deleted and KEY_ENTER became button 26. Keep this document in sync whenever bindings, threads, or gadget prerequisites change.
