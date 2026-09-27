#include <devops/resource_monitor.hpp>
#include <format>
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <unistd.h>
#include <sys/resource.h>

namespace fs = std::filesystem;

namespace devops {

// ── ResourceMonitor ──

ResourceMonitor::ResourceMonitor(pid_t target_pid, const ResourceLimits& limits)
    : target_pid_(target_pid), limits_(limits) {}

ResourceMonitor::~ResourceMonitor() {
    stop();
}

bool ResourceMonitor::start(uint32_t interval_ms) {
    if (running_) return false;

    running_ = true;
    monitor_thread_ = std::thread(&ResourceMonitor::monitor_loop, this);
    return true;
}

bool ResourceMonitor::stop() {
    if (!running_) return true;

    running_ = false;
    if (monitor_thread_.joinable()) {
        monitor_thread_.join();
    }
    return true;
}

ResourceUsage ResourceMonitor::read_usage() const {
    ResourceUsage usage = {};

    // Read from /proc/<pid>/status
    std::string status_path = std::format("/proc/{}/status", target_pid_);
    std::ifstream status_file(status_path);

    if (status_file.is_open()) {
        std::string line;
        while (std::getline(status_file, line)) {
            if (line.substr(0, 6) == "VmRSS:") {
                std::istringstream iss(line.substr(6));
                iss >> usage.memory_bytes;
                usage.memory_bytes *= 1024;  // Convert from KB
            }
            else if (line.substr(0, 4) == "Threads:") {
                std::istringstream iss(line.substr(4));
                iss >> usage.pids_current;
            }
        }
    }

    // Read CPU usage from /proc/<pid>/stat
    std::string stat_path = std::format("/proc/{}/stat", target_pid_);
    std::ifstream stat_file(stat_path);

    if (stat_file.is_open()) {
        std::string line;
        std::getline(stat_file, line);

        // Parse utime and stime (fields 14 and 15)
        std::istringstream iss(line);
        std::string token;
        for (int i = 1; i <= 13; i++) iss >> token;  // Skip to utime

        uint64_t utime = 0, stime = 0;
        iss >> utime >> stime;

        uint64_t total_time = utime + stime;
        uint64_t delta = total_time - prev_cpu_time_;
        prev_cpu_time_ = total_time;

        // Convert to percentage (assuming 100 Hz tick rate)
        usage.cpu_percent = static_cast<uint32_t>(delta * 100 / sysconf(_SC_CLK_TCK));
    }

    // Read disk I/O from /proc/<pid>/io
    std::string io_path = std::format("/proc/{}/io", target_pid_);
    std::ifstream io_file(io_path);

    if (io_file.is_open()) {
        std::string line;
        while (std::getline(io_file, line)) {
            if (line.substr(0, 11) == "read_bytes:") {
                std::istringstream iss(line.substr(11));
                iss >> usage.disk_read_bytes;
            }
            else if (line.substr(0, 12) == "write_bytes:") {
                std::istringstream iss(line.substr(12));
                iss >> usage.disk_write_bytes;
            }
        }
    }

    // Network stats (simplified - read from /proc/net/dev)
    // In production, would read per-interface stats
    usage.network_rx_bytes = 0;
    usage.network_tx_bytes = 0;

    // Uptime
    std::ifstream uptime_file("/proc/uptime");
    if (uptime_file.is_open()) {
        uptime_file >> usage.uptime_seconds;
    }

    return usage;
}

void ResourceMonitor::check_alerts(const ResourceUsage& usage) {
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    auto emit_alert = [&](ResourceAlert::Level level, const std::string& resource,
                         const std::string& msg, double current, double limit) {
        ResourceAlert alert;
        alert.level = level;
        alert.resource = resource;
        alert.message = msg;
        alert.current_value = current;
        alert.limit_value = limit;
        alert.timestamp = now;

        if (alert_callback_) {
            alert_callback_(alert);
        }
    };

    // Memory check
    double mem_ratio = static_cast<double>(usage.memory_bytes) / limits_.memory_bytes;
    if (mem_ratio > 0.9) {
        emit_alert(ResourceAlert::Level::CRITICAL, "memory",
                  "Memory usage critical", usage.memory_bytes, limits_.memory_bytes);
    } else if (mem_ratio > 0.75) {
        emit_alert(ResourceAlert::Level::WARNING, "memory",
                  "Memory usage high", usage.memory_bytes, limits_.memory_bytes);
    }

    // CPU check
    if (usage.cpu_percent > limits_.cpu_percent) {
        emit_alert(ResourceAlert::Level::WARNING, "cpu",
                  "CPU usage exceeded", usage.cpu_percent, limits_.cpu_percent);
    }

    // PID check
    if (usage.pids_current > limits_.pids_max) {
        emit_alert(ResourceAlert::Level::CRITICAL, "pids",
                  "Process limit exceeded", usage.pids_current, limits_.pids_max);
    }
}

void ResourceMonitor::monitor_loop() {
    while (running_) {
        auto usage = read_usage();

        {
            std::lock_guard<std::mutex> lock(history_mutex_);
            history_.push_back({usage, static_cast<double>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count())});

            // Keep only last 1000 entries
            if (history_.size() > 1000) {
                history_.erase(history_.begin());
            }
        }

        check_alerts(usage);

        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
}

ResourceUsage ResourceMonitor::get_usage() const {
    return read_usage();
}

void ResourceMonitor::on_alert(std::function<void(const ResourceAlert&)> callback) {
    alert_callback_ = std::move(callback);
}

std::vector<ResourceMonitor::HistoryEntry> ResourceMonitor::get_history(uint32_t last_n) const {
    std::lock_guard<std::mutex> lock(history_mutex_);
    std::vector<HistoryEntry> result;
    size_t start = history_.size() > last_n ? history_.size() - last_n : 0;
    result.reserve(last_n);
    for (size_t i = start; i < history_.size(); i++) {
        result.push_back(history_[i]);
    }
    return result;
}

bool ResourceMonitor::is_over_limit() const {
    auto usage = read_usage();
    return usage.memory_bytes > limits_.memory_bytes ||
           usage.cpu_percent > limits_.cpu_percent ||
           usage.pids_current > limits_.pids_max;
}

std::vector<ResourceAlert> ResourceMonitor::get_violations() const {
    std::vector<ResourceAlert> violations;
    auto usage = read_usage();

    if (usage.memory_bytes > limits_.memory_bytes) {
        violations.push_back({ResourceAlert::Level::CRITICAL, "memory",
                             "Memory limit exceeded",
                             static_cast<double>(usage.memory_bytes),
                             static_cast<double>(limits_.memory_bytes), 0});
    }

    if (usage.cpu_percent > limits_.cpu_percent) {
        violations.push_back({ResourceAlert::Level::WARNING, "cpu",
                             "CPU limit exceeded",
                             static_cast<double>(usage.cpu_percent),
                             static_cast<double>(limits_.cpu_percent), 0});
    }

    return violations;
}

bool ResourceMonitor::set_limit(const ResourceLimits& limits) {
    limits_ = limits;
    return true;
}

// ── ResourceManager ──

ResourceManager::ResourceManager() {}
ResourceManager::~ResourceManager() {}

bool ResourceManager::add_monitor(const std::string& sandbox_id, pid_t pid,
                                  const ResourceLimits& limits) {
    auto monitor = std::make_shared<ResourceMonitor>(pid, limits);

    monitor->on_alert([this, sandbox_id](const ResourceAlert& alert) {
        if (global_alert_callback_) {
            global_alert_callback_(sandbox_id, alert);
        }
    });

    std::lock_guard<std::mutex> lock(monitors_mutex_);
    monitors_[sandbox_id] = monitor;
    return monitor->start();
}

bool ResourceManager::remove_monitor(const std::string& sandbox_id) {
    std::lock_guard<std::mutex> lock(monitors_mutex_);
    auto it = monitors_.find(sandbox_id);
    if (it == monitors_.end()) return false;

    it->second->stop();
    monitors_.erase(it);
    return true;
}

std::shared_ptr<ResourceMonitor> ResourceManager::get_monitor(const std::string& sandbox_id) {
    std::lock_guard<std::mutex> lock(monitors_mutex_);
    auto it = monitors_.find(sandbox_id);
    return it != monitors_.end() ? it->second : nullptr;
}

std::vector<ResourceManager::SandboxUsage> ResourceManager::get_all_usage() const {
    std::lock_guard<std::mutex> lock(monitors_mutex_);
    std::vector<SandboxUsage> result;

    for (const auto& [id, monitor] : monitors_) {
        result.push_back({id, monitor->get_usage(), monitor->is_over_limit()});
    }

    return result;
}

ResourceManager::GlobalStats ResourceManager::get_global_stats() const {
    std::lock_guard<std::mutex> lock(monitors_mutex_);
    GlobalStats stats = {};

    for (const auto& [id, monitor] : monitors_) {
        auto usage = monitor->get_usage();
        stats.total_memory_used += usage.memory_bytes;
        stats.total_cpu_percent += usage.cpu_percent;
        stats.total_pids += usage.pids_current;
        stats.total_sandboxes++;
        if (monitor->is_over_limit()) {
            stats.sandboxes_over_limit++;
        }
    }

    return stats;
}

void ResourceManager::on_alert(std::function<void(const std::string&, const ResourceAlert&)> callback) {
    global_alert_callback_ = std::move(callback);
}

} // namespace devops
