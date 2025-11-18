# Wheel HID Emulator

Transform keyboard and mouse into a Logitech G29 Racing Wheel for racing games on Linux.

## Features

- Emulates Logitech G29 Driving Force Racing Wheel (VID:0x046d PID:0xc24f)
- Mouse horizontal movement → Steering wheel
- W/S keys → Throttle/Brake pedals (ramping 0-100%)
- 25 buttons mapped to keyboard keys
- Arrow keys → D-Pad
- Ctrl+M to toggle emulation on/off
- 125 Hz update rate
- Auto-detection mode for keyboard/mouse

## Requirements

- Linux with uinput support
- Root privileges (for /dev/uinput access)
- g++ compiler with C++17 support

## Building

```bash
make
```

## Usage

```bash
# First time: detect your keyboard and mouse
sudo ./wheel-emulator --detect

# Run emulator
sudo ./wheel-emulator
```

Press **Ctrl+M** to toggle emulation on/off.  
Press **Ctrl+C** to exit.

## Controls

**Steering & Pedals:**
- Mouse X → Steering wheel
- W key (hold) → Throttle (0-100%)
- S key (hold) → Brake (0-100%)

**D-Pad:**
- Arrow keys → Up/Down/Left/Right

**Buttons (25 total):**  
Q, E, F, G, H, R, T, Y, U, I, O, P, 1-0, LShift, Space, Tab

See `/etc/wheel-emulator.conf` for suggested game actions.

## Configuration

Edit `/etc/wheel-emulator.conf`:

```ini
[devices]
keyboard=/dev/input/event6
mouse=/dev/input/event11

[sensitivity]
sensitivity=50
```

**Sensitivity:** 1-100 (default 50). Internally scaled by 0.2.

## How It Works

1. Creates virtual Logitech G29 wheel via `/dev/uinput`
2. Reads keyboard/mouse from `/dev/input/event*`
3. When enabled (Ctrl+M), grabs devices for exclusive access
4. Translates mouse/keyboard input to wheel/pedals/buttons
5. Emits events at 125 Hz

## Technical Details

**Virtual Device:**
- Name: "Logitech G29 Driving Force Racing Wheel"
- VID: 0x046d, PID: 0xc24f

**Axes:**
- ABS_X: Steering (-32768 to 32767)
- ABS_Y: Unused (always 32767)
- ABS_Z: Brake (32767=rest, -32768=pressed) **INVERTED**
- ABS_RZ: Throttle (32767=rest, -32768=pressed) **INVERTED**
- ABS_HAT0X/Y: D-Pad

**Inverted Pedals:** Real G29 uses inverted pedals. This is required for proper detection in Windows/games.

## Troubleshooting

**Wheel not detected:**
- Check: `ls /dev/input/by-id/ | grep Logitech`
- Test: `jstest /dev/input/js0` or `evtest`

**Wrong keyboard/mouse:**
- Run: `sudo ./wheel-emulator --detect`

**Permission denied:**
- Run with `sudo`

**Steering too sensitive/insensitive:**
- Edit `/etc/wheel-emulator.conf`, change `sensitivity=50`

## Documentation

- `README.md` - This file
- `logics.md` - Detailed architecture

## License

Open source.
