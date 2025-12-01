#include "device_enumerator.h"

#include <chrono>
#include <dirent.h>
#include <cstring>
#include <string>
#include <utility>

namespace {
constexpr auto kScanInterval = std::chrono::milliseconds(400);
}

DeviceEnumerator::DeviceEnumerator(ScanCallback callback)
        : callback_(std::move(callback)), stop_(false), scan_requested_(false), force_requested_(false) {}

DeviceEnumerator::~DeviceEnumerator() {
    Stop();
}

void DeviceEnumerator::Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (thread_.joinable()) {
        return;
    }
    stop_ = false;
    thread_ = std::thread(&DeviceEnumerator::ThreadMain, this);
}

void DeviceEnumerator::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void DeviceEnumerator::RequestScan(bool force) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        scan_requested_ = true;
        force_requested_ = force_requested_ || force;
    }
    cv_.notify_all();
}

std::vector<std::string> DeviceEnumerator::EnumerateNow() const {
    return EnumerateEventNodes();
}

void DeviceEnumerator::ThreadMain() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stop_) {
        cv_.wait_for(lock, kScanInterval, [this]() {
            return stop_ || scan_requested_;
        });
        if (stop_) {
            break;
        }
        bool requested = scan_requested_;
        bool force = force_requested_;
        scan_requested_ = false;
        force_requested_ = false;
        lock.unlock();
        auto nodes = EnumerateEventNodes();
        if (callback_) {
            callback_(std::move(nodes), force || requested);
        }
        lock.lock();
    }
}

std::vector<std::string> DeviceEnumerator::EnumerateEventNodes() {
    std::vector<std::string> nodes;
    DIR* dir = opendir("/dev/input");
    if (!dir) {
        return nodes;
    }
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strncmp(entry->d_name, "event", 5) != 0) {
            continue;
        }
        nodes.emplace_back(std::string("/dev/input/") + entry->d_name);
    }
    closedir(dir);
    return nodes;
}
