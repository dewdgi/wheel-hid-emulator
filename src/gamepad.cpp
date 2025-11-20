#include "gamepad.h"
void GamepadDevice::NotifyAllShutdownCVs() {
    // Wake all threads waiting on condition variables
    state_cv.notify_all();
    ffb_cv.notify_all();
}
#include "input.h"
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <cstring>
#include <iostream>
#include <dirent.h>
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <cerrno>
#include <linux/uhid.h>
#include <linux/uinput.h>
#include <linux/input-event-codes.h>
#include <thread>

namespace {

constexpr std::array<uint16_t, static_cast<size_t>(WheelButton::Count)> kButtonCodes = {
    BTN_SOUTH,
    BTN_EAST,
    BTN_WEST,
    BTN_NORTH,
    BTN_TL,
    BTN_TR,
    BTN_TL2,
    BTN_TR2,
    BTN_SELECT,
    BTN_START,
    BTN_THUMBL,
    BTN_THUMBR,
    BTN_MODE,
    BTN_DEAD,
    BTN_TRIGGER_HAPPY1,
    BTN_TRIGGER_HAPPY2,
    BTN_TRIGGER_HAPPY3,
    BTN_TRIGGER_HAPPY4,
    BTN_TRIGGER_HAPPY5,
    BTN_TRIGGER_HAPPY6,
    BTN_TRIGGER_HAPPY7,
    BTN_TRIGGER_HAPPY8,
    BTN_TRIGGER_HAPPY9,
    BTN_TRIGGER_HAPPY10,
    BTN_TRIGGER_HAPPY11,
    BTN_TRIGGER_HAPPY12
};

static_assert(kButtonCodes.size() == static_cast<size_t>(WheelButton::Count), "Button code table mismatch");

}

void GamepadDevice::ShutdownThreads() {
    ffb_running = false;
    gadget_running = false;
    state_cv.notify_all();
    ffb_cv.notify_all();

    if (gadget_thread.joinable()) {
        gadget_thread.join();
    }
    if (ffb_thread.joinable()) {
        ffb_thread.join();
    }

    if (fd >= 0) {
        close(fd);
        fd = -1;
    }

    DestroyUSBGadget();
}
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
#include <thread>

GamepadDevice::~GamepadDevice() {
    ShutdownThreads();
}
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
        enabled(false), steering(0), user_steering(0.0f), ffb_offset(0.0f), ffb_velocity(0.0f), ffb_gain(1.0f),
            throttle(0.0f), brake(0.0f), clutch(0.0f), dpad_x(0), dpad_y(0),
            ffb_force(0), ffb_autocenter(0), ffb_enabled(true),
            gadget_output_pending_len(0) {
    ffb_running = false;
    state_dirty = false;
    button_states.fill(0);
}

// Query enabled state (mutex-protected)
bool GamepadDevice::IsEnabled() {
    std::lock_guard<std::mutex> lock(state_mutex);
    return enabled;
}

void GamepadDevice::SetEnabled(bool enable, Input& input) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        if (enabled != enable) {
            enabled = enable;
            changed = true;
        }
    }
    if (!changed) {
        return;
    }

    input.Grab(enable);
    if (!enable) {
        input.ResetState();
    }
    SendNeutral();
    std::cout << (enable ? "Emulation ENABLED" : "Emulation DISABLED") << std::endl;
}

// Toggle enabled state atomically (mutex-protected)
void GamepadDevice::ToggleEnabled(Input& input) {
    bool next_state;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        next_state = !enabled;
    }
    SetEnabled(next_state, input);
}

