#include "gamepad.h"
#include "input.h"
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <cstring>
#include <iostream>
#include <dirent.h>
#include <cstdlib>
#include <linux/uhid.h>
#include <linux/uinput.h>
#include <linux/input-event-codes.h>


GamepadDevice::GamepadDevice() 
        : fd(-1), use_uhid(false), use_gadget(false), gadget_running(false),
            enabled(false), steering(0), throttle(0.0f), brake(0.0f), clutch(0.0f), dpad_x(0), dpad_y(0),
            ffb_force(0), ffb_autocenter(0), ffb_enabled(true), user_torque(0.0f) {}

// Clutch axis update (ramp like throttle/brake)
void GamepadDevice::UpdateClutch(bool pressed) {
    std::lock_guard<std::mutex> lock(state_mutex);
    while (gadget_running && running) {
        // Wait for either read or write ready (with short timeout for fast shutdown)
        int ret = poll(&pfd, 1, 20);  // 20ms timeout for responsiveness

        if (!gadget_running || !running) break;

        if (ret < 0) {
            if (errno == EINTR) continue;
            std::cerr << "USB Gadget poll error: " << strerror(errno) << std::endl;
            break;
        }

        if (ret == 0) {
            // Timeout - continue loop
            continue;
        }

        // Check for FFB OUTPUT reports from host
        if (pfd.revents & POLLIN) {
            ssize_t bytes = read(fd, ffb_buffer, sizeof(ffb_buffer));
            if (bytes == 7) {
                // Valid FFB command received
                ParseFFBCommand(ffb_buffer, bytes);
            } else if (bytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                std::cerr << "USB Gadget read error: " << strerror(errno) << std::endl;
            }
        }

        // Send INPUT report when host is ready to receive
        if (pfd.revents & POLLOUT) {
            std::vector<uint8_t> report;
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                report = BuildHIDReport();
            }
            ssize_t bytes = write(fd, report.data(), report.size());
            if (bytes < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    if (errno == ESHUTDOWN || errno == ECONNRESET) {
                        std::cout << "USB Gadget device disconnected" << std::endl;
                        break;
                    }
                    std::cerr << "USB Gadget write error: " << strerror(errno) << std::endl;
                }
            }
        }

        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            std::cerr << "USB Gadget poll error flags" << std::endl;
            break;
        }
    }
// Query enabled state (mutex-protected)
bool GamepadDevice::IsEnabled() {
    std::lock_guard<std::mutex> lock(state_mutex);
    return enabled;
}

// Toggle enabled state atomically (mutex-protected)
void GamepadDevice::ToggleEnabled(Input& input) {
    std::lock_guard<std::mutex> lock(state_mutex);
    enabled = !enabled;
    input.Grab(enabled);
    if (enabled) {
        std::cout << "Emulation ENABLED" << std::endl;
    } else {
        std::cout << "Emulation DISABLED" << std::endl;
    }
}

// Logitech G29 HID Report Descriptor
// Based on real G29 wheel descriptor with proper OUTPUT report for hid-lg driver
// The kernel driver expects OUTPUT report with no report ID (report 0) for FFB commands
// Updated HID descriptor for 26 buttons (matching G29 and logics.md)
static const uint8_t g29_hid_descriptor[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x04,        // Usage (Joystick)
    0xA1, 0x01,        // Collection (Application)
    0xA1, 0x02,        //   Collection (Logical)
    0x09, 0x01,        //     Usage (Pointer)
    0xA1, 0x00,        //     Collection (Physical)
    0x09, 0x30,        //       Usage (X) - Steering
    0x09, 0x31,        //       Usage (Y) - Clutch (G29: 4 axes)
    0x09, 0x32,        //       Usage (Z) - Accelerator
    0x09, 0x35,        //       Usage (Rz) - Brake
    0x15, 0x00,        //       Logical Minimum (0)
    0x27, 0xFF, 0xFF, 0x00, 0x00,  //       Logical Maximum (65535)
    0x35, 0x00,        //       Physical Minimum (0)
    0x47, 0xFF, 0xFF, 0x00, 0x00,  //       Physical Maximum (65535)
    0x75, 0x10,        //       Report Size (16)
    0x95, 0x04,        //       Report Count (4)
    0x81, 0x02,        //       Input (Data,Var,Abs)
    0xC0,              //     End Collection
    0x09, 0x39,        //     Usage (Hat switch)
    0x15, 0x00,        //     Logical Minimum (0)
    0x25, 0x07,        //     Logical Maximum (7)
    0x35, 0x00,        //     Physical Minimum (0)
    0x46, 0x3B, 0x01,  //     Physical Maximum (315)
    0x65, 0x14,        //     Unit (Degrees)
    0x75, 0x04,        //     Report Size (4)
    0x95, 0x01,        //     Report Count (1)
    0x81, 0x42,        //     Input (Data,Var,Abs,Null)
    0x75, 0x04,        //     Report Size (4)
    0x95, 0x01,        //     Report Count (1)
    0x81, 0x03,        //     Input (Const,Var,Abs)
    0x05, 0x09,        //     Usage Page (Button)
    0x19, 0x01,        //     Usage Minimum (Button 1)
    0x29, 0x1A,        //     Usage Maximum (Button 26)
    0x15, 0x00,        //     Logical Minimum (0)
    0x25, 0x01,        //     Logical Maximum (1)
    0x75, 0x01,        //     Report Size (1)
    0x95, 0x1A,        //     Report Count (26)
    0x81, 0x02,        //     Input (Data,Var,Abs)
    0x75, 0x06,        //     Report Size (6) (padding to next byte)
    0x95, 0x01,        //     Report Count (1)
    0x81, 0x03,        //     Input (Const,Var,Abs)
    0xC0,              //   End Collection (Logical)
    0xA1, 0x02,        //   Collection (Logical)
    0x09, 0x02,        //     Usage (0x02) - FFB usage
    0x15, 0x00,        //     Logical Minimum (0)
    0x26, 0xFF, 0x00,  //     Logical Maximum (255)
    0x95, 0x07,        //     Report Count (7)
    0x75, 0x08,        //     Report Size (8)
    0x91, 0x02,        //     Output (Data,Var,Abs)
    0xC0,              //   End Collection (Logical)
    0xC0               // End Collection (Application)
};

