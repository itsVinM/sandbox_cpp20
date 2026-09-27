#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <sys/types.h>

namespace devops {

struct ResourceLimits {
    uint64_t memory_bytes = 256 * 1024 * 1024;   // 256 MB
    uint32_t cpu_percent = 50;                     // 50% CPU
    uint32_t pids_max = 64;                        // max processes
    uint64_t disk_bytes = 1024 * 1024 * 1024;     // 1 GB disk
    uint64_t network_rx_bps = 10 * 1024 * 1024;   // 10 MB/s RX
    uint64_t network_tx_bps = 10 * 1024 * 1024;   // 10 MB/s TX
};

struct ResourceUsage {
    uint64_t memory_bytes;
    uint32_t cpu_percent;
    uint32_t pids_current;
    uint64_t disk_read_bytes;
    uint64_t disk_write_bytes;
    uint64_t network_rx_bytes;
    uint64_t network_tx_bytes;
    double uptime_seconds;
};

    struct ResourceAlert {
    enum class Level { INFO, WARNING, CRITICAL };
    Level level;
    std::string resource;  // "memory", "cpu", "pids", etc.
    std::string message;
    double current_value;
    double limit_value;
    uint64_t timestamp;
};

class ResourceMonitor {
public:
    ResourceMonitor(pid_t target_pid, const ResourceLimits& limits);
    ~ResourceMonitor();

    // Start/stop monitoring
    bool start(uint32_t interval_ms = 1000);
    bool stop();

    // Get current usage
    ResourceUsage get_usage() const;

    // Set alert callback
    void on_alert(std::function<void(const ResourceAlert&)> callback);

    // Get usage history
    struct HistoryEntry {
        ResourceUsage usage;
        double timestamp;
    };
    std::vector<HistoryEntry> get_history(uint32_t last_n = 100) const;

    // Check if resource limits are exceeded
    bool is_over_limit() const;
    std::vector<ResourceAlert> get_violations() const;

    // Dynamic limit adjustment
    bool set_limit(const ResourceLimits& limits);

private:
    void monitor_loop();
    ResourceUsage read_usage() const;
    void check_alerts(const ResourceUsage& usage);

    pid_t target_pid_;
    ResourceLimits limits_;
    std::atomic<bool> running_{false};
    std::thread monitor_thread_;

    mutable std::mutex history_mutex_;
    std::vector<HistoryEntry> history_;

    std::function<void(const ResourceAlert&)> alert_callback_;

    // Previous values for delta calculations
    mutable uint64_t prev_cpu_time_ = 0;
    mutable uint64_t prev_disk_read_ = 0;
    mutable uint64_t prev_disk_write_ = 0;
    mutable uint64_t prev_net_rx_ = 0;
    mutable uint64_t prev_net_tx_ = 0;
};

// Manages monitors for multiple sandboxes
class ResourceManager {
public:
    ResourceManager();
    ~ResourceManager();

    // Add/remove monitors
    bool add_monitor(const std::string& sandbox_id, pid_t pid, const ResourceLimits& limits);
    bool remove_monitor(const std::string& sandbox_id);

    // Get monitor by sandbox ID
    std::shared_ptr<ResourceMonitor> get_monitor(const std::string& sandbox_id);

    // Get all usage summaries
    struct SandboxUsage {
        std::string sandbox_id;
        ResourceUsage usage;
        bool over_limit;
    };
    std::vector<SandboxUsage> get_all_usage() const;

    // Global resource stats
    struct GlobalStats {
        uint64_t total_memory_used;
        uint32_t total_cpu_percent;
        uint32_t total_pids;
        uint64_t total_sandboxes;
        uint64_t sandboxes_over_limit;
    };
    GlobalStats get_global_stats() const;

    // Set global alert callback
    void on_alert(std::function<void(const std::string&, const ResourceAlert&)> callback);

private:
    std::unordered_map<std::string, std::shared_ptr<ResourceMonitor>> monitors_;
    std::function<void(const std::string&, const ResourceAlert&)> global_alert_callback_;
    mutable std::mutex monitors_mutex_;
};

} // namespace devops
