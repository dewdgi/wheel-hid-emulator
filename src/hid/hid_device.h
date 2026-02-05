#ifndef HID_DEVICE_H
#define HID_DEVICE_H

#include <array>
#include <atomic>
#include <string>
#include <windows.h>
#include "../vjoy_sdk/inc/public.h"
#include "../vjoy_sdk/inc/vjoyinterface.h"

namespace hid {

class HidDevice {
public:
    HidDevice();
    ~HidDevice();

    bool Initialize();
    void Shutdown();
    bool IsReady() const;

    // The core output function
    bool WriteReportBlocking(const std::array<uint8_t, 13>& report);

    // FFB Callback mechanism for WheelDevice to hook into
    void RegisterFFBCallback(void* callback, void* user_data);

private:
    std::atomic<bool> acquired_;
    UINT vjoy_id_ = 1;
};

}  // namespace hid

#endif  // HID_DEVICE_H
