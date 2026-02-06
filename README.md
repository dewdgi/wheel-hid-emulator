# Wheel HID Emulator

**Keyboard + mouse as a Logitech G29 racing wheel on Linux via USB HID gadget.**

[![Watch the demo](https://img.youtube.com/vi/8nfdKWtkmtE/maxresdefault.jpg)](https://www.youtube.com/watch?v=8nfdKWtkmtE)

## Requirements

- Linux kernel with `CONFIG_USB_CONFIGFS=y` and `libcomposite`
- Root privileges
- g++ with C++17

## Install & Run

```bash
make
sudo modprobe libcomposite
sudo modprobe dummy_hcd   # only for self-hosted testing
sudo ./wheel-emulator
```

**Ctrl+M** — toggle emulation. **Ctrl+C** — exit.

### NixOS

```
nix profile add github:dewdgi/wheel-hid-emulator#wheel-hid-emulator
```

## Controls

| Action | Input | HID Field |
| :--- | :--- | :--- |
| **Steer** | Mouse X | ABS_X |
| **Throttle** | `W` | ABS_Z |
| **Brake** | `S` | ABS_RZ |
| **Clutch** | `A` | ABS_Y |
| **D-Pad** | Arrows | ABS_HAT0X/Y |

**Buttons:**

| Button | Key | | Button | Key |
| :--- | :--- | :--- | :--- | :--- |
| Cross | `Q` | | L1 | `H` |
| Circle | `E` | | R1 | `R` |
| Square | `F` | | L2 | `T` |
| Triangle | `G` | | R2 | `Y` |
| Select | `U` | | Start | `I` |
| L3 | `O` | | R3 | `P` |
| PS/Mode | `1` | | TH 1-12 | `2-9`, `0`, `LShift`, `Space`, `Tab`, `Enter` |

## Configuration

`/etc/wheel-emulator.conf`:

```ini
[devices]
keyboard=              # blank = auto-detect
mouse=                 # blank = auto-detect

[sensitivity]
sensitivity=50         # 1-100

[ffb]
gain=0.3               # 0.1-4.0
```

## License

Open source. Contributions welcome.

---

Architecture details: [`logics.md`](logics.md)
