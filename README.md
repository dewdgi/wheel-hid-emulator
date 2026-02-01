# Wheel HID Emulator (Windows Port)

**Turn your keyboard and mouse into a virtual Logitech G29 racing wheel.**

This program emulates a steering wheel on Windows using the **vJoy** driver. It enables Force Feedback (FFB) support in games like *Assetto Corsa*, *Euro Truck Simulator 2*, *Forza*, etc., using just a mouse.

For the internals, check [`logics.md`](logics.md).

## Features

-   **Mouse Steering:** Precise 1:1 mouse-to-wheel mapping with sensitivity control.
-   **Force Feedback:** The mouse pointer resists movement based on game physics (springs, bumps, curbs, drifting).
-   **Single Portable Executable:** No installation required (other than vJoy).
-   **Logitech G29 Emulation:** Recognized as a real wheel by most games.

## Prerequisites

1.  **vJoy Driver** (v2.1.9 or later) installed.
2.  Configure vJoy Device 1 (using `Configure vJoy` app):
    -   **Axes:** X, Y, Z, Rx, Rz (All enabled)
    -   **Buttons:** 12 or more
    -   **FFB Effects:** Enable "Constant Force", "Ramp", "Spring", etc.

## How to Run

1.  Download `wheel-emulator.exe` from Releases.
2.  Run `wheel-emulator.exe`.
3.  **In-Game:**
    -   Bind Steering to vJoy Axis X.
    -   Bind Throttle/Brake/Clutch to vJoy Axes (usually Y/Z/Rz).
4.  **Toggle Emulation:** Press **Ctrl + M**.
    -   The mouse cursor will hide/lock.
    -   Move the mouse to steer.
    -   Press **Ctrl + M** again to release the mouse.

## Controls

| Action | Input | vJoy Axis |
| :--- | :--- | :--- |
| **Steer** | Mouse X | Axis X |
| **Throttle** | `W` | Axis Z |
| **Brake** | `S` | Axis Rz |
| **Clutch** | `A` | Axis Y |
| **D-Pad** | Arrows | Hat Switch |

**Buttons:**
`Q, E, F, G, H, R, T, Y` ... map to Buttons 1-8.
See source code or experiment in `joy.cpl` to find all mappings.

## Configuration (`wheel-emulator.conf`)

The program looks for `wheel-emulator.conf` in the same directory. If missing, it uses defaults.

```ini
[sensitivity]
sensitivity=50    # 1-100. Higher = faster steering response.

[ffb]
gain=1.0          # 0.1-4.0. Force Feedback strength multiplier.
```

## Building from Source

**Requirements:**
-   **MinGW-w64** (g++) added to PATH.
-   **vJoy SDK** (Headers/Libs included in `src/vjoy_sdk` or installed).

**Build:**
Run `build_with_g++.bat`. It will produce a single `wheel-emulator.exe`.

## Troubleshooting

-   **"vJoy not enabled":** Install vJoy and ensure Device 1 is active.
-   **FFB feels wrong:**
    -   In-Game: Invert FFB if the wheel pulls to the side instead of centering.
    -   Conf: Adjust `gain` in `wheel-emulator.conf`.
-   **Mouse jitters:** Lower the game's FFB update rate or reduce `sensitivity`.

## License

Open Source (MIT).
This project uses the vJoy SDK (Public Domain/MIT).