void GamepadDevice::SetFFBGain(float gain) {
    if (gain < 0.1f) {
        gain = 0.1f;
    } else if (gain > 4.0f) {
        gain = 4.0f;
    }
    std::lock_guard<std::mutex> lock(state_mutex);
    ffb_gain = gain;
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
    std::cout << "Attempting to create device using USB Gadget (real USB device)..." << std::endl;
    if (!CreateUSBGadget()) {
        std::cerr << "USB Gadget creation failed; wheel emulator requires a USB gadget capable kernel" << std::endl;
        std::cerr << "Ensure configfs is mounted, libcomposite/dummy_hcd modules are available, and a UDC is present." << std::endl;
        return false;
    }

    if (!gadget_thread.joinable()) {
        gadget_thread = std::thread(&GamepadDevice::USBGadgetPollingThread, this);
    }
    if (!ffb_thread.joinable()) {
        ffb_thread = std::thread(&GamepadDevice::FFBUpdateThread, this);
    }
    SendNeutral();
    return true;
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
          "ln -s functions/hid.usb0 configs/c.1/&& "
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

void GamepadDevice::DestroyUSBGadget() {
    if (!use_gadget) {
        return;
    }

    const char* cleanup_cmd =
        "cd /sys/kernel/config/usb_gadget 2>/dev/null && "
        "if [ -d g29wheel ]; then "
        "  cd g29wheel && "
        "  echo '' > UDC 2>/dev/null || true; "
        "  rm -f configs/c.1/hid.usb0 2>/dev/null || true; "
        "  rmdir configs/c.1/strings/0x409 2>/dev/null || true; "
        "  rmdir configs/c.1 2>/dev/null || true; "
        "  rmdir functions/hid.usb0 2>/dev/null || true; "
        "  cd .. && rmdir g29wheel 2>/dev/null || true; "
        "fi";

    int ret = system(cleanup_cmd);
    if (ret != 0) {
        std::cerr << "USB Gadget cleanup command returned " << ret << std::endl;
    } else {
        std::cout << "USB Gadget g29wheel removed" << std::endl;
    }

    use_gadget = false;
    use_uhid = false;
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
    if (delta == 0) {
        return;
    }

    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        constexpr float base_gain = 0.05f;  // Keeps mouse steering response consistent with earlier builds
        const float gain = static_cast<float>(sensitivity) * base_gain;
        const float max_step = 2000.0f;
        float step = delta * gain;
        if (step > max_step) step = max_step;
        if (step < -max_step) step = -max_step;
        user_steering += step;
        const float max_angle = 32767.0f;
        if (user_steering > max_angle) user_steering = max_angle;
        if (user_steering < -max_angle) user_steering = -max_angle;
        changed = ApplySteeringLocked();
    }

    if (changed) {
        NotifyStateChanged();
    }
}

void GamepadDevice::UpdateThrottle(bool pressed, float /*dt*/) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        float next = pressed ? 100.0f : 0.0f;
        if (throttle != next) {
            throttle = next;
            changed = true;
        }
    }
    if (changed) {
        NotifyStateChanged();
    }
}

void GamepadDevice::UpdateBrake(bool pressed, float /*dt*/) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        float next = pressed ? 100.0f : 0.0f;
        if (brake != next) {
            brake = next;
            changed = true;
        }
    }
    if (changed) {
        NotifyStateChanged();
    }
}

void GamepadDevice::UpdateClutch(bool pressed, float /*dt*/) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        float next = pressed ? 100.0f : 0.0f;
        if (clutch != next) {
            clutch = next;
            changed = true;
        }
    }
    if (changed) {
        NotifyStateChanged();
    }
}

void GamepadDevice::UpdateButtons(const Input& input) {
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        SetButton(WheelButton::South, input.IsKeyPressed(KEY_Q));
        SetButton(WheelButton::East, input.IsKeyPressed(KEY_E));
        SetButton(WheelButton::West, input.IsKeyPressed(KEY_F));
        SetButton(WheelButton::North, input.IsKeyPressed(KEY_G));
        SetButton(WheelButton::TL, input.IsKeyPressed(KEY_H));
        SetButton(WheelButton::TR, input.IsKeyPressed(KEY_R));
        SetButton(WheelButton::TL2, input.IsKeyPressed(KEY_T));
        SetButton(WheelButton::TR2, input.IsKeyPressed(KEY_Y));
        SetButton(WheelButton::Select, input.IsKeyPressed(KEY_U));
        SetButton(WheelButton::Start, input.IsKeyPressed(KEY_I));
        SetButton(WheelButton::ThumbL, input.IsKeyPressed(KEY_O));
        SetButton(WheelButton::ThumbR, input.IsKeyPressed(KEY_P));
        SetButton(WheelButton::Mode, input.IsKeyPressed(KEY_1));
        SetButton(WheelButton::Dead, input.IsKeyPressed(KEY_2));
        SetButton(WheelButton::TriggerHappy1, input.IsKeyPressed(KEY_3));
        SetButton(WheelButton::TriggerHappy2, input.IsKeyPressed(KEY_4));
        SetButton(WheelButton::TriggerHappy3, input.IsKeyPressed(KEY_5));
        SetButton(WheelButton::TriggerHappy4, input.IsKeyPressed(KEY_6));
        SetButton(WheelButton::TriggerHappy5, input.IsKeyPressed(KEY_7));
        SetButton(WheelButton::TriggerHappy6, input.IsKeyPressed(KEY_8));
        SetButton(WheelButton::TriggerHappy7, input.IsKeyPressed(KEY_9));
        SetButton(WheelButton::TriggerHappy8, input.IsKeyPressed(KEY_0));
        SetButton(WheelButton::TriggerHappy9, input.IsKeyPressed(KEY_LEFTSHIFT));
        SetButton(WheelButton::TriggerHappy10, input.IsKeyPressed(KEY_SPACE));
        SetButton(WheelButton::TriggerHappy11, input.IsKeyPressed(KEY_TAB));
        SetButton(WheelButton::TriggerHappy12, input.IsKeyPressed(KEY_ENTER));
    }
    NotifyStateChanged();
}