bool GamepadDevice::CreateUHID() {
    fd = open("/dev/uhid", O_RDWR);
    if (fd < 0) {
        std::cerr << "Failed to open /dev/uhid. Are you running as root?" << std::endl;
        std::cerr << "Make sure the uhid kernel module is loaded: sudo modprobe uhid" << std::endl;
        return false;
    }
    
    struct uhid_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = UHID_CREATE2;
    
    strcpy((char*)ev.u.create2.name, "Logitech G29 Driving Force Racing Wheel");
    ev.u.create2.rd_size = sizeof(g29_hid_descriptor);
    memcpy(ev.u.create2.rd_data, g29_hid_descriptor, sizeof(g29_hid_descriptor));
    ev.u.create2.bus = BUS_USB;
    ev.u.create2.vendor = 0x046d;   // Logitech
    ev.u.create2.product = 0xc24f;  // G29 Racing Wheel
    ev.u.create2.version = 0x0111;
    ev.u.create2.country = 0;
    
    if (write(fd, &ev, sizeof(ev)) < 0) {
        std::cerr << "Failed to create UHID device" << std::endl;
        close(fd);
        fd = -1;
        return false;
    }
    
    // Set file descriptor to non-blocking for event reading
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    
    // Wait for UHID_START event to confirm device creation
    bool device_started = false;
    for (int i = 0; i < 10; i++) {  // Try for 1 second (10 * 100ms)
        struct uhid_event response;
        ssize_t ret = read(fd, &response, sizeof(response));
        if (ret > 0) {
            if (response.type == UHID_START) {
                device_started = true;
                break;
            } else if (response.type == UHID_OPEN) {
                // Device opened by kernel, this is good
                device_started = true;
                break;
            }
        }
        usleep(100000); // 100ms
    }
    
    if (!device_started) {
        std::cerr << "Warning: UHID device created but UHID_START event not received" << std::endl;
        std::cerr << "Device may not be fully initialized" << std::endl;
    }
    
    std::cout << "Virtual Logitech G29 Racing Wheel created via UHID" << std::endl;
    std::cout << "VID:046d PID:c24f Version:0x0111" << std::endl;
    std::cout << "Device provides HIDRAW support for Proton/Wine compatibility" << std::endl;
    
    use_uhid = true;
    return true;
}

bool GamepadDevice::Create() {
    // Try USB Gadget first (proper USB device with full driver support)
    std::cout << "Attempting to create device using USB Gadget (real USB device)..." << std::endl;
    if (CreateUSBGadget()) {
        return true;
    }
    
    // Try UHID second (provides HIDRAW support)
    std::cout << "USB Gadget not available, trying UHID..." << std::endl;
    if (CreateUHID()) {
        return true;
    }
    
    // Fall back to uinput if both fail
    std::cout << "UHID failed, falling back to uinput..." << std::endl;
    std::cout << "Note: uinput doesn't provide HIDRAW, some games may not detect the wheel" << std::endl;
    return CreateUInput();
}

bool GamepadDevice::CreateUSBGadget() {
    // USB Gadget HID creates a REAL USB device that the Logitech driver can bind to
    // Setup USB Gadget ConfigFS if not already configured
    
    // Load required kernel modules automatically
    system("modprobe libcomposite 2>/dev/null");
    system("modprobe dummy_hcd 2>/dev/null");
    usleep(100000); // Give modules time to load
    
    const char* hidg_dev = "/dev/hidg0";
    
    // ALWAYS recreate the gadget to ensure correct configuration
    // (don't reuse old config which might have wrong report_length)
    std::cout << "Setting up USB Gadget with fresh configuration..." << std::endl;
    
    // Check if configfs is mounted, if not try to mount it
    if (access("/sys/kernel/config", F_OK) != 0) {
        system("mkdir -p /sys/kernel/config 2>/dev/null");
        system("mount -t configfs none /sys/kernel/config 2>/dev/null");
    }
    
    // Verify configfs and usb_gadget directory exists
    if (access("/sys/kernel/config/usb_gadget", F_OK) != 0) {
        std::cerr << "USB Gadget ConfigFS not available in kernel" << std::endl;
        std::cerr << "Kernel needs CONFIG_USB_CONFIGFS=y" << std::endl;
        return false;
    }
    
    // Check for UDC (USB Device Controller)
    if (access("/sys/class/udc", F_OK) != 0) {
        std::cerr << "No USB Device Controller (UDC) found" << std::endl;
        std::cerr << "Load dummy_hcd: sudo modprobe dummy_hcd" << std::endl;
        return false;
    }
    
    // Try to set up USB Gadget ConfigFS
    std::string cmd;
    
    // Remove existing gadget if present
    cmd = "cd /sys/kernel/config/usb_gadget 2>/dev/null && "
          "if [ -d g29wheel ]; then "
          "  cd g29wheel && "
          "  echo '' > UDC 2>/dev/null || true; "
          "  rm -f configs/c.1/hid.usb0 2>/dev/null || true; "
          "  rmdir configs/c.1/strings/0x409 2>/dev/null || true; "
          "  rmdir configs/c.1 2>/dev/null || true; "
          "  rmdir functions/hid.usb0 2>/dev/null || true; "
          "  rmdir strings/0x409 2>/dev/null || true; "
          "  cd .. && rmdir g29wheel 2>/dev/null || true; "
          "fi";
    system(cmd.c_str());
    
    // Build the HID descriptor as a hex string for the shell command
    std::string descriptor_hex;
    for (size_t i = 0; i < sizeof(g29_hid_descriptor); i++) {
        char buf[8];
        snprintf(buf, sizeof(buf), "\\x%02x", g29_hid_descriptor[i]);
        descriptor_hex += buf;
    }
    
    // Create gadget directory structure
    // IMPORTANT: Removed "no_out_endpoint" to allow OUTPUT reports (needed for FFB)
    cmd = "cd /sys/kernel/config/usb_gadget && "
          "mkdir g29wheel && cd g29wheel && "
          "echo 0x046d > idVendor && "
          "echo 0xc24f > idProduct && "
          "echo 0x0111 > bcdDevice && "
          "echo 0x0200 > bcdUSB && "
          "mkdir -p strings/0x409 && "
          "echo 'Logitech' > strings/0x409/manufacturer && "
          "echo 'G29 Driving Force Racing Wheel' > strings/0x409/product && "
          "echo '000000000001' > strings/0x409/serialnumber && "
          "mkdir -p functions/hid.usb0 && cd functions/hid.usb0 && "
          "echo 1 > protocol && echo 1 > subclass && echo 13 > report_length && "
          "printf '" + descriptor_hex + "' > report_desc && "
          "cd /sys/kernel/config/usb_gadget/g29wheel && "
          "mkdir -p configs/c.1/strings/0x409 && "
          "echo 'G29 Configuration' > configs/c.1/strings/0x409/configuration && "
          "echo 500 > configs/c.1/MaxPower && "
          "ln -s functions/hid.usb0 configs/c.1/ && "
          "UDC=$(ls /sys/class/udc 2>/dev/null | head -n1) && "
          "if [ -n \"$UDC\" ]; then echo $UDC > UDC; fi";
    
    int ret = system(cmd.c_str());
    if (ret != 0) {
        std::cerr << "Failed to setup USB Gadget (command returned " << ret << ")" << std::endl;
        return false;
    }
    
    usleep(500000); // Wait for device creation
    
    fd = open(hidg_dev, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        std::cerr << "USB Gadget configured but failed to open " << hidg_dev << std::endl;
        return false;
    }
    
    std::cout << "USB Gadget device created successfully!" << std::endl;
    std::cout << "Real USB Logitech G29 device (VID:046d PID:c24f)" << std::endl;
    std::cout << "Device will bind to kernel's hid-lg driver with proper USB interface 0" << std::endl;
    
    use_gadget = true;
    use_uhid = true;
    
    // Start polling thread to respond to host requests (like real USB HID device)
    gadget_running = true;
    gadget_thread = std::thread(&GamepadDevice::USBGadgetPollingThread, this);
    
    // Start FFB update thread (continuously applies FFB forces)
    ffb_running = true;
    ffb_thread = std::thread(&GamepadDevice::FFBUpdateThread, this);
    
    return true;
}

