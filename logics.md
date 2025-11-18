# Wheel Emulator Architecture

## Purpose
Transform keyboard+mouse into Logitech G29 Racing Wheel for racing games using Linux uinput/evdev APIs.

---

## Project Structure

```
wheel-emulator/
├── Makefile              # Build configuration
├── logics.md            # This file - architecture documentation
├── README.md            # User documentation
└── src/
    ├── main.cpp         # Entry point, main loop, device detection mode
    ├── config.h/cpp     # Configuration loading with device paths
    ├── input.h/cpp      # Keyboard/mouse reading, device discovery
    └── gamepad.h/cpp    # Virtual Xbox 360 controller
```

---

## Components

### Config
- **Location**: `/etc/wheel-emulator.conf` (system-wide only)
- **Format**: INI file with sections: `[devices]`, `[sensitivity]`, `[button_mapping]`
- **Features**:
  - Stores explicit device paths (keyboard/mouse)
  - Sensitivity setting (1-100, default 50)
  - Custom button mappings
  - Auto-generates default config if missing
  - Can be updated via `UpdateDevices()` method

### Input
- **Discovery**: Explicit device paths (from config) OR auto-detection
- **Capabilities**: 
  - Reads keyboard/mouse events
  - Maintains key state array
  - Detects Ctrl+M toggle
  - Grab/ungrab devices for exclusive access
- **Auto-detection**:
  - Priority-based device selection
  - Filters out unwanted devices (consumer control, system control)
  - Prefers real mice over touchpads

### Gamepad
- **Type**: Virtual Logitech G29 Racing Wheel (VID=0x046d, PID=0xc24f)
- **Axes**:
  - `ABS_X`: Steering wheel (steering, -32768 to 32767)
  - `ABS_Y`: Y axis (unused, always 0)
  - `ABS_RX/RY`: RX/RY axes (unused, always 0)
  - `ABS_Z`: Brake pedal (0-255)
  - `ABS_RZ`: Throttle pedal (0-255)
  - `ABS_HAT0X/Y`: D-Pad (-1, 0, 1)
- **Buttons**: A, B, X, Y, LB, RB, Select, Start

---

## Program Flow

### Detection Mode (`--detect` flag)

Interactive device identification:

1. Check root privileges
2. Open all `/dev/input/event*` devices
3. Filter by capabilities:
   - Keyboard: `EV_KEY` with `KEY_A` or `KEY_SPACE`
   - Mouse: `EV_REL` with `REL_X`
4. **Keyboard detection** (5 seconds):
   - User types on keyboard
   - Count key press events per device
   - Select device with most events
5. **Mouse detection** (5 seconds):
   - User moves mouse
   - Count `REL_X` movement events per device
   - Select device with most events
6. **Auto-configure**:
   - Update `/etc/wheel-emulator.conf` with detected paths
   - Write to `[devices]` section
7. Exit

### Normal Mode

1. **Initialization**:
   - Check root privileges
   - Load config from `/etc/wheel-emulator.conf`
   - Create virtual Logitech G29 Racing Wheel via `/dev/uinput`
   - Discover keyboard and mouse (explicit path or auto-detect)
   - Start with emulation **disabled** (devices not grabbed)

2. **Main Loop** (1000 Hz):
   ```
   while running:
       Read keyboard events → update key state map
       Read mouse events → accumulate delta_x
       
       Check for Ctrl+M toggle:
           if toggled ON:  grab devices, enable emulation
           if toggled OFF: ungrab devices, disable emulation
       
       if emulation enabled:
           Update steering (accumulative, linear)
           Update throttle/brake (analog ramping)
           Update buttons and D-Pad
           Send gamepad state
       else:
           Send neutral state
       
       Sleep 1ms
   ```

3. **Cleanup** (Ctrl+C):
   - Ungrab devices
   - Close file descriptors (RAII)
   - Destroy virtual gamepad
   - Exit

---

## Algorithms

### Steering (Accumulative Linear)

```cpp
// Pure accumulative steering
steering += delta * sensitivity * 0.111f;
steering = clamp(steering, -32768.0f, 32767.0f);
```