void GamepadDevice::NotifyStateChanged() {
    state_dirty.store(true, std::memory_order_release);
    state_cv.notify_all();
    ffb_cv.notify_all();
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
    if (fd < 0) {
        return;
    }

    if (use_gadget) {
        if (!state_dirty.load(std::memory_order_acquire)) {
            NotifyStateChanged();
        }
        return;
    }

    if (use_uhid) {
        SendUHIDReport();
        return;
    }

    std::lock_guard<std::mutex> lock(state_mutex);
    uint32_t button_bits = BuildButtonBitsLocked();

    // Steering (ABS_X)
    EmitEvent(EV_ABS, ABS_X, static_cast<int16_t>(steering));

    // Clutch (ABS_Y) inverted to match real G29
    int16_t clutch_val = 32767 - static_cast<int16_t>(clutch * 655.35f);
    EmitEvent(EV_ABS, ABS_Y, clutch_val);

    // Throttle / Brake
    int16_t throttle_val = 32767 - static_cast<int16_t>(throttle * 655.35f);
    int16_t brake_val = 32767 - static_cast<int16_t>(brake * 655.35f);
    EmitEvent(EV_ABS, ABS_Z, brake_val);
    EmitEvent(EV_ABS, ABS_RZ, throttle_val);

    // Buttons
    for (size_t i = 0; i < button_states.size(); ++i) {
        bool pressed = (button_bits >> i) & 0x1u;
        EmitEvent(EV_KEY, kButtonCodes[i], pressed ? 1 : 0);
    }

    // D-Pad
    EmitEvent(EV_ABS, ABS_HAT0X, dpad_x);
    EmitEvent(EV_ABS, ABS_HAT0Y, dpad_y);

    EmitEvent(EV_SYN, SYN_REPORT, 0);
}

std::array<uint8_t, 13> GamepadDevice::BuildHIDReport() {
    // G29 HID Report structure (13 bytes total as defined by descriptor):
    // Byte 0-1: X (Steering) - 16-bit, little endian, 0-65535, center=32768
    // Byte 2-3: Y (Unused) - 16-bit, constant 65535
    // Byte 4-5: Z (Brake) - 16-bit, little endian, inverted: 65535=rest, 0=pressed  
    // Byte 6-7: Rz (Throttle) - 16-bit, little endian, inverted: 65535=rest, 0=pressed
    // Byte 8: HAT switch (4 bits) + padding (4 bits)
    // Byte 9-12: Buttons (25 bits) + padding (7 bits)
    
    // Lock state mutex to prevent race with FFB thread
    std::lock_guard<std::mutex> lock(state_mutex);
    
    std::array<uint8_t, 13> report{};
    
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
    
    uint32_t button_bits = BuildButtonBitsLocked();
    report[9] = button_bits & 0xFF;
    report[10] = (button_bits >> 8) & 0xFF;
    report[11] = (button_bits >> 16) & 0xFF;
    report[12] = (button_bits >> 24) & 0xFF;
    
    return report;
}