bool GamepadDevice::CreateUInput() {
    fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        std::cerr << "Failed to open /dev/uinput. Are you running as root?" << std::endl;
        return false;
    }
    
    // Enable event types
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    ioctl(fd, UI_SET_EVBIT, EV_FF);
    
    // Enable Force Feedback effects (matching real G29)
    ioctl(fd, UI_SET_FFBIT, FF_CONSTANT);
    ioctl(fd, UI_SET_FFBIT, FF_PERIODIC);
    ioctl(fd, UI_SET_FFBIT, FF_SQUARE);
    ioctl(fd, UI_SET_FFBIT, FF_TRIANGLE);
    ioctl(fd, UI_SET_FFBIT, FF_SINE);
    ioctl(fd, UI_SET_FFBIT, FF_SAW_UP);
    ioctl(fd, UI_SET_FFBIT, FF_SAW_DOWN);
    ioctl(fd, UI_SET_FFBIT, FF_RAMP);
    ioctl(fd, UI_SET_FFBIT, FF_SPRING);
    ioctl(fd, UI_SET_FFBIT, FF_FRICTION);
    ioctl(fd, UI_SET_FFBIT, FF_DAMPER);
    ioctl(fd, UI_SET_FFBIT, FF_INERTIA);
    ioctl(fd, UI_SET_FFBIT, FF_GAIN);
    ioctl(fd, UI_SET_FFBIT, FF_AUTOCENTER);
    
    // Enable joystick buttons (matching real G29 wheel - 25 buttons total)
    // 13 base buttons
    ioctl(fd, UI_SET_KEYBIT, BTN_SOUTH);    // Cross
    ioctl(fd, UI_SET_KEYBIT, BTN_EAST);     // Circle
    ioctl(fd, UI_SET_KEYBIT, BTN_WEST);     // Square
    ioctl(fd, UI_SET_KEYBIT, BTN_NORTH);    // Triangle
    ioctl(fd, UI_SET_KEYBIT, BTN_TL);       // L1
    ioctl(fd, UI_SET_KEYBIT, BTN_TR);       // R1
    ioctl(fd, UI_SET_KEYBIT, BTN_TL2);      // L2
    ioctl(fd, UI_SET_KEYBIT, BTN_TR2);      // R2
    ioctl(fd, UI_SET_KEYBIT, BTN_SELECT);   // Share
    ioctl(fd, UI_SET_KEYBIT, BTN_START);    // Options
    ioctl(fd, UI_SET_KEYBIT, BTN_THUMBL);   // L3
    ioctl(fd, UI_SET_KEYBIT, BTN_THUMBR);   // R3
    ioctl(fd, UI_SET_KEYBIT, BTN_MODE);     // PS
    // Dead button
    ioctl(fd, UI_SET_KEYBIT, BTN_DEAD);     // Dead
    // 12 trigger-happy (D-pad + rotary)
    ioctl(fd, UI_SET_KEYBIT, BTN_TRIGGER_HAPPY1);  // D-pad Up
    ioctl(fd, UI_SET_KEYBIT, BTN_TRIGGER_HAPPY2);  // D-pad Down
    ioctl(fd, UI_SET_KEYBIT, BTN_TRIGGER_HAPPY3);  // D-pad Left
    ioctl(fd, UI_SET_KEYBIT, BTN_TRIGGER_HAPPY4);  // D-pad Right
    ioctl(fd, UI_SET_KEYBIT, BTN_TRIGGER_HAPPY5);  // Red 1
    ioctl(fd, UI_SET_KEYBIT, BTN_TRIGGER_HAPPY6);  // Red 2
    ioctl(fd, UI_SET_KEYBIT, BTN_TRIGGER_HAPPY7);  // Red 3
    ioctl(fd, UI_SET_KEYBIT, BTN_TRIGGER_HAPPY8);  // Red 4
    ioctl(fd, UI_SET_KEYBIT, BTN_TRIGGER_HAPPY9);  // Red 5
    ioctl(fd, UI_SET_KEYBIT, BTN_TRIGGER_HAPPY10); // Red 6
    ioctl(fd, UI_SET_KEYBIT, BTN_TRIGGER_HAPPY11); // Rotary Left
    ioctl(fd, UI_SET_KEYBIT, BTN_TRIGGER_HAPPY12); // Rotary Right
    
    // Setup axes
    struct uinput_abs_setup abs_setup;
    
    // Steering wheel (ABS_X)
    memset(&abs_setup, 0, sizeof(abs_setup));
    abs_setup.code = ABS_X;
    abs_setup.absinfo.minimum = -32768;
    abs_setup.absinfo.maximum = 32767;
    abs_setup.absinfo.value = 0;
    abs_setup.absinfo.fuzz = 0;
    abs_setup.absinfo.flat = 0;
    ioctl(fd, UI_ABS_SETUP, &abs_setup);
    
    // Y axis (unused for G29) - Real G29 keeps this at maximum
    memset(&abs_setup, 0, sizeof(abs_setup));
    abs_setup.code = ABS_Y;
    abs_setup.absinfo.minimum = -32768;
    abs_setup.absinfo.maximum = 32767;
    abs_setup.absinfo.value = 32767;  // Match real G29
    abs_setup.absinfo.fuzz = 0;
    abs_setup.absinfo.flat = 0;
    ioctl(fd, UI_ABS_SETUP, &abs_setup);
    
    // Brake pedal (ABS_Z) - G29 pedals: 32767 at rest, -32768 when fully pressed
    memset(&abs_setup, 0, sizeof(abs_setup));
    abs_setup.code = ABS_Z;
    abs_setup.absinfo.minimum = -32768;
    abs_setup.absinfo.maximum = 32767;
    abs_setup.absinfo.value = 32767;  // At rest = maximum
    abs_setup.absinfo.fuzz = 0;
    abs_setup.absinfo.flat = 0;
    ioctl(fd, UI_ABS_SETUP, &abs_setup);
    
    // Throttle pedal (ABS_RZ) - G29 pedals: 32767 at rest, -32768 when fully pressed
    memset(&abs_setup, 0, sizeof(abs_setup));
    abs_setup.code = ABS_RZ;
    abs_setup.absinfo.minimum = -32768;
    abs_setup.absinfo.maximum = 32767;
    abs_setup.absinfo.value = 32767;  // At rest = maximum
    abs_setup.absinfo.fuzz = 0;
    abs_setup.absinfo.flat = 0;
    ioctl(fd, UI_ABS_SETUP, &abs_setup);
    
    // D-Pad X
    memset(&abs_setup, 0, sizeof(abs_setup));
    abs_setup.code = ABS_HAT0X;
    abs_setup.absinfo.minimum = -1;
    abs_setup.absinfo.maximum = 1;
    abs_setup.absinfo.value = 0;
    ioctl(fd, UI_ABS_SETUP, &abs_setup);
    
    // D-Pad Y
    memset(&abs_setup, 0, sizeof(abs_setup));
    abs_setup.code = ABS_HAT0Y;
    abs_setup.absinfo.minimum = -1;
    abs_setup.absinfo.maximum = 1;
    abs_setup.absinfo.value = 0;
    ioctl(fd, UI_ABS_SETUP, &abs_setup);
    
    // Setup device identity
    struct uinput_setup setup;
    memset(&setup, 0, sizeof(setup));
    setup.id.bustype = BUS_USB;
    setup.id.vendor = 0x046d;   // Logitech
    setup.id.product = 0xc24f;  // G29 Racing Wheel
    setup.id.version = 0x0111;  // Match real G29 version (273 decimal)
    setup.ff_effects_max = 16;  // G29 supports up to 16 simultaneous effects
    strcpy(setup.name, "Logitech G29 Driving Force Racing Wheel");
    
    ioctl(fd, UI_DEV_SETUP, &setup);
    ioctl(fd, UI_DEV_CREATE);
    
    // Wait a moment for the device to be created
    usleep(100000);
    
    // Find the event device path
    std::string event_path = "unknown";
    DIR* dir = opendir("/dev/input");
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (strncmp(entry->d_name, "event", 5) == 0) {
                std::string path = std::string("/dev/input/") + entry->d_name;
                int test_fd = open(path.c_str(), O_RDONLY);
                if (test_fd >= 0) {
                    char name[256] = "Unknown";
                    ioctl(test_fd, EVIOCGNAME(sizeof(name)), name);
                    if (strcmp(name, "Logitech G29 Driving Force Racing Wheel") == 0) {
                        event_path = path;
                        close(test_fd);
                        break;
                    }
                    close(test_fd);
                }
            }
        }
        closedir(dir);
    }
    
    std::cout << "Virtual Logitech G29 Driving Force Racing Wheel created at " << event_path << std::endl;
    return true;
}

