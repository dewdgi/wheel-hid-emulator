# Wheel HID Emulator

# Demonstration video
[![Watch the demo](https://img.youtube.com/vi/8nfdKWtkmtE/0.jpg)](https://www.youtube.com/watch?v=8nfdKWtkmtE)

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

What you should see:
- The program creates/reuses the `g29wheel` gadget immediately and pushes a neutral frame, so the OS/game sees a Logitech wheel even before you enable input grabbing.
- Press **Ctrl+M** to toggle emulation. When enabled the app grabs your keyboard/mouse and streams wheel data; when disabled it releases them but keeps the gadget alive in a neutral state.
- Press **Ctrl+C** to exit. The gadget remains until you reboot or manually remove it (see troubleshooting).

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
- **Need to remove the gadget:** `echo '' | sudo tee /sys/kernel/config/usb_gadget/g29wheel/UDC` followed by deleting the `g29wheel` directory (details in `logics.md`).
- **Force feedback feels weak/too strong:** adjust `[ffb] gain` in the config and restart.

## Want the nitty-gritty?

`logics.md` documents every subsystem (USB gadget sequencing, enable/disable handshake, threads, HID layout, etc.). Check it out if you need to hack on the codebase or understand exactly how the neutral/snapshot flow works.

## License

Open source. Contributions welcome.
