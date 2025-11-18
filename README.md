# Wheel Emulator

Transform keyboard and mouse into a Logitech G29 Racing Wheel for racing games on Linux.

## Features

- Mouse horizontal movement → Steering wheel
- W/S keys → Throttle/Brake with analog ramping
- Arrow keys → D-Pad
- Configurable button mappings
- Toggle emulation on/off with Ctrl+M
- Adjustable steering sensitivity

## Requirements

- Linux with uinput support
- Root privileges (for /dev/uinput access)
- g++ compiler with C++17 support

## Building

```bash
make
```

## Installation

```bash
sudo make install
```

This installs the binary to `/usr/local/bin/wheel-emulator`.

## Usage

Run with root privileges:

```bash
sudo ./wheel-emulator
```

### Controls

- **Ctrl+M** - Toggle emulation on/off
- **Ctrl+C** - Exit program
- **Mouse** - Steering (horizontal movement)
- **W** - Throttle (analog)
- **S** - Brake (analog)
- **Arrow keys** - D-Pad
- **Q/E/F/G/H** - Buttons (default: A/B/X/Y/LB)

## Configuration

Configuration file is automatically created at `~/.config/wheel-emulator.conf` on first run.

You can also place it at `/etc/wheel-emulator.conf` for system-wide configuration.

### Example Configuration

```ini
[sensitivity]
sensitivity=20

[button_mapping]
KEY_Q=BTN_A
KEY_E=BTN_B
KEY_F=BTN_X
KEY_G=BTN_Y
KEY_H=BTN_TL
```

### Sensitivity

Values range from 1 to 100:
- Linear scaling: sensitivity directly multiplies mouse movement
- Lower values (1-10) = very low sensitivity, requires large mouse movements
- Medium values (20-50) = balanced sensitivity
- Higher values (80-100) = high sensitivity, small movements create large steering
- Default: 20
- Recommended for racing: 5-20 depending on mouse DPI

### Button Mapping

Available buttons (from G29 wheel):
- BTN_A, BTN_B, BTN_X, BTN_Y
- BTN_TL (left bumper), BTN_TR (right bumper)
- BTN_SELECT, BTN_START

Available keys: Any KEY_* from Linux input event codes (e.g., KEY_Q, KEY_E, KEY_SPACE, KEY_TAB)

## How It Works

1. Auto-discovers keyboard and mouse from `/dev/input/event*`
   - Prioritizes real keyboards over consumer control devices
   - Prioritizes real mice, excludes keyboard pointer devices
   - Filters out touchpads and virtual input devices
2. Creates a virtual Logitech G29 Racing Wheel via `/dev/uinput`
3. When enabled, grabs keyboard and mouse for exclusive access
4. Reads input events and translates them to gamepad events
5. Sends gamepad state at 1000 Hz

When emulation is disabled (default), all inputs pass through normally. Press Ctrl+M to enable.

## Architecture

See `LOGICK.txt` for detailed architecture documentation.

## License

Open source - feel free to modify and distribute.
