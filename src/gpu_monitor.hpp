#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace lc {

struct GpuStats {
    bool available = false;
    std::string name;
    int util = -1;        // GPU utilization, percent
    long mem_used = -1;   // MB
    long mem_total = -1;  // MB
};

// Polls `nvidia-smi` on a background thread and exposes the latest snapshot.
// If nvidia-smi is missing or fails, stats stay unavailable.
class GpuMonitor {
public:
    GpuMonitor();   // starts the polling thread
    ~GpuMonitor();  // stops it

    GpuMonitor(const GpuMonitor&) = delete;
    GpuMonitor& operator=(const GpuMonitor&) = delete;

    GpuStats get();  // thread-safe copy of the latest snapshot

private:
    void run();
    void poll();

    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::mutex mu_;
    GpuStats stats_;
};

}  // namespace lc