**Characteristics**:
- **Linear**: Each pixel of mouse movement adds a constant amount
- **Accumulative**: Steering value persists until mouse moves opposite direction
- **Sensitivity scaling**: 
  - `sensitivity=50`: ~15cm mouse movement for full lock at 1000 DPI
  - `sensitivity=100`: ~7.5cm for full lock
  - `sensitivity=25`: ~30cm for full lock
- **Formula breakdown**:
  - At 1000 DPI: 15cm ≈ 5905 pixels
  - Full lock = 32768 units
  - Multiplier: `32768 / 5905 ≈ 5.55` at sensitivity=50
  - General: `multiplier = sensitivity * 0.111`

### Throttle/Brake (Analog Ramping)

```cpp
if (key_pressed):
    value = min(100.0, value + 3.0)  // Ramp up 3% per frame
else:
    value = max(0.0, value - 3.0)    // Ramp down 3% per frame

output = uint8_t(value * 2.55)  // Convert to 0-255 range
```

**Timing**: At 1000 Hz, full throttle/brake in ~33ms (33 frames × 3%)

### D-Pad

```cpp
dpad_x = arrow_right - arrow_left   // -1, 0, or 1
dpad_y = arrow_down - arrow_up       // -1, 0, or 1
```

### Toggle Detection (Ctrl+M)

```cpp
both_pressed = keys[KEY_LEFTCTRL] && keys[KEY_M]
if (both_pressed && !prev_toggle):
    toggle_state = !toggle_state
prev_toggle = both_pressed
```

**Edge detection**: Only triggers on press, not on hold

---

## Input Mappings

| Input | Output | Description |
|-------|--------|-------------|
| Mouse X | `ABS_X` | Steering (accumulative) |
| W | `ABS_RZ` | Throttle (right trigger) |
| S | `ABS_Z` | Brake (left trigger) |
| Arrow keys | `ABS_HAT0X/Y` | D-Pad |
| Q/E/F/G/H | `BTN_A/B/X/Y/TL` | Buttons (configurable) |
| Ctrl+M | - | Toggle emulation on/off |
| Ctrl+C | - | Exit program |

---

## Configuration File

**Location**: `/etc/wheel-emulator.conf`

**Format**:
```ini
# Wheel Emulator Configuration
# Run with --detect flag to identify your devices

[devices]
# Specify exact device paths (use --detect to find them)
# Leave empty for auto-detection
keyboard=/dev/input/event6
mouse=/dev/input/event11

[sensitivity]
sensitivity=50

[button_mapping]
### Button Mapping

Available buttons:
- BTN_A, BTN_B, BTN_X, BTN_Y
- BTN_TL (left bumper), BTN_TR (right bumper)
- BTN_SELECT, BTN_START

Available keys: Any KEY_* from Linux input event codes (e.g., KEY_Q, KEY_E, KEY_SPACE, KEY_TAB)

Note: G29 has 20+ buttons available on the real wheel, but we map only basic ones for keyboard control.

KEY_Q=BTN_A
KEY_E=BTN_B
KEY_F=BTN_X
KEY_G=BTN_Y
KEY_H=BTN_TL
# KEY_R=BTN_TR
# KEY_TAB=BTN_SELECT
# KEY_ENTER=BTN_START
# KEY_LEFTSHIFT=BTN_THUMBL
# KEY_LEFTCTRL=BTN_THUMBR
# KEY_ESC=BTN_MODE
```

---

## Device Discovery

### Auto-detection Priority System

**Keyboard devices**:
| Priority | Criteria |
|----------|----------|
| 100 | Name contains " keyboard" (with space) |
| 50 | Name contains "keyboard" |
| 10 | Consumer Control or System Control |

**Mouse devices**:
| Priority | Criteria |
|----------|----------|
| 100 | Real mice ("mouse", "wireless device", brand names) |
| 50 | Generic devices with `REL_X` |
| 20 | Virtual mouse from touchpads (UNIW, ELAN, Synaptics) |
| 10 | Touchpad devices |
| 5 | Consumer Control or System Control |

