#ifndef DEVICE_ENUMERATOR_H
#define DEVICE_ENUMERATOR_H

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class DeviceEnumerator {
public:
    using ScanCallback = std::function<void(std::vector<std::string>&&, bool force)>;

    explicit DeviceEnumerator(ScanCallback callback);
    ~DeviceEnumerator();

    DeviceEnumerator(const DeviceEnumerator&) = delete;
    DeviceEnumerator& operator=(const DeviceEnumerator&) = delete;

    void Start();
    void Stop();
    void RequestScan(bool force);
    std::vector<std::string> EnumerateNow() const;

private:
    void ThreadMain();
    static std::vector<std::string> EnumerateEventNodes();

    ScanCallback callback_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_;
    bool scan_requested_;
    bool force_requested_;
};

#endif  // DEVICE_ENUMERATOR_H
