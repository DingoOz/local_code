#include "gpu_monitor.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <sstream>
#include <vector>

namespace lc {

namespace {
std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
}  // namespace

GpuMonitor::GpuMonitor() : thread_([this] { run(); }) {}

GpuMonitor::~GpuMonitor() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

GpuStats GpuMonitor::get() {
    std::lock_guard<std::mutex> lk(mu_);
    return stats_;
}

void GpuMonitor::run() {
    while (!stop_) {
        poll();
        // Sleep ~1.5s, but wake promptly when asked to stop.
        for (int i = 0; i < 15 && !stop_; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void GpuMonitor::poll() {
    GpuStats s;
    FILE* p = popen(
        "nvidia-smi --query-gpu=name,utilization.gpu,memory.used,memory.total "
        "--format=csv,noheader,nounits 2>/dev/null",
        "r");
    if (p) {
        std::array<char, 512> buf{};
        if (std::fgets(buf.data(), buf.size(), p)) {
            std::stringstream ss(buf.data());
            std::vector<std::string> f;
            std::string field;
            while (std::getline(ss, field, ',')) f.push_back(trim(field));
            if (f.size() >= 4 && !f[0].empty()) {
                try {
                    s.name = f[0];
                    s.util = std::stoi(f[1]);
                    s.mem_used = std::stol(f[2]);
                    s.mem_total = std::stol(f[3]);
                    s.available = true;
                } catch (...) {
                    s.available = false;
                }
            }
        }
        pclose(p);
    }
    std::lock_guard<std::mutex> lk(mu_);
    stats_ = s;
}

}  // namespace lc
