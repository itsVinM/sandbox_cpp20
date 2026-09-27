#include <devops/health_monitor.hpp>
#include <chrono>

namespace devops {

HealthMonitor::HealthMonitor() = default;
HealthMonitor::~HealthMonitor() { stop(); }

void HealthMonitor::register_check(const std::string& id, HealthCheckFn fn) {
    std::lock_guard lock(mutex_);
    checks_[id] = std::move(fn);
    statuses_[id] = {id, true, "pending", std::chrono::steady_clock::now()};
}

void HealthMonitor::set_interval_ms(uint32_t ms) {
    interval_ms_ = ms;
}

void HealthMonitor::start() {
    if (running_) return;
    running_ = true;
    thread_ = std::thread(&HealthMonitor::monitor_loop, this);
}

void HealthMonitor::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

std::vector<HealthStatus> HealthMonitor::get_all() const {
    std::lock_guard lock(mutex_);
    std::vector<HealthStatus> result;
    for (const auto& [id, status] : statuses_) {
        result.push_back(status);
    }
    return result;
}

bool HealthMonitor::is_healthy(const std::string& id) const {
    std::lock_guard lock(mutex_);
    auto it = statuses_.find(id);
    return it != statuses_.end() && it->second.healthy;
}

int HealthMonitor::unhealthy_count() const {
    std::lock_guard lock(mutex_);
    int count = 0;
    for (const auto& [id, status] : statuses_) {
        if (!status.healthy) count++;
    }
    return count;
}

void HealthMonitor::monitor_loop() {
    while (running_) {
        std::lock_guard lock(mutex_);
        auto now = std::chrono::steady_clock::now();

        for (auto& [id, fn] : checks_) {
            std::string msg;
            bool healthy = fn(msg);

            statuses_[id] = {
                id,
                healthy,
                msg.empty() ? (healthy ? "ok" : "unhealthy") : msg,
                now
            };
        }

        // Sleep in small increments to allow quick shutdown
        lock.~lock_guard();
        for (int i = 0; i < interval_ms_ / 100 && running_; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

} // namespace devops
