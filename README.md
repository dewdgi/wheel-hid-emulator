# Wheel HID Emulator

# Demonstration video
[![Watch the demo](https://img.youtube.com/vi/8nfdKWtkmtE/maxresdefault.jpg)](https://www.youtube.com/watch?v=8nfdKWtkmtE)

Turn a keyboard and mouse into a Logitech G29 racing wheel on Linux. The emulator exposes a real USB HID gadget so your system instantly recognizes it as a wheel without extra drivers. If you want to dive into the internals, check [`logics.md`](logics.md).

## Before You Start

- Linux kernel with `CONFIG_USB_CONFIGFS=y`, `libcomposite`, and either a hardware UDC or `dummy_hcd` for loopback testing.
- Root privileges (required for ConfigFS operations, `/dev/hidg0`, and grabbing input devices).
- g++ with C++17 support for building the binary.

## Install & Run

```bash
make                # build the binary
sudo make install   # optional: copy to /usr/local/bin

# Ensure USB gadget support is ready (no-op if modules already loaded)
sudo modprobe libcomposite
sudo modprobe dummy_hcd   # only needed for self-hosted testing

sudo ./wheel-emulator
```

## For NixOS
```
nix profile install github:youruser/wheel-hid-emulator#wheel-hid-emulator

or

nix-env -f . -iA wheel-hid-emulator
```

What you should see:
- The program creates/reuses the `g29wheel` gadget immediately and pushes a neutral frame, so the OS/game sees a Logitech wheel even before you enable input grabbing.
- Press **Ctrl+M** to toggle emulation. Hold both keys briefly and release them to fire the toggle—waiting for the release ensures the desktop sees the key-up events before the grab happens, eliminating stuck `m`/Enter spam.
- Press **Ctrl+C** to exit. Shutdown automatically unbinds and deletes the gadget tree; manual cleanup is only needed if a previous run crashed.
- Shutdown now wakes any blocking input threads via an internal eventfd, so Ctrl+C responds immediately even if no keyboard/mouse events are happening.

## Controls & Mapping

| Action | Input | HID Field |
|--------|-------|-----------|
| Steering | Mouse X movement | ABS_X |
| Throttle | `W` | ABS_Z (inverted) |
| Brake | `S` | ABS_RZ (inverted) |
| Clutch | `A` | ABS_Y (inverted) |
| D-Pad | Arrow keys | ABS_HAT0X/ABS_HAT0Y |

| Button | Keyboard | Typical bind |
|--------|----------|---------------|
| South / Cross | Q | Downshift |
| East / Circle | E | Upshift |
| West / Square | F | Flash lights |
| North / Triangle | G | Horn |
| L1 | H | Lights toggle |
| R1 | R | Camera right |
| L2 | T | Telemetry |
| R2 | Y | HUD cycle |
| Select | U | Pit limiter |
| Start | I | Ignition |
| L3 | O | Wipers |
| R3 | P | Pause |
| PS / Mode | 1 | TC down |
| Dead | 2 | TC up |
| TH 1 | 3 | ABS down |
| TH 2 | 4 | ABS up |
| TH 3 | 5 | Brake bias fwd |
| TH 4 | 6 | Brake bias back |
| TH 5 | 7 | Engine map – |
| TH 6 | 8 | Engine map + |
| TH 7 | 9 | Pit request |
| TH 8 | 0 | Leaderboard |
| TH 9 | Left Shift | Camera left |
| TH 10 | Space | Handbrake |
| TH 11 | Tab | Cycle view |
| TH 12 | Enter | Extra bind |

Feel free to remap these inside your game—Linux will always report a Logitech G29.

## Architecture Snapshot

- `DeviceScanner` (`src/input/device_scanner.*`) handles low-level `/dev/input/event*` discovery, hotplugging, grabs, and key state aggregation.
- `InputManager` (`src/input/input_manager.*`) runs a reader thread that transforms raw events into `InputFrame` objects (mouse delta + logical button/pedal snapshot) for the main loop, and it now diffs frames under a mutex so rapid button changes can’t be lost.
- `WheelDevice` (`src/wheel_device.*`) owns steering/pedal/button state plus the force-feedback pipeline, and is the only code that mutates HID state when emulation is enabled.
- `hid::HidDevice` (`src/hid/hid_device.*`) wraps ConfigFS and `/dev/hidg0`, creating/binding the Logitech G29 gadget on startup, tearing it down on exit, and reusing any leftover tree from a prior crash when needed. Its descriptor accessors are now mutexed, keeping write threads safe while the endpoint is reopened or reset.
- `logics.md` contains a deeper dive into threading, HID layout, and the enable/disable handshake if you need more detail.

## Configure (`/etc/wheel-emulator.conf`)

```ini
[devices]
keyboard=
mouse=

[sensitivity]
sensitivity=50    # 1–100, higher = faster steering

[ffb]
gain=0.3          # 0.1–4.0 force multiplier
```

- Leave `keyboard`/`mouse` blank for auto-discovery, or set them to `/dev/input/eventX` paths to pin specific devices.
- `sensitivity` scales mouse movement (default 50 works for most setups).
- `gain` multiplies force feedback strength; start low when using `dummy_hcd` to avoid oscillations.

## Troubleshooting

- **Wheel never appears in-game:** ensure `libcomposite`/`dummy_hcd` are loaded, `sudo ./wheel-emulator` is running, and `lsusb | grep 046d:c24f` shows the gadget.
- **Wrong keyboard/mouse grabbed:** set explicit device paths in the config and restart the emulator.
- **Toggle fired while keys still held (shell keeps printing characters):** be sure to release both Ctrl and `M` after pressing them together. The emulator only toggles on the release edge now so the OS receives the key-up before devices are grabbed.
- **Need to remove the gadget:** normally unnecessary because shutdown already removes it; if a crashed process left debris, run `echo '' | sudo tee /sys/kernel/config/usb_gadget/g29wheel/UDC` and delete the `g29wheel` directory (details in `logics.md`).
- **Force feedback feels weak/too strong:** adjust `[ffb] gain` in the config and restart.

## Want the nitty-gritty?

`logics.md` documents every subsystem (USB gadget sequencing, enable/disable handshake, threads, HID layout, etc.). Check it out if you need to hack on the codebase or understand exactly how the neutral/snapshot flow works.

## License

Open source. Contributions welcome.
