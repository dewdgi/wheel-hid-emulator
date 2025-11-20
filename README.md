# Wheel HID Emulator

Transform a keyboard and mouse into a force-feedback Logitech G29 Racing Wheel on Linux. The emulator exposes a **real USB HID gadget** (VID `046d`, PID `c24f`) so games, Wine/Proton, and consoles see an authentic wheel.

## Highlights

- Native USB gadget device on `/dev/hidg0` that enumerates exactly like a Logitech G29.
- Force feedback physics loop (125 Hz) with Logitech-compatible OUTPUT report parsing.
- Mouse X → steering, keyboard keys → pedals, clutch, 26 buttons, and D-pad.
- Hotplug-aware input stack with Ctrl+M grab/ungrab toggle and Ctrl+C shutdown.
- Configurable steering sensitivity and FFB gain via `/etc/wheel-emulator.conf`.

## Requirements

| Requirement | Details |
|-------------|---------|
| Kernel      | `CONFIG_USB_CONFIGFS=y`, `libcomposite`, and either a hardware UDC or `dummy_hcd` for self-hosted testing |
| Privileges  | Root (needed for ConfigFS operations, `/dev/hidg0`, and grabbing input devices) |
| Toolchain   | g++ with C++17 support |

## Build & Quick Start

```bash
make

# Optional: install system-wide (installs binary under /usr/local/bin)
sudo make install

# Load the gadget drivers (no-op if already present)
sudo modprobe libcomposite
sudo modprobe dummy_hcd   # only needed for USB gadget testing on the same PC

# Run the emulator
sudo ./wheel-emulator
```

Runtime controls:
- **Ctrl+M** – toggle emulation on/off (grabs or releases keyboard/mouse)
- **Ctrl+C** – exit cleanly, tearing down the ConfigFS gadget

## Controls & Mapping

### Axes

| Action  | Input Source | HID Axis | Notes |
|---------|--------------|----------|-------|
| Steering | Mouse horizontal delta | ABS_X | Accumulates into ±32767, scaled by `sensitivity*0.05` |
| Throttle | `W` key | ABS_Z | 0 % (released) to 100 % (held). Sent inverted (32767=rest) to match the real G29 |
| Brake | `S` key | ABS_RZ | Same ramp/inversion as throttle |
| Clutch | `A` key | ABS_Y | Same ramp/inversion as throttle |
| D-Pad | Arrow keys | ABS_HAT0X/Y | 8-way hat synthesized from KEY_UP/DOWN/LEFT/RIGHT |

### Buttons (26 total)

| Logitech Button | Keyboard Key | Typical Use (suggestion) |
|-----------------|--------------|--------------------------|
| South (Cross)         | Q | Downshift |
| East (Circle)         | E | Upshift |
| West (Square)         | F | Flash/high beams |
| North (Triangle)      | G | Horn |
| L1                    | H | Lights toggle |
| R1                    | R | Camera right |
| L2                    | T | Telemetry |
| R2                    | Y | HUD cycle |
| Select (Share)        | U | Pit limiter |
| Start (Options)       | I | Ignition |
| L3                    | O | Wipers |
| R3                    | P | Pause |
| PS / Mode             | 1 | TC Down |
| Dead                  | 2 | TC Up |
| Trigger Happy 1       | 3 | ABS Down |
| Trigger Happy 2       | 4 | ABS Up |
| Trigger Happy 3       | 5 | Brake bias fwd |
| Trigger Happy 4       | 6 | Brake bias back |
| Trigger Happy 5       | 7 | Engine map - |
| Trigger Happy 6       | 8 | Engine map + |
| Trigger Happy 7       | 9 | Pit request |
| Trigger Happy 8       | 0 | Leaderboard |
| Trigger Happy 9       | Left Shift | Camera left |
| Trigger Happy 10      | Space | Handbrake |
| Trigger Happy 11      | Tab | Cycle view |
| Trigger Happy 12      | Enter | Extra bind |

Remap these inside your game just like a real Logitech wheel.

## Configuration (`/etc/wheel-emulator.conf`)

```ini
[devices]
# Leave blank for hotplug auto-detect. Set both fields to pin exact event nodes.
keyboard=
mouse=

[sensitivity]
sensitivity=50   # 1–100, multiplied by 0.05 internally

[ffb]
gain=0.3         # 0.1–4.0 force multiplier
```

- **Device overrides** – set both `keyboard` and `mouse` to `/dev/input/eventX` paths if you want to prevent hotplug discovery from grabbing other peripherals.
- **Sensitivity** – caps each mouse sample to ±2000 counts and clamps steering to ±32767 to mimic the G29 range.
- **FFB gain** – multiplies the shaped torque inside `FFBUpdateThread`. Raise this if you want a heavier wheel; keep it below 1.0 when using `dummy_hcd` to avoid oscillations.

## Force Feedback & Gadget Flow

1. Games talk to the kernel `hid-lg` driver, which sends 7-byte OUTPUT reports over interface 0.
2. The emulator’s USB gadget OUTPUT thread polls `/dev/hidg0`, buffers into 7-byte packets, and calls `ParseFFBCommand()` immediately.
3. The physics thread (125 Hz) shapes torque, adds autocenter springs, applies gain, and runs a critically damped second-order model before blending with mouse steering.
4. HID IN reports are serialized by the gadget polling thread, which is the sole writer to `/dev/hidg0`. Every SendState just marks the data as dirty, keeping the main loop lock-free.

## Troubleshooting

| Symptom | Checks |
|---------|--------|
| Host never sees the wheel | `lsusb | grep 046d:c24f`, confirm `dummy_hcd`/hardware UDC is loaded, ensure no old gadget instance is bound to the UDC |
| Permission errors | Always run as root (or via sudo) so ConfigFS and `/dev/hidg0` are writable |
| Wrong keyboard/mouse grabbed | Set `keyboard=` and `mouse=` in `/etc/wheel-emulator.conf` to the desired event nodes |
| Steering too twitchy | Lower `sensitivity` in the config (values map roughly linearly) |
| Force feedback weak | Increase `[ffb] gain`, but stay ≤4.0 to keep the physics stable |

## Documentation

- `README.md` – operational overview (this file)
- `logics.md` – deep dive into architecture, threads, and HID layout

## License

Open source. Contributions welcome.
