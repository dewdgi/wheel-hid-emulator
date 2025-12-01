#ifndef HID_DEVICE_H
#define HID_DEVICE_H

#include <array>
#include <atomic>
#include <mutex>
#include <string>

namespace hid {

class HidDevice {
public:
    HidDevice();
    ~HidDevice();

    HidDevice(const HidDevice&) = delete;
    HidDevice& operator=(const HidDevice&) = delete;
    HidDevice(HidDevice&&) noexcept = delete;
    HidDevice& operator=(HidDevice&&) noexcept = delete;

    bool Initialize();
    void Shutdown();

    int fd() const { return fd_; }
    bool IsReady() const { return fd_ >= 0; }

    void SetNonBlockingMode(bool enabled);
    void ResetEndpoint();

    bool WaitForEndpointReady(int timeout_ms = 1500);
    bool WriteReportBlocking(const std::array<uint8_t, 13>& report);
    bool WriteHIDBlocking(const uint8_t* data, size_t size);

    bool BindUDC();
    bool UnbindUDC();
    bool IsUdcBound() const { return udc_bound_.load(std::memory_order_acquire); }

private:
    bool CreateUSBGadget();
    void DestroyUSBGadget();
    std::string GadgetUDCPath() const;
    std::string GadgetStatePath() const;
    std::string DetectFirstUDC() const;
    bool EnsureEndpointOpen();

    int fd_;
    std::atomic<bool> udc_bound_;
    std::string udc_name_;
    std::atomic<bool> non_blocking_mode_;
    mutable std::mutex fd_mutex_;
    mutable std::mutex udc_mutex_;

};

}  // namespace hid

#endif  // HID_DEVICE_H