void GamepadDevice::UpdateSteering(int delta, int sensitivity) {
    std::lock_guard<std::mutex> lock(state_mutex);
    
    // Apply small deadzone to filter out mouse jitter
    if (delta > -2 && delta < 2) {
        delta = 0;
    }
    
    // Mouse input sets user torque - scaled to match FFB force range
    // Reduced from 200x to 20x so mouse doesn't overpower FFB
    user_torque = delta * static_cast<float>(sensitivity) * 20.0f;
}

void GamepadDevice::UpdateThrottle(bool pressed) {
    std::lock_guard<std::mutex> lock(state_mutex);
    
    if (pressed) {
        throttle = (throttle + 3.0f > 100.0f) ? 100.0f : throttle + 3.0f;
    } else {
        throttle = (throttle - 3.0f < 0.0f) ? 0.0f : throttle - 3.0f;
    }
}

void GamepadDevice::UpdateBrake(bool pressed) {
    std::lock_guard<std::mutex> lock(state_mutex);
    
    if (pressed) {
        brake = (brake + 3.0f > 100.0f) ? 100.0f : brake + 3.0f;
    } else {
        brake = (brake - 3.0f < 0.0f) ? 0.0f : brake - 3.0f;
    }
}

void GamepadDevice::UpdateButtons(const Input& input) {
    std::lock_guard<std::mutex> lock(state_mutex);
    
    // Map keyboard keys to G29 buttons (26 total, see logics.md)
    // 13 base buttons
    buttons["BTN_SOUTH"] = input.IsKeyPressed(KEY_Q);      // Cross
    buttons["BTN_EAST"] = input.IsKeyPressed(KEY_E);       // Circle
    buttons["BTN_WEST"] = input.IsKeyPressed(KEY_F);       // Square
    buttons["BTN_NORTH"] = input.IsKeyPressed(KEY_G);      // Triangle
    buttons["BTN_TL"] = input.IsKeyPressed(KEY_H);         // L1
    buttons["BTN_TR"] = input.IsKeyPressed(KEY_R);         // R1
    buttons["BTN_TL2"] = input.IsKeyPressed(KEY_T);        // L2
    buttons["BTN_TR2"] = input.IsKeyPressed(KEY_Y);        // R2
    buttons["BTN_SELECT"] = input.IsKeyPressed(KEY_U);     // Share
    buttons["BTN_START"] = input.IsKeyPressed(KEY_I);      // Options
    buttons["BTN_THUMBL"] = input.IsKeyPressed(KEY_O);     // L3
    buttons["BTN_THUMBR"] = input.IsKeyPressed(KEY_P);     // R3
    buttons["BTN_MODE"] = input.IsKeyPressed(KEY_1);       // PS
    // Dead button
    buttons["BTN_DEAD"] = input.IsKeyPressed(KEY_2);       // Dead
    // 12 trigger-happy (D-pad + rotary)
    buttons["BTN_TRIGGER_HAPPY1"] = input.IsKeyPressed(KEY_3);   // D-pad Up
    buttons["BTN_TRIGGER_HAPPY2"] = input.IsKeyPressed(KEY_4);   // D-pad Down
    buttons["BTN_TRIGGER_HAPPY3"] = input.IsKeyPressed(KEY_5);   // D-pad Left
    buttons["BTN_TRIGGER_HAPPY4"] = input.IsKeyPressed(KEY_6);   // D-pad Right
    buttons["BTN_TRIGGER_HAPPY5"] = input.IsKeyPressed(KEY_7);   // Red 1
    buttons["BTN_TRIGGER_HAPPY6"] = input.IsKeyPressed(KEY_8);   // Red 2
    buttons["BTN_TRIGGER_HAPPY7"] = input.IsKeyPressed(KEY_9);   // Red 3
    buttons["BTN_TRIGGER_HAPPY8"] = input.IsKeyPressed(KEY_0);   // Red 4
    buttons["BTN_TRIGGER_HAPPY9"] = input.IsKeyPressed(KEY_LEFTSHIFT); // Red 5
    buttons["BTN_TRIGGER_HAPPY10"] = input.IsKeyPressed(KEY_SPACE);    // Red 6
    buttons["BTN_TRIGGER_HAPPY11"] = input.IsKeyPressed(KEY_TAB);      // Rotary Left
    buttons["BTN_TRIGGER_HAPPY12"] = input.IsKeyPressed(KEY_ENTER);    // Rotary Right
}