void GamepadDevice::SendUHIDReport() {
    auto report_data = BuildHIDReport();
    
    if (use_gadget) {
        // USB Gadget: Write raw HID report directly with retry on EAGAIN
        WriteHIDBlocking(report_data.data(), report_data.size());
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
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        steering = 0.0f;
        user_steering = 0.0f;
        ffb_offset = 0.0f;
        ffb_velocity = 0.0f;
        throttle = 0.0f;
        brake = 0.0f;
        clutch = 0.0f;
        dpad_x = 0;
        dpad_y = 0;
        button_states.fill(0);
    }

    if (use_gadget) {
        NotifyStateChanged();
    } else {
        SendState();
    }
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

void GamepadDevice::SetButton(WheelButton button, bool pressed) {
    button_states[static_cast<size_t>(button)] = pressed ? 1 : 0;
}

uint32_t GamepadDevice::BuildButtonBitsLocked() const {
    uint32_t bits = 0;
    for (size_t i = 0; i < button_states.size(); ++i) {
        if (button_states[i]) {
            bits |= (1u << i);
        }
    }
    return bits;
}

bool GamepadDevice::WriteHIDBlocking(const uint8_t* data, size_t size) {
    if (fd < 0) {
        return false;
    }

    size_t written = 0;
    while (written < size) {
        ssize_t ret = write(fd, data + written, size - written);
        if (ret > 0) {
            written += static_cast<size_t>(ret);
            continue;
        }

        if (ret == -1) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                pollfd p{};
                p.fd = fd;
                p.events = POLLOUT;
                int poll_ret = poll(&p, 1, 5);
                if (poll_ret <= 0) {
                    if (poll_ret == -1 && errno == EINTR) {
                        continue;
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(500));
                    continue;
                }
                continue;
            }
        }
        return false;
    }
    return true;
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
                    
                    auto report = BuildHIDReport();
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
    if (fd < 0) {
        return;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    std::unique_lock<std::mutex> lock(state_mutex);
    while (gadget_running && running) {
        state_cv.wait_for(lock, std::chrono::milliseconds(2), [&]{
            return !gadget_running || !running || state_dirty.load(std::memory_order_acquire);
        });
        if (!gadget_running || !running) {
            break;
        }
        bool should_send = state_dirty.exchange(false, std::memory_order_acq_rel);
        lock.unlock();
        if (should_send) {
            SendUHIDReport();
        }
        ReadGadgetOutput();
        lock.lock();
    }
}

void GamepadDevice::ReadGadgetOutput() {
    if (!use_gadget || fd < 0) {
        return;
    }

    uint8_t buffer[32];
    while (gadget_running && running) {
        ssize_t bytes = read(fd, buffer, sizeof(buffer));
        if (bytes < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            break;
        }
        if (bytes == 0) {
            break;
        }

        size_t total = static_cast<size_t>(bytes);
        size_t offset = 0;
        while (offset < total) {
            size_t needed = 7 - gadget_output_pending_len;
            size_t chunk = total - offset;
            if (chunk > needed) {
                chunk = needed;
            }
            std::memcpy(gadget_output_pending.data() + gadget_output_pending_len,
                        buffer + offset,
                        chunk);
            gadget_output_pending_len += chunk;
            offset += chunk;

            if (gadget_output_pending_len == 7) {
                ParseFFBCommand(gadget_output_pending.data(), 7);
                gadget_output_pending_len = 0;
            }
        }
    }
}

