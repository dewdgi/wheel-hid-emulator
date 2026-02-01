#include "hid_device.h"
#include "../logging/logger.h"
#include <iostream>
#include <thread>
#include "vjoy_loader.h"

namespace hid {

constexpr const char* kTag = "hid_device";

HidDevice::HidDevice() : udc_bound_(false), non_blocking_mode_(true), vjoy_id_(1) {}

HidDevice::~HidDevice() {
    if (vjoy_id_ > 0 && vJoy.IsLoaded()) {
        // vJoy.RelinquishVJD(vjoy_id_); // Optional
    }
    FreeVJoyLibrary();
}

bool HidDevice::Initialize() {
    if (!LoadVJoyLibrary()) {
        LOG_ERROR(kTag, "Could not load vJoyInterface.dll");
        return false;
    }

    if (!vJoy.vJoyEnabled()) {
        LOG_ERROR(kTag, "vJoy driver not enabled - failed to initialize");
        return false;
    }

    // Check status
    VjdStat status = vJoy.GetVJDStatus(vjoy_id_);
    if (status == VJD_STAT_OWN || status == VJD_STAT_FREE) {
        if (!vJoy.AcquireVJD(vjoy_id_)) {
            LOG_ERROR(kTag, "Failed to acquire vJoy device " + std::to_string(vjoy_id_));
            return false;
        }
        LOG_INFO(kTag, "Acquired vJoy device " + std::to_string(vjoy_id_));
    } else {
        LOG_ERROR(kTag, "vJoy device " + std::to_string(vjoy_id_) + " is busy or missing");
        return false;
    }
    
    udc_bound_ = true;
    vJoy.ResetVJD(vjoy_id_);
    return true;
}

void HidDevice::Shutdown() {
    if (udc_bound_ && vJoy.IsLoaded()) {
        vJoy.RelinquishVJD(vjoy_id_);
        udc_bound_ = false;
    }
}

void HidDevice::ResetEndpoint() {
    if (vJoy.IsLoaded()) {
        vJoy.ResetVJD(vjoy_id_);
    }
}

bool HidDevice::BindUDC() {
    return true; // Not needed for vJoy
}

bool HidDevice::UnbindUDC() {
    return true; 
}

bool HidDevice::IsUdcBound() const {
    return udc_bound_; 
}

bool HidDevice::IsReady() const {
    if (!vJoy.IsLoaded()) return false;
    return vJoy.GetVJDStatus(vjoy_id_) == VJD_STAT_OWN;
}

bool HidDevice::WaitForEndpointReady(int timeout_ms) {
    if (IsReady()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
    return IsReady();
}

void HidDevice::SetNonBlockingMode(bool non_blocking) {
    // Not applicable for vJoy API
}

bool HidDevice::WriteReportBlocking(const std::array<uint8_t, 13>& report) {
    if (!vJoy.IsLoaded()) return false;

    // Convert internal 13-byte report to vJoy JOYSTICK_POSITION_V2
    JOYSTICK_POSITION_V2 iReport;
    iReport.bDevice = (BYTE)vjoy_id_;

    // 1. Steering (0..65535 -> 1..32768)
    uint16_t steering_u = static_cast<uint16_t>(report[0]) | (static_cast<uint16_t>(report[1]) << 8);
    iReport.wAxisX = static_cast<LONG>((steering_u / 2) + 1);

    // 2. Pedals (0..65535 -> 1..32768)
    uint16_t clutch_u = static_cast<uint16_t>(report[2]) | (static_cast<uint16_t>(report[3]) << 8);
    uint16_t throttle_u = static_cast<uint16_t>(report[4]) | (static_cast<uint16_t>(report[5]) << 8);
    uint16_t brake_u = static_cast<uint16_t>(report[6]) | (static_cast<uint16_t>(report[7]) << 8);
    
    iReport.wAxisY = static_cast<LONG>((throttle_u / 2) + 1); 
    iReport.wAxisZ = static_cast<LONG>((brake_u / 2) + 1);    
    iReport.wAxisXRot = static_cast<LONG>((clutch_u / 2) + 1);

    // 3. Hat
    uint8_t hat = report[8] & 0x0F;
    if (hat > 7) iReport.bHats = -1;
    else iReport.bHats = hat * 4500; 

    // 4. Buttons
    uint32_t buttons = static_cast<uint32_t>(report[9]) | 
                       (static_cast<uint32_t>(report[10]) << 8) | 
                       (static_cast<uint32_t>(report[11]) << 16) | 
                       (static_cast<uint32_t>(report[12]) << 24);
    iReport.lButtons = buttons;
    iReport.lButtonsEx1 = 0;
    iReport.lButtonsEx2 = 0;
    iReport.lButtonsEx3 = 0;

    return vJoy.UpdateVJD(vjoy_id_, (PVOID)&iReport);
}

bool HidDevice::WriteHIDBlocking(const uint8_t* data, size_t size) {
    return false;
}

void HidDevice::RegisterFFBCallback(void* callback, void* user_data) {
     if (vJoy.IsLoaded()) {
         vJoy.FfbRegisterGenCB((FfbGenCB)callback, user_data);
     }
}

} // namespace hid
