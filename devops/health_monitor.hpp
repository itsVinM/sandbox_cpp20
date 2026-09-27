#pragma once
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>

namespace devops {

struct HealthStatus {
    std::string id;
    bool healthy;
    std::string message;
    std::chrono::steady_clock::time_point last_check;
};

using HealthCheckFn = std::function<bool(std::string&)>;

class HealthMonitor {
public:
    HealthMonitor();
    ~HealthMonitor();

    void register_check(const std::string& id, HealthCheckFn fn);
    void set_interval_ms(uint32_t ms);
    void start();
    void stop();

    std::vector<HealthStatus> get_all() const;
    bool is_healthy(const std::string& id) const;
    int unhealthy_count() const;

private:
    void monitor_loop();

    std::map<std::string, HealthCheckFn> checks_;
    mutable std::mutex mutex_;
    std::map<std::string, HealthStatus> statuses_;
    uint32_t interval_ms_ = 5000;
    std::thread thread_;
    std::atomic<bool> running_{false};
};

} // namespace devops