void GamepadDevice::FFBUpdateThread() {
    float filtered_ffb = 0.0f;
    using clock = std::chrono::steady_clock;
    auto last = clock::now();

    while (true) {
        std::unique_lock<std::mutex> lock(state_mutex);
        ffb_cv.wait_for(lock, std::chrono::milliseconds(1));
        if (!ffb_running || !running) {
            break;
        }

        // Snapshot shared state so we can do heavier math outside the lock
        int16_t local_force = ffb_force;
        int16_t local_autocenter = ffb_autocenter;
        float local_offset = ffb_offset;
        float local_velocity = ffb_velocity;
        float local_gain = ffb_gain;
        float local_steering = steering;
        lock.unlock();

        auto now = clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        if (dt <= 0.0f) dt = 0.001f;
        if (dt > 0.01f) dt = 0.01f;
        last = now;

        float commanded_force = ShapeFFBTorque(static_cast<float>(local_force));

        const float force_filter_hz = 38.0f;
        float alpha = 1.0f - std::exp(-dt * force_filter_hz);
        if (alpha < 0.0f) alpha = 0.0f;
        if (alpha > 1.0f) alpha = 1.0f;
        filtered_ffb += (commanded_force - filtered_ffb) * alpha;

        float spring = 0.0f;
        if (local_autocenter > 0) {
            spring = -(local_steering * static_cast<float>(local_autocenter)) / 32768.0f;
        }

        const float offset_limit = 22000.0f;
        float target_offset = (filtered_ffb + spring) * local_gain;
        target_offset = std::clamp(target_offset, -offset_limit, offset_limit);

        const float stiffness = 120.0f;
        const float damping = 8.0f;
        const float max_velocity = 90000.0f;
        float error = target_offset - local_offset;
        local_velocity += error * stiffness * dt;
        float damping_factor = std::exp(-damping * dt);
        local_velocity *= damping_factor;
        local_velocity = std::clamp(local_velocity, -max_velocity, max_velocity);

        local_offset += local_velocity * dt;
        if (local_offset > offset_limit) {
            local_offset = offset_limit;
            local_velocity = 0.0f;
        } else if (local_offset < -offset_limit) {
            local_offset = -offset_limit;
            local_velocity = 0.0f;
        }

        lock.lock();
        if (!ffb_running || !running) {
            break;
        }
        ffb_offset = local_offset;
        ffb_velocity = local_velocity;
        bool steering_changed = ApplySteeringLocked();
        lock.unlock();

        if (steering_changed) {
            state_dirty.store(true, std::memory_order_release);
            state_cv.notify_all();
        }
    }
}

void GamepadDevice::ParseFFBCommand(const uint8_t* data, size_t size) {
    if (size != 7) return;  // G29 FFB commands are always 7 bytes
    
    std::lock_guard<std::mutex> lock(state_mutex);
    
    uint8_t cmd = data[0];
    
    // Logitech G29 FFB protocol (based on hid-lg4ff.c kernel driver)
    switch (cmd) {
        case 0x11:  // Constant force effect (slot 1)
            // data[1] = 0x08 (effect type)
            // data[2] = force magnitude (0x00-0xFF, 0x80 = no force)
            // data[3] = direction? (0x80 = center)
            {
                // Convert 0x00-0xFF range to -128..127 force value
                int8_t force = static_cast<int8_t>(data[2]) - 0x80;
                // Invert direction (Logitech positive pushes toward center) and keep strength modest
                ffb_force = static_cast<int16_t>(-force) * 48;
            }
            break;
            
        case 0x13:  // Stop effect / de-activate force
            ffb_force = 0;
            break;
            
        case 0xf5:  // Disable autocenter
            ffb_autocenter = 0;
            break;
            
        case 0xfe:  // Set autocenter parameters
            // data[1] = 0x0d
            // data[2], data[3] = autocenter strength
            // data[4] = spring rate
            if (data[1] == 0x0d) {
                ffb_autocenter = static_cast<int16_t>(data[2]) * 16;  // Gentler spring
            }
            break;
            
        case 0x14:  // Activate autocenter
            if (ffb_autocenter == 0) {
                ffb_autocenter = 1024;  // Default light autocenter
            }
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

float GamepadDevice::ShapeFFBTorque(float raw_force) const {
    float abs_force = std::fabs(raw_force);
    if (abs_force < 80.0f) {
        // Keep microscopic oscillations alive but nearly imperceptible
        return raw_force * (abs_force / 80.0f);
    }

    const float min_gain = 0.25f; // Light road feel while cruising
    const float slip_knee = 4000.0f;
    const float slip_full = 14000.0f;
    float t = (abs_force - 80.0f) / (slip_full - 80.0f);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    float slip_weight = t * t; // Emphasize larger forces non-linearly

    float gain = min_gain;
    if (abs_force > slip_knee) {
        float heavy = (abs_force - slip_knee) / (slip_full - slip_knee);
        if (heavy < 0.0f) heavy = 0.0f;
        if (heavy > 1.0f) heavy = 1.0f;
        gain = min_gain + (1.0f - min_gain) * heavy;
    } else {
        gain = min_gain + (slip_weight * (1.0f - min_gain));
    }

    const float boost = 3.0f;
    return raw_force * gain * boost;
}

bool GamepadDevice::ApplySteeringLocked() {
    float combined = user_steering + ffb_offset;
    if (combined > 32767.0f) combined = 32767.0f;
    if (combined < -32768.0f) combined = -32768.0f;
    if (std::fabs(combined - steering) < 0.1f) {
        return false;
    }
    steering = combined;
    return true;
}