**Filtering**:
- Excludes keyboards with pointer capabilities from mouse selection
- Opens devices with `O_RDONLY | O_NONBLOCK`

---

## Build System

```makefile
CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2
TARGET = wheel-emulator
```

**Commands**:
- `make` - Build executable
- `make clean` - Remove build artifacts
- `make install` - Install to `/usr/local/bin` (requires root)

---

## Usage

### First-time Setup
```bash
sudo ./wheel-emulator --detect
# Follow prompts: type on keyboard, move mouse
# Config automatically updated
```

### Normal Usage
```bash
sudo ./wheel-emulator
# Press Ctrl+M to enable emulation
# Press Ctrl+M again to disable
# Press Ctrl+C to exit
```

### Manual Configuration
Edit `/etc/wheel-emulator.conf`:
```ini
[devices]
keyboard=/dev/input/event6
mouse=/dev/input/event11

[sensitivity]
sensitivity=50
```

---

## Technical Details

### Event Reading
- **Non-blocking I/O**: All devices opened with `O_NONBLOCK`
- **Polling rate**: 1000 Hz (1ms sleep per loop iteration)
- **Event consumption**: All pending events read each iteration
- **Key state**: Boolean array indexed by `KEY_*` codes

### Device Grabbing
- **Function**: `ioctl(fd, EVIOCGRAB, enable)`
- **Purpose**: Exclusive access to input devices when emulation enabled
- **Behavior**: 
  - When enabled: OS and other apps don't receive input
  - When disabled: Normal input passthrough
- **Note**: Can still read events while grabbed

### Virtual Gamepad Creation
```cpp
// uinput device setup
fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
ioctl(fd, UI_SET_EVBIT, EV_KEY);
ioctl(fd, UI_SET_EVBIT, EV_ABS);

// Setup each axis with UI_ABS_SETUP
struct uinput_abs_setup abs_setup;
abs_setup.code = ABS_X;
abs_setup.absinfo.minimum = -32768;
abs_setup.absinfo.maximum = 32767;
ioctl(fd, UI_ABS_SETUP, &abs_setup);

// Create device
struct uinput_setup setup;
setup.id.vendor = 0x046d;  // Logitech
setup.id.product = 0xc24f; // G29 Racing Wheel
ioctl(fd, UI_DEV_SETUP, &setup);
ioctl(fd, UI_DEV_CREATE);
```

### Event Emission
```cpp
void EmitEvent(type, code, value) {
    struct input_event ev;
    ev.type = type;
    ev.code = code;
    ev.value = value;
    gettimeofday(&ev.time, NULL);
    write(fd, &ev, sizeof(ev));
}

// Always emit EV_SYN after all events in a frame
EmitEvent(EV_SYN, SYN_REPORT, 0);
```

---

## Why This Design?

1. **Explicit device paths**: Auto-detection can fail with multiple devices; `--detect` mode lets user confirm
2. **Grab/ungrab toggle**: Allows using keyboard/mouse normally when not racing
3. **Accumulative steering**: Mimics real steering wheel behavior - wheel stays where you turn it
4. **Linear sensitivity**: Direct relationship between mouse movement and steering angle
5. **System-wide config**: `/etc` location ensures consistency across sessions
6. **G29 emulation**: Direct wheel emulation for better compatibility with racing games

---

## Troubleshooting

### Steering doesn't work
- Check sensitivity value in config (try 50)
- Verify mouse device path is correct (`--detect` mode)
- Ensure emulation is enabled (Ctrl+M)

### Keyboard/mouse not grabbed
- Must run as root (`sudo`)
- Check device paths in config
- Try `--detect` mode to verify devices

### Game doesn't detect wheel
- Verify virtual wheel created: `ls /dev/input/by-id/ | grep Logitech`
- Check Steam Input settings (may need to disable Steam Input for this game)
- Test with `jstest /dev/input/js0` or `evtest /dev/input/eventX`

### Ctrl+M doesn't work
- Verify keyboard device is correct
- Check if keyboard has multiple event devices (use `--detect`)
- Try different keyboard if external keyboard connected

---

## License & Credits

Created for racing games on Linux. Uses standard Linux kernel APIs (uinput, evdev).
