#include "hid_device.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <poll.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>

#include "../logging/logger.h"

namespace hid {
namespace {
constexpr uint16_t kVendorId = 0x046d;
constexpr uint16_t kProductId = 0xc24f;
constexpr uint16_t kVersion = 0x0111;
constexpr size_t kReportLength = 13;
constexpr const char* kGadgetName = "g29wheel";
constexpr const char* kHidFunction = "hid.usb0";
constexpr const char* kHidDevicePath = "/dev/hidg0";
constexpr int kDefaultPollTimeoutMs = 50;

constexpr uint8_t kG29HidDescriptor[] = {
    0x05, 0x01, 0x09, 0x04, 0xA1, 0x01, 0xA1, 0x02, 0x09, 0x01, 0xA1, 0x00,
    0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35, 0x15, 0x00,
    0x27, 0xFF, 0xFF, 0x00, 0x00, 0x35, 0x00,
    0x47, 0xFF, 0xFF, 0x00, 0x00, 0x75, 0x10,
    0x95, 0x04, 0x81, 0x02, 0xC0,
    0x09, 0x39, 0x15, 0x00, 0x25, 0x07, 0x35, 0x00, 0x46, 0x3B, 0x01,
    0x65, 0x14, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42,
    0x75, 0x04, 0x95, 0x01, 0x81, 0x03,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x1A, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x1A, 0x81, 0x02,
    0x75, 0x06, 0x95, 0x01, 0x81, 0x03,
    0xC0,
    0xA1, 0x02, 0x09, 0x02, 0x15, 0x00, 0x26, 0xFF, 0x00,
    0x95, 0x07, 0x75, 0x08, 0x91, 0x02,
    0xC0,
    0xC0
};

std::string HexValue(uint16_t value) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%04x", value);
    return std::string(buf);
}

std::string ReadTrimmedFile(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        return {};
    }
    std::string line;
    std::getline(in, line);
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) {
        line.pop_back();
    }
    return line;
}

bool WriteStringToFile(const std::string& path, const std::string& value) {
    int fd = open(path.c_str(), O_WRONLY | O_TRUNC);
    if (fd < 0) {
        return false;
    }
    std::string payload = value;
    if (!payload.empty() && payload.back() != '\n') {
        payload.push_back('\n');
    } else if (payload.empty()) {
        payload = "\n";
    }
    ssize_t written = write(fd, payload.data(), payload.size());
    close(fd);
    return written == static_cast<ssize_t>(payload.size());
}

bool RunCommand(const std::string& command) {
    int rc = std::system(command.c_str());
    if (rc != 0) {
        LOG_DEBUG("hid", "Command failed (" << rc << "): " << command);
    }
    return rc == 0;
}

void RemoveGadgetTree(const std::string& gadget_name, const std::string& hid_function) {
    std::string cleanup = "cd /sys/kernel/config/usb_gadget 2>/dev/null && ";
    cleanup += "if [ -d " + gadget_name + " ]; then ";
    cleanup += "  cd " + gadget_name + " && ";
    cleanup += "  echo '' > UDC 2>/dev/null || true; ";
    cleanup += "  rm -f configs/c.1/" + hid_function + " 2>/dev/null || true; ";
    cleanup += "  rmdir configs/c.1/strings/0x409 2>/dev/null || true; ";
    cleanup += "  rmdir configs/c.1 2>/dev/null || true; ";
    cleanup += "  rmdir functions/" + hid_function + " 2>/dev/null || true; ";
    cleanup += "  rmdir strings/0x409 2>/dev/null || true; ";
    cleanup += "  cd .. && rmdir " + gadget_name + " 2>/dev/null || true; ";
    cleanup += "fi";
    RunCommand(cleanup);
}

void EnsureKernelModulesLoaded() {
    RunCommand("modprobe libcomposite 2>/dev/null");
    RunCommand("modprobe dummy_hcd 2>/dev/null");
}

void EnsureConfigfsMounted() {
    if (access("/sys/kernel/config", F_OK) == 0) {
        return;
    }
    RunCommand("mkdir -p /sys/kernel/config 2>/dev/null");
    RunCommand("mount -t configfs none /sys/kernel/config 2>/dev/null");
}

}  // namespace

HidDevice::HidDevice()
        : fd_(-1), udc_bound_(false), non_blocking_mode_(true) {}

HidDevice::~HidDevice() {
    Shutdown();
}

bool HidDevice::Initialize() {
    LOG_INFO("hid", "Initializing USB HID gadget");
    if (!CreateUSBGadget()) {
        LOG_ERROR("hid", "Failed to create USB gadget tree");
        return false;
    }
    if (!BindUDC()) {
        DestroyUSBGadget();
        return false;
    }
    if (!EnsureEndpointOpen()) {
        DestroyUSBGadget();
        return false;
    }
    return true;
}

void HidDevice::Shutdown() {
    LOG_INFO("hid", "Shutting down HID gadget");
    {
        std::lock_guard<std::mutex> lock(fd_mutex_);
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
    }
    DestroyUSBGadget();
}