void GamepadDevice::UpdateDPad(const Input& input) {
    std::lock_guard<std::mutex> lock(state_mutex);
    
    int right = input.IsKeyPressed(KEY_RIGHT) ? 1 : 0;
    int left = input.IsKeyPressed(KEY_LEFT) ? 1 : 0;
    int down = input.IsKeyPressed(KEY_DOWN) ? 1 : 0;
    int up = input.IsKeyPressed(KEY_UP) ? 1 : 0;
    
    dpad_x = right - left;
    dpad_y = down - up;
}

void GamepadDevice::SendState() {
    if (fd < 0) return;
    
    if (use_uhid) {
        SendUHIDReport();
        return;
    }
    
    // Lock state mutex to prevent race with FFB thread
    std::lock_guard<std::mutex> lock(state_mutex);
    
    // UInput path (legacy)
    // Send steering wheel position - convert float to int16_t
    EmitEvent(EV_ABS, ABS_X, static_cast<int16_t>(steering));
    
    // Send clutch (ABS_Y)
    int16_t clutch_val = 32767 - static_cast<int16_t>(clutch * 655.35f);
    EmitEvent(EV_ABS, ABS_Y, clutch_val);
    
    // Send throttle and brake as pedal axes (G29 standard)
    // Real G29 pedals are inverted: 32767 at rest, -32768 when fully pressed
    int16_t throttle_val = 32767 - static_cast<int16_t>(throttle * 655.35f);
    int16_t brake_val = 32767 - static_cast<int16_t>(brake * 655.35f);
    
    EmitEvent(EV_ABS, ABS_Z, brake_val);    // Brake pedal
    EmitEvent(EV_ABS, ABS_RZ, throttle_val); // Throttle pedal
    
    // Send all 26 G29 buttons
    EmitEvent(EV_KEY, BTN_SOUTH, buttons["BTN_SOUTH"] ? 1 : 0);
    EmitEvent(EV_KEY, BTN_EAST, buttons["BTN_EAST"] ? 1 : 0);
    EmitEvent(EV_KEY, BTN_WEST, buttons["BTN_WEST"] ? 1 : 0);
    EmitEvent(EV_KEY, BTN_NORTH, buttons["BTN_NORTH"] ? 1 : 0);
    EmitEvent(EV_KEY, BTN_TL, buttons["BTN_TL"] ? 1 : 0);
    EmitEvent(EV_KEY, BTN_TR, buttons["BTN_TR"] ? 1 : 0);
    EmitEvent(EV_KEY, BTN_TL2, buttons["BTN_TL2"] ? 1 : 0);
    EmitEvent(EV_KEY, BTN_TR2, buttons["BTN_TR2"] ? 1 : 0);
    EmitEvent(EV_KEY, BTN_SELECT, buttons["BTN_SELECT"] ? 1 : 0);
    EmitEvent(EV_KEY, BTN_START, buttons["BTN_START"] ? 1 : 0);
    EmitEvent(EV_KEY, BTN_THUMBL, buttons["BTN_THUMBL"] ? 1 : 0);
    EmitEvent(EV_KEY, BTN_THUMBR, buttons["BTN_THUMBR"] ? 1 : 0);
    EmitEvent(EV_KEY, BTN_MODE, buttons["BTN_MODE"] ? 1 : 0);
    EmitEvent(EV_KEY, BTN_DEAD, buttons["BTN_DEAD"] ? 1 : 0);
    EmitEvent(EV_KEY, BTN_TRIGGER_HAPPY1, buttons["BTN_TRIGGER_HAPPY1"] ? 1 : 0);
    EmitEvent(EV_KEY, BTN_TRIGGER_HAPPY2, buttons["BTN_TRIGGER_HAPPY2"] ? 1 : 0);
    EmitEvent(EV_KEY, BTN_TRIGGER_HAPPY3, buttons["BTN_TRIGGER_HAPPY3"] ? 1 : 0);
    EmitEvent(EV_KEY, BTN_TRIGGER_HAPPY4, buttons["BTN_TRIGGER_HAPPY4"] ? 1 : 0);
    EmitEvent(EV_KEY, BTN_TRIGGER_HAPPY5, buttons["BTN_TRIGGER_HAPPY5"] ? 1 : 0);
    EmitEvent(EV_KEY, BTN_TRIGGER_HAPPY6, buttons["BTN_TRIGGER_HAPPY6"] ? 1 : 0);
    EmitEvent(EV_KEY, BTN_TRIGGER_HAPPY7, buttons["BTN_TRIGGER_HAPPY7"] ? 1 : 0);
    EmitEvent(EV_KEY, BTN_TRIGGER_HAPPY8, buttons["BTN_TRIGGER_HAPPY8"] ? 1 : 0);
    EmitEvent(EV_KEY, BTN_TRIGGER_HAPPY9, buttons["BTN_TRIGGER_HAPPY9"] ? 1 : 0);
    EmitEvent(EV_KEY, BTN_TRIGGER_HAPPY10, buttons["BTN_TRIGGER_HAPPY10"] ? 1 : 0);
    EmitEvent(EV_KEY, BTN_TRIGGER_HAPPY11, buttons["BTN_TRIGGER_HAPPY11"] ? 1 : 0);
    EmitEvent(EV_KEY, BTN_TRIGGER_HAPPY12, buttons["BTN_TRIGGER_HAPPY12"] ? 1 : 0);
    
    // Send D-Pad
    EmitEvent(EV_ABS, ABS_HAT0X, dpad_x);
    EmitEvent(EV_ABS, ABS_HAT0Y, dpad_y);
    
    // Sync
    EmitEvent(EV_SYN, SYN_REPORT, 0);
}

