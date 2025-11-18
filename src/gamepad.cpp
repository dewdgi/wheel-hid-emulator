#include "gamepad.h"
#include "input.h"
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <dirent.h>
#include <cstdlib>
#include <linux/uhid.h>
#include <linux/uinput.h>
#include <linux/input-event-codes.h>

GamepadDevice::GamepadDevice() 
    : fd(-1), use_uhid(false), use_gadget(false), gadget_running(false), steering(0), throttle(0.0f), brake(0.0f), dpad_x(0), dpad_y(0) {
}

GamepadDevice::~GamepadDevice() {
    // Stop USB Gadget polling thread if running
    if (gadget_running) {
        gadget_running = false;
        if (gadget_thread.joinable()) {
            gadget_thread.join();
        }
    }
    
    if (fd >= 0) {
        if (use_uhid) {
            struct uhid_event ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = UHID_DESTROY;
            write(fd, &ev, sizeof(ev));
        } else {
            ioctl(fd, UI_DEV_DESTROY);
        }
        close(fd);
    }
}

// Logitech G29 HID Report Descriptor
// Based on real G29 wheel descriptor with proper OUTPUT report for hid-lg driver
// The kernel driver expects OUTPUT report with no report ID (report 0) for FFB commands
static const uint8_t g29_hid_descriptor[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x04,        // Usage (Joystick)
    0xA1, 0x01,        // Collection (Application)
    
    // First collection: Input controls
    0xA1, 0x02,        //   Collection (Logical)
    
    // Steering wheel axis
    0x09, 0x01,        //     Usage (Pointer)
    0xA1, 0x00,        //     Collection (Physical)
    0x09, 0x30,        //       Usage (X)
    0x15, 0x00,        //       Logical Minimum (0)
    0x27, 0xFF, 0xFF, 0x00, 0x00,  //       Logical Maximum (65535)
    0x35, 0x00,        //       Physical Minimum (0)
    0x47, 0xFF, 0xFF, 0x00, 0x00,  //       Physical Maximum (65535)
    0x75, 0x10,        //       Report Size (16)
    0x95, 0x01,        //       Report Count (1)
    0x81, 0x02,        //       Input (Data,Var,Abs)
    0xC0,              //     End Collection
    
    // Pedals (3 axes: throttle, brake, clutch)
    0x09, 0x01,        //     Usage (Pointer)
    0xA1, 0x00,        //     Collection (Physical)
    0x09, 0x33,        //       Usage (Rx) - Throttle
    0x09, 0x34,        //       Usage (Ry) - Brake  
    0x09, 0x35,        //       Usage (Rz) - Clutch
    0x15, 0x00,        //       Logical Minimum (0)
    0x27, 0xFF, 0xFF, 0x00, 0x00,  //       Logical Maximum (65535)
    0x35, 0x00,        //       Physical Minimum (0)
    0x47, 0xFF, 0xFF, 0x00, 0x00,  //       Physical Maximum (65535)
    0x75, 0x10,        //       Report Size (16)
    0x95, 0x03,        //       Report Count (3)
    0x81, 0x02,        //       Input (Data,Var,Abs)
    0xC0,              //     End Collection
    
    // HAT switch (D-Pad)
    0x09, 0x39,        //     Usage (Hat switch)
    0x15, 0x00,        //     Logical Minimum (0)
    0x25, 0x07,        //     Logical Maximum (7)
    0x35, 0x00,        //     Physical Minimum (0)
    0x46, 0x3B, 0x01,  //     Physical Maximum (315)
    0x65, 0x14,        //     Unit (Degrees)
    0x75, 0x04,        //     Report Size (4)
    0x95, 0x01,        //     Report Count (1)
    0x81, 0x42,        //     Input (Data,Var,Abs,Null)
    
    // Padding
    0x75, 0x04,        //     Report Size (4)
    0x95, 0x01,        //     Report Count (1)
    0x81, 0x03,        //     Input (Const,Var,Abs)
    
    // Buttons (25 buttons)
    0x05, 0x09,        //     Usage Page (Button)
    0x19, 0x01,        //     Usage Minimum (Button 1)
    0x29, 0x19,        //     Usage Maximum (Button 25)
    0x15, 0x00,        //     Logical Minimum (0)
    0x25, 0x01,        //     Logical Maximum (1)
    0x75, 0x01,        //     Report Size (1)
    0x95, 0x19,        //     Report Count (25)
    0x81, 0x02,        //     Input (Data,Var,Abs)
    
    // Padding to byte boundary (7 bits)
    0x75, 0x07,        //     Report Size (7)
    0x95, 0x01,        //     Report Count (1)
    0x81, 0x03,        //     Input (Const,Var,Abs)
    
    0xC0,              //   End Collection (Logical)
    
    // Second collection: OUTPUT report for Force Feedback
    // CRITICAL: This must be present for hid-lg driver to bind successfully
    // The driver checks for HID_OUTPUT_REPORT 0 (no report ID) with 7 bytes
    0xA1, 0x02,        //   Collection (Logical)
    0x09, 0x02,        //     Usage (0x02) - FFB usage
    0x15, 0x00,        //     Logical Minimum (0)
    0x26, 0xFF, 0x00,  //     Logical Maximum (255)
    0x95, 0x07,        //     Report Count (7) - REQUIRED: hid-lg expects 7 bytes
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
    
    // Check if already set up
    if (access(hidg_dev, F_OK) == 0) {
        fd = open(hidg_dev, O_RDWR | O_NONBLOCK);
        if (fd >= 0) {
            std::cout << "USB Gadget device already configured and opened" << std::endl;
            std::cout << "Real USB Logitech G29 device (VID:046d PID:c24f)" << std::endl;
            use_gadget = true;
            use_uhid = true;
            return true;
        }
    }
    
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
    
    // Create gadget directory structure
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
          "echo 1 > protocol && echo 1 > subclass && echo 16 > report_length && "
          "echo 1 > no_out_endpoint && "
          "printf '\\x05\\x01\\x09\\x04\\xa1\\x01\\x09\\x01\\xa1\\x00\\x09\\x30\\x15\\x00\\x27\\xff\\xff\\x00\\x00\\x35\\x00\\x47\\xff\\xff\\x00\\x00\\x75\\x10\\x95\\x01\\x81\\x02\\xc0\\x09\\x01\\xa1\\x00\\x09\\x33\\x09\\x34\\x09\\x35\\x15\\x00\\x27\\xff\\xff\\x00\\x00\\x35\\x00\\x47\\xff\\xff\\x00\\x00\\x75\\x10\\x95\\x03\\x81\\x02\\xc0\\x09\\x39\\x15\\x00\\x25\\x07\\x35\\x00\\x46\\x3b\\x01\\x65\\x14\\x75\\x04\\x95\\x01\\x81\\x42\\x75\\x04\\x95\\x01\\x81\\x03\\x05\\x09\\x19\\x01\\x29\\x19\\x15\\x00\\x25\\x01\\x75\\x01\\x95\\x19\\x81\\x02\\x75\\x07\\x95\\x01\\x81\\x03\\xc0' > report_desc && "
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
    // First 10 buttons using standard joystick codes
    ioctl(fd, UI_SET_KEYBIT, BTN_TRIGGER);  // Button 1
    ioctl(fd, UI_SET_KEYBIT, BTN_THUMB);    // Button 2
    ioctl(fd, UI_SET_KEYBIT, BTN_THUMB2);   // Button 3
    ioctl(fd, UI_SET_KEYBIT, BTN_TOP);      // Button 4
    ioctl(fd, UI_SET_KEYBIT, BTN_TOP2);     // Button 5
    ioctl(fd, UI_SET_KEYBIT, BTN_PINKIE);   // Button 6
    ioctl(fd, UI_SET_KEYBIT, BTN_BASE);     // Button 7
    ioctl(fd, UI_SET_KEYBIT, BTN_BASE2);    // Button 8
    ioctl(fd, UI_SET_KEYBIT, BTN_BASE3);    // Button 9
    ioctl(fd, UI_SET_KEYBIT, BTN_BASE4);    // Button 10
    
    // Additional buttons
    ioctl(fd, UI_SET_KEYBIT, BTN_BASE5);    // Button 11
    ioctl(fd, UI_SET_KEYBIT, BTN_BASE6);    // Button 12
    ioctl(fd, UI_SET_KEYBIT, BTN_DEAD);     // Button 13
    
    // Extra buttons using BTN_TRIGGER_HAPPY range for total 25
    ioctl(fd, UI_SET_KEYBIT, BTN_TRIGGER_HAPPY1);  // Button 14
    ioctl(fd, UI_SET_KEYBIT, BTN_TRIGGER_HAPPY2);  // Button 15
    ioctl(fd, UI_SET_KEYBIT, BTN_TRIGGER_HAPPY3);  // Button 16
    ioctl(fd, UI_SET_KEYBIT, BTN_TRIGGER_HAPPY4);  // Button 17
    ioctl(fd, UI_SET_KEYBIT, BTN_TRIGGER_HAPPY5);  // Button 18
    ioctl(fd, UI_SET_KEYBIT, BTN_TRIGGER_HAPPY6);  // Button 19
    ioctl(fd, UI_SET_KEYBIT, BTN_TRIGGER_HAPPY7);  // Button 20
    ioctl(fd, UI_SET_KEYBIT, BTN_TRIGGER_HAPPY8);  // Button 21
    ioctl(fd, UI_SET_KEYBIT, BTN_TRIGGER_HAPPY9);  // Button 22
    ioctl(fd, UI_SET_KEYBIT, BTN_TRIGGER_HAPPY10); // Button 23
    ioctl(fd, UI_SET_KEYBIT, BTN_TRIGGER_HAPPY11); // Button 24
    ioctl(fd, UI_SET_KEYBIT, BTN_TRIGGER_HAPPY12); // Button 25
    
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
    
    // Pure linear steering: each pixel of mouse movement adds to steering
    // Using sensitivity/5 for better feel (e.g., sensitivity=20 -> 4 units per pixel)
    // At sensitivity=20: 4 units per pixel, full lock at 32768/4 = 8192 pixels (~8cm at 1000 DPI)
    // At sensitivity=50: 10 units per pixel, full lock at 32768/10 = 3276 pixels (~3cm at 1000 DPI)
    steering += delta * static_cast<float>(sensitivity) * 0.2f;
    
    // Clamp to int16_t range
    if (steering < -32768.0f) steering = -32768.0f;
    if (steering > 32767.0f) steering = 32767.0f;
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
    
    // Map keyboard keys to wheel buttons (all 25 buttons for G29)
    // Primary buttons (1-12)
    buttons["BTN_TRIGGER"] = input.IsKeyPressed(KEY_Q);
    buttons["BTN_THUMB"] = input.IsKeyPressed(KEY_E);
    buttons["BTN_THUMB2"] = input.IsKeyPressed(KEY_F);
    buttons["BTN_TOP"] = input.IsKeyPressed(KEY_G);
    buttons["BTN_TOP2"] = input.IsKeyPressed(KEY_H);
    buttons["BTN_PINKIE"] = input.IsKeyPressed(KEY_R);
    buttons["BTN_BASE"] = input.IsKeyPressed(KEY_T);
    buttons["BTN_BASE2"] = input.IsKeyPressed(KEY_Y);
    buttons["BTN_BASE3"] = input.IsKeyPressed(KEY_U);
    buttons["BTN_BASE4"] = input.IsKeyPressed(KEY_I);
    buttons["BTN_BASE5"] = input.IsKeyPressed(KEY_O);
    buttons["BTN_BASE6"] = input.IsKeyPressed(KEY_P);
    
    // Additional buttons (13-25)
    buttons["BTN_DEAD"] = input.IsKeyPressed(KEY_1);
    buttons["BTN_TRIGGER_HAPPY1"] = input.IsKeyPressed(KEY_2);
    buttons["BTN_TRIGGER_HAPPY2"] = input.IsKeyPressed(KEY_3);
    buttons["BTN_TRIGGER_HAPPY3"] = input.IsKeyPressed(KEY_4);
    buttons["BTN_TRIGGER_HAPPY4"] = input.IsKeyPressed(KEY_5);
    buttons["BTN_TRIGGER_HAPPY5"] = input.IsKeyPressed(KEY_6);
    buttons["BTN_TRIGGER_HAPPY6"] = input.IsKeyPressed(KEY_7);
    buttons["BTN_TRIGGER_HAPPY7"] = input.IsKeyPressed(KEY_8);
    buttons["BTN_TRIGGER_HAPPY8"] = input.IsKeyPressed(KEY_9);
    buttons["BTN_TRIGGER_HAPPY9"] = input.IsKeyPressed(KEY_0);
    buttons["BTN_TRIGGER_HAPPY10"] = input.IsKeyPressed(KEY_LEFTSHIFT);
    buttons["BTN_TRIGGER_HAPPY11"] = input.IsKeyPressed(KEY_SPACE);
    buttons["BTN_TRIGGER_HAPPY12"] = input.IsKeyPressed(KEY_TAB);
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
    
    // UInput path (legacy)
    // Send steering wheel position - convert float to int16_t
    EmitEvent(EV_ABS, ABS_X, static_cast<int16_t>(steering));
    
    // Send Y axis (unused for G29, always at maximum like real wheel)
    EmitEvent(EV_ABS, ABS_Y, 32767);
    
    // Send throttle and brake as pedal axes (G29 standard)
    // Real G29 pedals are inverted: 32767 at rest, -32768 when fully pressed
    int16_t throttle_val = 32767 - static_cast<int16_t>(throttle * 655.35f);
    int16_t brake_val = 32767 - static_cast<int16_t>(brake * 655.35f);
    
    EmitEvent(EV_ABS, ABS_Z, brake_val);    // Brake pedal
    EmitEvent(EV_ABS, ABS_RZ, throttle_val); // Throttle pedal
    
    // Send wheel buttons (joystick style - all 25 buttons to match real G29)
    EmitEvent(EV_KEY, BTN_TRIGGER, buttons["BTN_TRIGGER"] ? 1 : 0);
    EmitEvent(EV_KEY, BTN_THUMB, buttons["BTN_THUMB"] ? 1 : 0);
    EmitEvent(EV_KEY, BTN_THUMB2, buttons["BTN_THUMB2"] ? 1 : 0);
    EmitEvent(EV_KEY, BTN_TOP, buttons["BTN_TOP"] ? 1 : 0);
    EmitEvent(EV_KEY, BTN_TOP2, buttons["BTN_TOP2"] ? 1 : 0);
    EmitEvent(EV_KEY, BTN_PINKIE, buttons["BTN_PINKIE"] ? 1 : 0);
    EmitEvent(EV_KEY, BTN_BASE, buttons["BTN_BASE"] ? 1 : 0);
    EmitEvent(EV_KEY, BTN_BASE2, buttons["BTN_BASE2"] ? 1 : 0);
    EmitEvent(EV_KEY, BTN_BASE3, buttons["BTN_BASE3"] ? 1 : 0);
    EmitEvent(EV_KEY, BTN_BASE4, buttons["BTN_BASE4"] ? 1 : 0);
    EmitEvent(EV_KEY, BTN_BASE5, buttons["BTN_BASE5"] ? 1 : 0);
    EmitEvent(EV_KEY, BTN_BASE6, buttons["BTN_BASE6"] ? 1 : 0);
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
    // G29 HID Report structure (16 bytes total):
    // Byte 0-1: Steering (16-bit, little endian, 0-65535, center=32768)
    // Byte 2-3: Throttle (16-bit, little endian, inverted: 65535=rest, 0=pressed)
    // Byte 4-5: Brake (16-bit, little endian, inverted: 65535=rest, 0=pressed)  
    // Byte 6-7: Clutch (16-bit, little endian, inverted: 65535=rest, 0=pressed)
    // Byte 8: HAT switch (4 bits) + padding (4 bits)
    // Byte 9-12: Buttons (25 bits) + padding (7 bits)
    
    std::vector<uint8_t> report(16, 0);
    
    // Steering: convert from -32768..32767 to 0..65535
    uint16_t steering_u = static_cast<uint16_t>(static_cast<int16_t>(steering) + 32768);
    report[0] = steering_u & 0xFF;
    report[1] = (steering_u >> 8) & 0xFF;
    
    // Throttle: inverted, 0-100% -> 65535-0
    uint16_t throttle_u = 65535 - static_cast<uint16_t>(throttle * 655.35f);
    report[2] = throttle_u & 0xFF;
    report[3] = (throttle_u >> 8) & 0xFF;
    
    // Brake: inverted, 0-100% -> 65535-0
    uint16_t brake_u = 65535 - static_cast<uint16_t>(brake * 655.35f);
    report[4] = brake_u & 0xFF;
    report[5] = (brake_u >> 8) & 0xFF;
    
    // Clutch: always at rest (65535)
    report[6] = 0xFF;
    report[7] = 0xFF;
    
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
    
    // Buttons: Pack 25 buttons into 4 bytes (32 bits, use 25)
    uint32_t button_bits = 0;
    if (buttons["BTN_TRIGGER"]) button_bits |= (1 << 0);
    if (buttons["BTN_THUMB"]) button_bits |= (1 << 1);
    if (buttons["BTN_THUMB2"]) button_bits |= (1 << 2);
    if (buttons["BTN_TOP"]) button_bits |= (1 << 3);
    if (buttons["BTN_TOP2"]) button_bits |= (1 << 4);
    if (buttons["BTN_PINKIE"]) button_bits |= (1 << 5);
    if (buttons["BTN_BASE"]) button_bits |= (1 << 6);
    if (buttons["BTN_BASE2"]) button_bits |= (1 << 7);
    if (buttons["BTN_BASE3"]) button_bits |= (1 << 8);
    if (buttons["BTN_BASE4"]) button_bits |= (1 << 9);
    if (buttons["BTN_BASE5"]) button_bits |= (1 << 10);
    if (buttons["BTN_BASE6"]) button_bits |= (1 << 11);
    if (buttons["BTN_DEAD"]) button_bits |= (1 << 12);
    if (buttons["BTN_TRIGGER_HAPPY1"]) button_bits |= (1 << 13);
    if (buttons["BTN_TRIGGER_HAPPY2"]) button_bits |= (1 << 14);
    if (buttons["BTN_TRIGGER_HAPPY3"]) button_bits |= (1 << 15);
    if (buttons["BTN_TRIGGER_HAPPY4"]) button_bits |= (1 << 16);
    if (buttons["BTN_TRIGGER_HAPPY5"]) button_bits |= (1 << 17);
    if (buttons["BTN_TRIGGER_HAPPY6"]) button_bits |= (1 << 18);
    if (buttons["BTN_TRIGGER_HAPPY7"]) button_bits |= (1 << 19);
    if (buttons["BTN_TRIGGER_HAPPY8"]) button_bits |= (1 << 20);
    if (buttons["BTN_TRIGGER_HAPPY9"]) button_bits |= (1 << 21);
    if (buttons["BTN_TRIGGER_HAPPY10"]) button_bits |= (1 << 22);
    if (buttons["BTN_TRIGGER_HAPPY11"]) button_bits |= (1 << 23);
    if (buttons["BTN_TRIGGER_HAPPY12"]) button_bits |= (1 << 24);
    
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
                // ev.u.output contains FFB data
                // For now, we just acknowledge it
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
    // This thread mimics real USB HID device behavior:
    // - Host polls the interrupt IN endpoint at regular intervals (1-8ms typical)
    // - Device responds immediately with current HID report
    // - This is request-response, not push-based
    
    std::cout << "USB Gadget polling thread started (mimicking real USB HID behavior)" << std::endl;
    
    // Set to blocking mode for proper poll-response behavior
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    
    while (gadget_running) {
        // Build current HID report with thread-safe state access
        std::vector<uint8_t> report;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            report = BuildHIDReport();
        }
        
        // Write report - this blocks until host polls (like real USB HID)
        // The kernel USB gadget driver handles the actual USB protocol
        ssize_t ret = write(fd, report.data(), report.size());
        
        if (ret < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                // Temporary error, retry
                usleep(1000);
                continue;
            } else if (errno == ESHUTDOWN || errno == ECONNRESET) {
                // Device disconnected gracefully
                std::cout << "USB Gadget device disconnected" << std::endl;
                break;
            } else {
                // Other error
                std::cerr << "USB Gadget write error: " << strerror(errno) << std::endl;
                usleep(1000);
            }
        }
        
        // Small delay to prevent CPU spinning if there's an issue
        // Real polling rate is controlled by host (typically 1-8ms)
        usleep(100);
    }
    
    std::cout << "USB Gadget polling thread stopped" << std::endl;
}