int HidDevice::fd() const {
    std::lock_guard<std::mutex> lock(fd_mutex_);
    return fd_;
}

bool HidDevice::IsReady() const {
    std::lock_guard<std::mutex> lock(fd_mutex_);
    return fd_ >= 0;
}

void HidDevice::SetNonBlockingMode(bool enabled) {
    bool previous = non_blocking_mode_.exchange(enabled);
    if (previous == enabled) {
        return;
    }
    std::lock_guard<std::mutex> lock(fd_mutex_);
    if (fd_ < 0) {
        return;
    }
    int flags = fcntl(fd_, F_GETFL, 0);
    if (flags < 0) {
        LOG_ERROR("hid", "fcntl(F_GETFL) failed: " << std::strerror(errno));
        return;
    }
    if (enabled) {
        flags |= O_NONBLOCK;
    } else {
        flags &= ~O_NONBLOCK;
    }
    if (fcntl(fd_, F_SETFL, flags) < 0) {
        LOG_ERROR("hid", "fcntl(F_SETFL) failed: " << std::strerror(errno));
    }
}

void HidDevice::ResetEndpoint() {
    std::lock_guard<std::mutex> lock(fd_mutex_);
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
}

bool HidDevice::CreateUSBGadget() {
    EnsureKernelModulesLoaded();
    EnsureConfigfsMounted();

    if (access("/sys/kernel/config/usb_gadget", F_OK) != 0) {
        LOG_ERROR("hid", "USB Gadget ConfigFS not available");
        return false;
    }
    if (access("/sys/class/udc", F_OK) != 0) {
        LOG_ERROR("hid", "No USB Device Controller detected");
        return false;
    }

    const std::string gadget_path = std::string("/sys/kernel/config/usb_gadget/") + kGadgetName;
    bool gadget_exists = (access(gadget_path.c_str(), F_OK) == 0);
    if (gadget_exists) {
        bool hid_exists = (access((gadget_path + "/functions/" + kHidFunction).c_str(), F_OK) == 0);
        bool config_exists = (access((gadget_path + "/configs/c.1").c_str(), F_OK) == 0);
        if (!hid_exists || !config_exists) {
            LOG_INFO("hid", "Existing gadget incomplete, rebuilding");
            RemoveGadgetTree(kGadgetName, kHidFunction);
            gadget_exists = false;
        }
    }

    if (!gadget_exists) {
        std::string descriptor_hex;
        descriptor_hex.reserve(sizeof(kG29HidDescriptor) * 4);
        for (uint8_t byte : kG29HidDescriptor) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\x%02x", byte);
            descriptor_hex += buf;
        }

        const std::string vendor_hex = HexValue(kVendorId);
        const std::string product_hex = HexValue(kProductId);
        const std::string version_hex = HexValue(kVersion);

        std::string cmd = "cd /sys/kernel/config/usb_gadget && ";
        cmd += "mkdir -p " + std::string(kGadgetName) + " && cd " + kGadgetName + " && ";
        cmd += "echo 0x" + vendor_hex + " > idVendor && ";
        cmd += "echo 0x" + product_hex + " > idProduct && ";
        cmd += "echo 0x" + version_hex + " > bcdDevice && ";
        cmd += "echo 0x0200 > bcdUSB && ";
        cmd += "mkdir -p strings/0x409 && ";
        cmd += "echo 'Logitech' > strings/0x409/manufacturer && ";
        cmd += "echo 'G29 Driving Force Racing Wheel' > strings/0x409/product && ";
        cmd += "echo '000000000001' > strings/0x409/serialnumber && ";
        cmd += "mkdir -p functions/" + std::string(kHidFunction) + " && cd functions/" + kHidFunction + " && ";
        cmd += "echo 1 > protocol && echo 1 > subclass && ";
        cmd += "echo " + std::to_string(kReportLength) + " > report_length && ";
        cmd += "printf '" + descriptor_hex + "' > report_desc && ";
        cmd += "cd /sys/kernel/config/usb_gadget/" + std::string(kGadgetName) + " && ";
        cmd += "mkdir -p configs/c.1/strings/0x409 && ";
        cmd += "echo 'G29 Configuration' > configs/c.1/strings/0x409/configuration && ";
        cmd += "echo 500 > configs/c.1/MaxPower && ";
        cmd += "ln -sf functions/" + std::string(kHidFunction) + " configs/c.1/";

        if (!RunCommand(cmd)) {
            LOG_ERROR("hid", "Failed to create USB gadget tree");
            RemoveGadgetTree(kGadgetName, kHidFunction);
            return false;
        }
        LOG_INFO("hid", "Created USB gadget '" << kGadgetName << "'");
    } else {
        LOG_INFO("hid", "Reusing USB gadget '" << kGadgetName << "'");
    }

    {
        std::lock_guard<std::mutex> guard(udc_mutex_);
        udc_name_ = ReadTrimmedFile(GadgetUDCPath());
        if (udc_name_.empty()) {
            udc_name_ = DetectFirstUDC();
        }
        if (udc_name_.empty()) {
            LOG_ERROR("hid", "No UDC available to bind");
            return false;
        }
    }

    return true;
}