std::vector<uint8_t> GamepadDevice::BuildHIDReport() {
    // G29 HID Report structure (13 bytes total as defined by descriptor):
    // Byte 0-1: X (Steering) - 16-bit, little endian, 0-65535, center=32768
    // Byte 2-3: Y (Unused) - 16-bit, constant 65535
    // Byte 4-5: Z (Brake) - 16-bit, little endian, inverted: 65535=rest, 0=pressed  
    // Byte 6-7: Rz (Throttle) - 16-bit, little endian, inverted: 65535=rest, 0=pressed
    // Byte 8: HAT switch (4 bits) + padding (4 bits)
    // Byte 9-12: Buttons (25 bits) + padding (7 bits)
    
    // Lock state mutex to prevent race with FFB thread
    std::lock_guard<std::mutex> lock(state_mutex);
    
    std::vector<uint8_t> report(13, 0);
    
    // X axis: Steering - convert from -32768..32767 to 0..65535
    uint16_t steering_u = static_cast<uint16_t>(static_cast<int16_t>(steering) + 32768);
    report[0] = steering_u & 0xFF;
    report[1] = (steering_u >> 8) & 0xFF;
    
    // Y axis: Clutch - inverted, 0-100% -> 65535-0
    uint16_t clutch_u = 65535 - static_cast<uint16_t>(clutch * 655.35f);
    report[2] = clutch_u & 0xFF;
    report[3] = (clutch_u >> 8) & 0xFF;
    
    // Z axis: Brake - inverted, 0-100% -> 65535-0
    uint16_t brake_u = 65535 - static_cast<uint16_t>(brake * 655.35f);
    report[4] = brake_u & 0xFF;
    report[5] = (brake_u >> 8) & 0xFF;
    
    // Rz axis: Throttle - inverted, 0-100% -> 65535-0
    uint16_t throttle_u = 65535 - static_cast<uint16_t>(throttle * 655.35f);
    report[6] = throttle_u & 0xFF;
    report[7] = (throttle_u >> 8) & 0xFF;
    
    // HAT switch (D-Pad): convert from X/Y to 8-direction value
    uint8_t hat = 0x0F; // Neutral
    if (dpad_y == -1 && dpad_x == 0) hat = 0; // Up
    else if (dpad_y == -1 && dpad_x == 1) hat = 1; // Up-Right
    else if (dpad_y == 0 && dpad_x == 1) hat = 2; // Right
    else if (dpad_y == 1 && dpad_x == 1) hat = 3; // Down-Right
    else if (dpad_y == 1 && dpad_x == 0) hat = 4; // Down
    else if (dpad_y == 1 && dpad_x == -1) hat = 5; // Down-Left
    else if (dpad_y == 0 && dpad_x == -1) hat = 6; // Left
    else if (dpad_y == -1 && dpad_x == -1) hat = 7; // Up-Left
    
    report[8] = hat & 0x0F;
    
    // Buttons: Pack 26 buttons into 4 bytes (32 bits, use 26)
    uint32_t button_bits = 0;
    if (buttons["BTN_SOUTH"]) button_bits |= (1 << 0);
    if (buttons["BTN_EAST"]) button_bits |= (1 << 1);
    if (buttons["BTN_WEST"]) button_bits |= (1 << 2);
    if (buttons["BTN_NORTH"]) button_bits |= (1 << 3);
    if (buttons["BTN_TL"]) button_bits |= (1 << 4);
    if (buttons["BTN_TR"]) button_bits |= (1 << 5);
    if (buttons["BTN_TL2"]) button_bits |= (1 << 6);
    if (buttons["BTN_TR2"]) button_bits |= (1 << 7);
    if (buttons["BTN_SELECT"]) button_bits |= (1 << 8);
    if (buttons["BTN_START"]) button_bits |= (1 << 9);
    if (buttons["BTN_THUMBL"]) button_bits |= (1 << 10);
    if (buttons["BTN_THUMBR"]) button_bits |= (1 << 11);
    if (buttons["BTN_MODE"]) button_bits |= (1 << 12);
    if (buttons["BTN_DEAD"]) button_bits |= (1 << 13);
    if (buttons["BTN_TRIGGER_HAPPY1"]) button_bits |= (1 << 14);
    if (buttons["BTN_TRIGGER_HAPPY2"]) button_bits |= (1 << 15);
    if (buttons["BTN_TRIGGER_HAPPY3"]) button_bits |= (1 << 16);
    if (buttons["BTN_TRIGGER_HAPPY4"]) button_bits |= (1 << 17);
    if (buttons["BTN_TRIGGER_HAPPY5"]) button_bits |= (1 << 18);
    if (buttons["BTN_TRIGGER_HAPPY6"]) button_bits |= (1 << 19);
    if (buttons["BTN_TRIGGER_HAPPY7"]) button_bits |= (1 << 20);
    if (buttons["BTN_TRIGGER_HAPPY8"]) button_bits |= (1 << 21);
    if (buttons["BTN_TRIGGER_HAPPY9"]) button_bits |= (1 << 22);
    if (buttons["BTN_TRIGGER_HAPPY10"]) button_bits |= (1 << 23);
    if (buttons["BTN_TRIGGER_HAPPY11"]) button_bits |= (1 << 24);
    if (buttons["BTN_TRIGGER_HAPPY12"]) button_bits |= (1 << 25);
    report[9] = button_bits & 0xFF;
    report[10] = (button_bits >> 8) & 0xFF;
    report[11] = (button_bits >> 16) & 0xFF;
    report[12] = (button_bits >> 24) & 0xFF;
    
    return report;
}

