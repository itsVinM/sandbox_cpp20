#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <sys/types.h>

namespace devops {

struct ProcessConfig {
    std::string name;
    std::string command;
    std::vector<std::string> args;
    std::string working_dir = "/";
    std::unordered_map<std::string, std::string> environment;

    // Restart policy
    enum class RestartPolicy { NEVER, ON_FAILURE, ALWAYS };
    RestartPolicy restart_policy = RestartPolicy::ON_FAILURE;
    uint32_t max_restarts = 3;
    uint32_t restart_delay_ms = 1000;

    // Resource limits
    uint64_t memory_limit_bytes = 256 * 1024 * 1024;
    uint32_t cpu_percent = 50;
    uint32_t timeout_seconds = 300;  // 5 minutes

    // Health check
    bool enable_health_check = false;
    uint32_t health_check_interval_ms = 5000;
    std::string health_check_command;
};

struct ProcessState {
    enum class Status { STOPPED, STARTING, RUNNING, RESTARTING, FAILED };
    Status status = Status::STOPPED;
    pid_t pid = -1;
    int exit_code = 0;
    uint64_t start_time = 0;
    uint64_t restart_count = 0;
    std::string last_error;
    uint64_t cpu_time_ms = 0;
    uint64_t memory_bytes = 0;
};

struct ProcessEvent {
    enum class Type { STARTED, STOPPED, CRASHED, RESTARTED, HEALTH_CHECK_FAILED };
    Type type;
    std::string process_name;
    pid_t pid;
    int exit_code;
    uint64_t timestamp;
    std::string message;
};

class ProcessSupervisor {
public:
    ProcessSupervisor();
    ~ProcessSupervisor();

    // Add/remove processes
    bool add_process(const ProcessConfig& config);
    bool remove_process(const std::string& name);

    // Control processes
    bool start(const std::string& name);
    bool stop(const std::string& name, uint32_t timeout_ms = 5000);
    bool restart(const std::string& name);
    bool stop_all();

    // Get state
    ProcessState get_state(const std::string& name) const;
    std::vector<std::pair<std::string, ProcessState>> get_all_states() const;

    // Event callback
    void on_event(std::function<void(const ProcessEvent&)> callback);

    // Watchdog
    bool start_watchdog(uint32_t interval_ms = 1000);
    bool stop_watchdog();

    // Get process info
    struct ProcessInfo {
        std::string name;
        ProcessState state;
        ProcessConfig config;
        uint64_t uptime_ms;
        std::vector<std::string> recent_logs;
    };
    std::optional<ProcessInfo> get_info(const std::string& name) const;

private:
    void monitor_process(const std::string& name);
    void handle_exit(const std::string& name, int status);
    bool spawn_process(const std::string& name);
    void emit_event(ProcessEvent::Type type, const std::string& name,
                   pid_t pid, int exit_code, const std::string& msg = "");

    struct ProcessEntry {
        ProcessConfig config;
        ProcessState state;
        std::thread monitor_thread;
        std::atomic<bool> running{false};
        std::vector<std::string> logs;
    };

    std::unordered_map<std::string, std::shared_ptr<ProcessEntry>> processes_;
    mutable std::mutex processes_mutex_;

    std::function<void(const ProcessEvent&)> event_callback_;
    std::thread watchdog_thread_;
    std::atomic<bool> watchdog_running_{false};
};

} // namespace devops