void HidDevice::DestroyUSBGadget() {
    UnbindUDC();
    RemoveGadgetTree(kGadgetName, kHidFunction);
}

std::string HidDevice::GadgetUDCPath() const {
    return std::string("/sys/kernel/config/usb_gadget/") + kGadgetName + "/UDC";
}

std::string HidDevice::GadgetStatePath() const {
    return std::string("/sys/kernel/config/usb_gadget/") + kGadgetName + "/state";
}

std::string HidDevice::DetectFirstUDC() const {
    DIR* dir = opendir("/sys/class/udc");
    if (!dir) {
        return {};
    }
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        std::string candidate(entry->d_name);
        closedir(dir);
        return candidate;
    }
    closedir(dir);
    return {};
}

bool HidDevice::BindUDC() {
    std::lock_guard<std::mutex> guard(udc_mutex_);
    if (udc_bound_.load(std::memory_order_acquire)) {
        return true;
    }
    if (udc_name_.empty()) {
        udc_name_ = DetectFirstUDC();
        if (udc_name_.empty()) {
            LOG_ERROR("hid", "Cannot bind gadget: no UDC");
            return false;
        }
    }
    if (!WriteStringToFile(GadgetUDCPath(), udc_name_)) {
        LOG_ERROR("hid", "Failed to bind UDC '" << udc_name_ << "'");
        return false;
    }
    udc_bound_.store(true, std::memory_order_release);
    LOG_INFO("hid", "Bound gadget to UDC '" << udc_name_ << "'");
    return true;
}

bool HidDevice::UnbindUDC() {
    std::lock_guard<std::mutex> guard(udc_mutex_);
    if (!udc_bound_.load(std::memory_order_acquire)) {
        return true;
    }
    if (!WriteStringToFile(GadgetUDCPath(), "")) {
        LOG_ERROR("hid", "Failed to unbind gadget");
        return false;
    }
    udc_bound_.store(false, std::memory_order_release);
    ResetEndpoint();
    LOG_INFO("hid", "Unbound gadget from UDC");
    return true;
}

bool HidDevice::EnsureEndpointOpen() {
    std::lock_guard<std::mutex> lock(fd_mutex_);
    if (fd_ >= 0) {
        return true;
    }
    int flags = O_RDWR;
    if (non_blocking_mode_.load()) {
        flags |= O_NONBLOCK;
    }
    fd_ = open(kHidDevicePath, flags);
    if (fd_ < 0) {
        LOG_ERROR("hid", "Failed to open " << kHidDevicePath << ": " << std::strerror(errno));
        return false;
    }
    LOG_INFO("hid", "Opened HID endpoint " << kHidDevicePath);
    return true;
}

bool HidDevice::WaitForEndpointReady(int timeout_ms) {
    if (timeout_ms <= 0) {
        timeout_ms = kDefaultPollTimeoutMs;
    }
    if (!EnsureEndpointOpen()) {
        return false;
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        int fd_copy;
        {
            std::lock_guard<std::mutex> lock(fd_mutex_);
            fd_copy = fd_;
        }
        if (fd_copy < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        struct pollfd pfd;
        pfd.fd = fd_copy;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        int wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (wait_ms < 0) {
            wait_ms = 0;
        }
        int rc = poll(&pfd, 1, wait_ms);
        if (rc > 0) {
            if (pfd.revents & (POLLOUT | POLLWRNORM)) {
                return true;
            }
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                std::lock_guard<std::mutex> lock(fd_mutex_);
                close(fd_);
                fd_ = -1;
                continue;
            }
        } else if (rc == 0) {
            break;
        } else if (errno != EINTR) {
            LOG_ERROR("hid", "poll failed: " << std::strerror(errno));
            break;
        }
    }
    return false;
}

bool HidDevice::WriteHIDBlocking(const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0) {
        return false;
    }

    size_t total = 0;
    while (total < size) {
        if (!EnsureEndpointOpen()) {
            return false;
        }

        int fd_copy;
        {
            std::lock_guard<std::mutex> lock(fd_mutex_);
            fd_copy = fd_;
        }
        if (fd_copy < 0) {
            continue;
        }

        ssize_t written = ::write(fd_copy, data + total, size - total);
        if (written > 0) {
            total += static_cast<size_t>(written);
            continue;
        }

        if (written == 0) {
            if (!WaitForEndpointReady(kDefaultPollTimeoutMs)) {
                return false;
            }
            continue;
        }

        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (!WaitForEndpointReady(kDefaultPollTimeoutMs)) {
                return false;
            }
            continue;
        }
        if (errno == EPIPE || errno == ENODEV || errno == ESHUTDOWN) {
            std::lock_guard<std::mutex> lock(fd_mutex_);
            if (fd_ >= 0) {
                close(fd_);
                fd_ = -1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        LOG_ERROR("hid", "write failed: " << std::strerror(errno));
        return false;
    }
    return true;
}

bool HidDevice::WriteReportBlocking(const std::array<uint8_t, 13>& report) {
    return WriteHIDBlocking(report.data(), report.size());
}

}  // namespace hid