void GamepadDevice::SendUHIDReport() {
    std::vector<uint8_t> report_data = BuildHIDReport();
    
    if (use_gadget) {
        // USB Gadget: Write raw HID report directly
        if (write(fd, report_data.data(), report_data.size()) < 0) {
            // Silently ignore write errors
        }
    } else {
        // UHID: Wrap in uhid_event structure
        struct uhid_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = UHID_INPUT2;
        ev.u.input2.size = report_data.size();
        memcpy(ev.u.input2.data, report_data.data(), report_data.size());
        
        if (write(fd, &ev, sizeof(ev)) < 0) {
            // Silently ignore write errors (device might be disconnected)
        }
    }
}

void GamepadDevice::SendNeutral() {
    if (fd < 0) return;
    
    // Reset steering to center
    steering = 0;
    throttle = 0;
    brake = 0;
    
    // Zero all axes (center steering wheel)
    EmitEvent(EV_ABS, ABS_X, 0);
    EmitEvent(EV_ABS, ABS_Y, 32767);  // Match real G29
    
    // Reset pedals to resting position (inverted: 32767 = not pressed)
    EmitEvent(EV_ABS, ABS_Z, 32767);
    EmitEvent(EV_ABS, ABS_RZ, 32767);
    
    // Zero all 25 buttons
    EmitEvent(EV_KEY, BTN_TRIGGER, 0);
    EmitEvent(EV_KEY, BTN_THUMB, 0);
    EmitEvent(EV_KEY, BTN_THUMB2, 0);
    EmitEvent(EV_KEY, BTN_TOP, 0);
    EmitEvent(EV_KEY, BTN_TOP2, 0);
    EmitEvent(EV_KEY, BTN_PINKIE, 0);
    EmitEvent(EV_KEY, BTN_BASE, 0);
    EmitEvent(EV_KEY, BTN_BASE2, 0);
    EmitEvent(EV_KEY, BTN_BASE3, 0);
    EmitEvent(EV_KEY, BTN_BASE4, 0);
    EmitEvent(EV_KEY, BTN_BASE5, 0);
    EmitEvent(EV_KEY, BTN_BASE6, 0);
    EmitEvent(EV_KEY, BTN_DEAD, 0);
    EmitEvent(EV_KEY, BTN_TRIGGER_HAPPY1, 0);
    EmitEvent(EV_KEY, BTN_TRIGGER_HAPPY2, 0);
    EmitEvent(EV_KEY, BTN_TRIGGER_HAPPY3, 0);
    EmitEvent(EV_KEY, BTN_TRIGGER_HAPPY4, 0);
    EmitEvent(EV_KEY, BTN_TRIGGER_HAPPY5, 0);
    EmitEvent(EV_KEY, BTN_TRIGGER_HAPPY6, 0);
    EmitEvent(EV_KEY, BTN_TRIGGER_HAPPY7, 0);
    EmitEvent(EV_KEY, BTN_TRIGGER_HAPPY8, 0);
    EmitEvent(EV_KEY, BTN_TRIGGER_HAPPY9, 0);
    EmitEvent(EV_KEY, BTN_TRIGGER_HAPPY10, 0);
    EmitEvent(EV_KEY, BTN_TRIGGER_HAPPY11, 0);
    EmitEvent(EV_KEY, BTN_TRIGGER_HAPPY12, 0);
    
    // Zero D-Pad
    EmitEvent(EV_ABS, ABS_HAT0X, 0);
    EmitEvent(EV_ABS, ABS_HAT0Y, 0);
    EmitEvent(EV_SYN, SYN_REPORT, 0);
}

void GamepadDevice::EmitEvent(uint16_t type, uint16_t code, int32_t value) {
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.code = code;
    ev.value = value;
    
    write(fd, &ev, sizeof(ev));
}

int16_t GamepadDevice::ClampSteering(int16_t value) {
    // int16_t is already in range [-32768, 32767], no clamping needed
    return value;
}

void GamepadDevice::ProcessUHIDEvents() {
    if (use_gadget) {
        // USB Gadget doesn't need event processing
        return;
    }
    
    if (!use_uhid || fd < 0) return;
    
    struct uhid_event ev;
    ssize_t ret;
    
    // Process all pending UHID events (non-blocking)
    while ((ret = read(fd, &ev, sizeof(ev))) > 0) {
        switch (ev.type) {
            case UHID_START:
                // Device started, ready to receive input
                break;
                
            case UHID_STOP:
                // Device stopped
                break;
                
            case UHID_OPEN:
                // Application opened the device
                break;
                
            case UHID_CLOSE:
                // Application closed the device
                break;
                
            case UHID_OUTPUT:
                // Force Feedback output from game
                // ev.u.output contains FFB data (7 bytes for G29)
                if (ev.u.output.size == 7) {
                    ParseFFBCommand(ev.u.output.data, ev.u.output.size);
                }
                break;
                
            case UHID_GET_REPORT:
                // Game requests current state
                // Send back current HID report
                {
                    struct uhid_event reply;
                    memset(&reply, 0, sizeof(reply));
                    reply.type = UHID_GET_REPORT_REPLY;
                    reply.u.get_report_reply.id = ev.u.get_report.id;
                    reply.u.get_report_reply.err = 0;
                    
                    std::vector<uint8_t> report = BuildHIDReport();
                    reply.u.get_report_reply.size = report.size();
                    memcpy(reply.u.get_report_reply.data, report.data(), report.size());
                    
                    write(fd, &reply, sizeof(reply));
                }
                break;
                
            case UHID_SET_REPORT:
                // Game sets report (e.g., FFB commands)
                // Parse FFB if it's OUTPUT report
                if (ev.u.set_report.rtype == UHID_OUTPUT_REPORT && ev.u.set_report.size == 7) {
                    ParseFFBCommand(ev.u.set_report.data, ev.u.set_report.size);
                }
                // Send acknowledgment
                {
                    struct uhid_event reply;
                    memset(&reply, 0, sizeof(reply));
                    reply.type = UHID_SET_REPORT_REPLY;
                    reply.u.set_report_reply.id = ev.u.set_report.id;
                    reply.u.set_report_reply.err = 0;
                    write(fd, &reply, sizeof(reply));
                }
                break;
                
            default:
                // Unknown event type
                break;
        }
    }
}

void GamepadDevice::USBGadgetPollingThread() {
    // USB Gadget bidirectional communication:
    // - Write INPUT reports (joystick state) when host polls
    // - Read OUTPUT reports (FFB commands) when host sends them
    
    std::cout << "USB Gadget polling thread started" << std::endl;
    
    // Use non-blocking I/O with poll() for bidirectional communication
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN | POLLOUT;  // Monitor both read and write
    
    uint8_t ffb_buffer[7];  // G29 FFB commands are 7 bytes
    
    while (gadget_running) {
        // Wait for either read or write ready (with timeout)
        int ret = poll(&pfd, 1, 100);  // 100ms timeout
        
        if (ret < 0) {
            if (errno == EINTR) continue;
            std::cerr << "USB Gadget poll error: " << strerror(errno) << std::endl;
            break;
        }
        
        if (ret == 0) {
            // Timeout - continue loop
            continue;
        }
        
        // Check for FFB OUTPUT reports from host
        if (pfd.revents & POLLIN) {
            ssize_t bytes = read(fd, ffb_buffer, sizeof(ffb_buffer));
            if (bytes == 7) {
                // Valid FFB command received
                ParseFFBCommand(ffb_buffer, bytes);
            } else if (bytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                std::cerr << "USB Gadget read error: " << strerror(errno) << std::endl;
            }
        }
        
        // Send INPUT report when host is ready to receive
        if (pfd.revents & POLLOUT) {
            std::vector<uint8_t> report;
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                report = BuildHIDReport();
            }
            
            ssize_t bytes = write(fd, report.data(), report.size());
            if (bytes < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    if (errno == ESHUTDOWN || errno == ECONNRESET) {
                        std::cout << "USB Gadget device disconnected" << std::endl;
                        break;
                    }
                    std::cerr << "USB Gadget write error: " << strerror(errno) << std::endl;
                }
            }
        }
        
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            std::cerr << "USB Gadget poll error flags" << std::endl;
            break;
        }
    }
    
    std::cout << "USB Gadget polling thread stopped" << std::endl;
}

void GamepadDevice::FFBUpdateThread() {
    // Physics simulation: steering is determined by forces
    // - FFB force from game (road feedback, tire grip, etc)
    // - User torque from mouse (human turning the wheel)
    // - Autocenter spring (wheel wants to return to center)
    
    std::cout << "FFB update thread started" << std::endl;
    
    float velocity = 0.0f;  // Current wheel rotation speed
    
    while (ffb_running && running) {
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            
            // Calculate total torque acting on wheel
            float total_torque = 0.0f;
            
            // Add FFB force from game
            total_torque += static_cast<float>(ffb_force);
            
            // Add user input torque (mouse)
            total_torque += user_torque;
            
            // Add autocenter spring force
            if (ffb_autocenter > 0) {
                float spring = -(steering * static_cast<float>(ffb_autocenter)) / 32768.0f;
                total_torque += spring;
            }
            
            // Torque changes velocity (F=ma)
            velocity += total_torque * 0.001f;  // Acceleration from torque
            
            // Damping/friction slows wheel down - reduced from 0.92 to 0.98 for smoother feel
            velocity *= 0.98f;
            
            // Velocity changes position
            steering += velocity;
            
            // Clamp to limits
            if (steering < -32768.0f) {
                steering = -32768.0f;
                velocity = 0.0f;
            }
            if (steering > 32767.0f) {
                steering = 32767.0f;
                velocity = 0.0f;
            }
        }
        
        // Update at 125Hz to match main loop (8ms per frame)
        usleep(8000);
    }
    
    std::cout << "FFB update thread stopped" << std::endl;
}

void GamepadDevice::ParseFFBCommand(const uint8_t* data, size_t size) {
    if (size != 7) return;  // G29 FFB commands are always 7 bytes
    
    std::lock_guard<std::mutex> lock(state_mutex);
    
    uint8_t cmd = data[0];
    
    // Debug: Print FFB commands
    std::cout << "FFB: " << std::hex;
    for (size_t i = 0; i < size; i++) {
        std::cout << (int)data[i] << " ";
    }
    std::cout << std::dec << std::endl;
    
    // Logitech G29 FFB protocol (based on hid-lg4ff.c kernel driver)
    switch (cmd) {
        case 0x11:  // Constant force effect (slot 1)
            // data[1] = 0x08 (effect type)
            // data[2] = force magnitude (0x00-0xFF, 0x80 = no force)
            // data[3] = direction? (0x80 = center)
            {
                // Convert 0x00-0xFF range to -128..127 force value
                // DO NOT SCALE - apply force exactly as game sends it
                int8_t force = static_cast<int8_t>(data[2]) - 0x80;
                ffb_force = force * 256;  // Convert to int16_t range
                std::cout << "FFB Constant Force: " << (int)ffb_force << std::endl;
            }
            break;
            
        case 0x13:  // Stop effect / de-activate force
            ffb_force = 0;
            std::cout << "FFB Stop" << std::endl;
            break;
            
        case 0xf5:  // Disable autocenter
            ffb_autocenter = 0;
            std::cout << "FFB Autocenter OFF" << std::endl;
            break;
            
        case 0xfe:  // Set autocenter parameters
            // data[1] = 0x0d
            // data[2], data[3] = autocenter strength
            // data[4] = spring rate
            if (data[1] == 0x0d) {
                ffb_autocenter = static_cast<int16_t>(data[2]) * 128;  // Scale to usable range
                std::cout << "FFB Autocenter Params: " << ffb_autocenter << std::endl;
            }
            break;
            
        case 0x14:  // Activate autocenter
            if (ffb_autocenter == 0) {
                ffb_autocenter = 4096;  // Default moderate autocenter
            }
            std::cout << "FFB Autocenter ON: " << ffb_autocenter << std::endl;
            break;
            
        case 0xf8:  // Extended commands (range, mode switching, LEDs, etc.)
            // data[1] = subcommand
            switch (data[1]) {
                case 0x81:  // Set wheel range
                    // data[2] = low byte, data[3] = high byte
                    // Range in degrees (40-900 for G29)
                    // We don't need to change anything - just acknowledge
                    break;
                case 0x12:  // Set LEDs
                    // data[2] = LED bitmask (5 LEDs on G29)
                    // Ignored for mouse/keyboard emulator
                    break;
                case 0x09:  // Mode switch (compatibility mode)
                    // Ignored - we're always in G29 native mode
                    break;
                case 0x0a:  // Mode revert on USB reset
                    // Ignored
                    break;
                default:
                    break;
            }
            break;
            
        default:
            // Unknown command - ignore
            break;
    }
}
