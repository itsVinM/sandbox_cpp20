#include <devops/process_supervisor.hpp>
#include <format>
#include <iostream>
#include <fstream>
#include <chrono>
#include <algorithm>
#include <cstring>
#include <csignal>
#include <sys/wait.h>
#include <sys/resource.h>
#include <unistd.h>
#include <fcntl.h>

namespace devops {

ProcessSupervisor::ProcessSupervisor() {}
ProcessSupervisor::~ProcessSupervisor() {
    stop_all();
    stop_watchdog();
}

bool ProcessSupervisor::add_process(const ProcessConfig& config) {
    std::lock_guard<std::mutex> lock(processes_mutex_);

    if (processes_.count(config.name)) {
        return false;
    }

    auto entry = std::make_shared<ProcessEntry>();
    entry->config = config;
    entry->state.status = ProcessState::Status::STOPPED;
    processes_[config.name] = entry;

    return true;
}

bool ProcessSupervisor::remove_process(const std::string& name) {
    std::lock_guard<std::mutex> lock(processes_mutex_);
    auto it = processes_.find(name);
    if (it == processes_.end()) return false;

    if (it->second->state.status == ProcessState::Status::RUNNING) {
        stop(name);
    }

    processes_.erase(it);
    return true;
}

bool ProcessSupervisor::start(const std::string& name) {
    std::lock_guard<std::mutex> lock(processes_mutex_);
    auto it = processes_.find(name);
    if (it == processes_.end()) return false;

    auto& entry = it->second;
    if (entry->state.status == ProcessState::Status::RUNNING) {
        return false;
    }

    return spawn_process(name);
}

bool ProcessSupervisor::spawn_process(const std::string& name) {
    auto& entry = processes_[name];
    auto& config = entry->config;

    entry->state.status = ProcessState::Status::STARTING;
    entry->state.restart_count = 0;

    pid_t pid = fork();
    if (pid < 0) {
        entry->state.status = ProcessState::Status::FAILED;
        entry->state.last_error = strerror(errno);
        return false;
    }

    if (pid == 0) {
        // Child process

        // Set working directory
        if (!config.working_dir.empty()) {
            chdir(config.working_dir.c_str());
        }

        // Set environment variables
        for (const auto& [key, value] : config.environment) {
            setenv(key.c_str(), value.c_str(), 1);
        }

        // Set resource limits
        struct rlimit rl;

        // Memory limit
        rl.rlim_cur = rl.rlim_max = config.memory_limit_bytes;
        setrlimit(RLIMIT_AS, &rl);

        // Process limit
        rl.rlim_cur = rl.rlim_max = 1;
        setrlimit(RLIMIT_NPROC, &rl);

        // Execute command
        std::vector<const char*> args;
        args.push_back(config.command.c_str());
        for (const auto& arg : config.args) {
            args.push_back(arg.c_str());
        }
        args.push_back(nullptr);

        execvp(config.command.c_str(), const_cast<char* const*>(args.data()));
        _exit(127);
    }

    // Parent process
    entry->state.pid = pid;
    entry->state.status = ProcessState::Status::RUNNING;
    entry->state.start_time = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    entry->running = true;

    // Start monitor thread
    if (entry->monitor_thread.joinable()) {
        entry->monitor_thread.join();
    }
    entry->monitor_thread = std::thread(&ProcessSupervisor::monitor_process, this, name);

    emit_event(ProcessEvent::Type::STARTED, name, pid, 0);
    std::cout << std::format("[supervisor] started: {} (pid={})\n", name, pid);

    return true;
}

void ProcessSupervisor::monitor_process(const std::string& name) {
    auto& entry = processes_[name];

    while (entry->running) {
        int status;
        pid_t result = waitpid(entry->state.pid, &status, WNOHANG);

        if (result > 0) {
            handle_exit(name, status);
            return;
        }

        // Check timeout
        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        uint64_t uptime = now - entry->state.start_time;

        if (entry->config.timeout_seconds > 0 &&
            uptime > entry->config.timeout_seconds * 1000) {
            std::cerr << std::format("[supervisor] {} timeout after {}s\n",
                                     name, entry->config.timeout_seconds);
            kill(entry->state.pid, SIGKILL);
            return;
        }

        // Update resource usage
        std::string status_path = std::format("/proc/{}/status", entry->state.pid);
        std::ifstream status_file(status_path);
        if (status_file.is_open()) {
            std::string line;
            while (std::getline(status_file, line)) {
                if (line.substr(0, 6) == "VmRSS:") {
                    std::istringstream iss(line.substr(6));
                    iss >> entry->state.memory_bytes;
                    entry->state.memory_bytes *= 1024;
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

void ProcessSupervisor::handle_exit(const std::string& name, int status) {
    auto& entry = processes_[name];

    entry->running = false;
    entry->state.exit_code = WEXITSTATUS(status);
    entry->state.status = ProcessState::Status::STOPPED;

    bool success = (entry->state.exit_code == 0);

    emit_event(success ? ProcessEvent::Type::STOPPED : ProcessEvent::Type::CRASHED,
              name, entry->state.pid, entry->state.exit_code);

    std::cout << std::format("[supervisor] {} exited (code={})\n",
                             name, entry->state.exit_code);

    // Restart logic
    if (!success && entry->config.restart_policy != ProcessConfig::RestartPolicy::NEVER) {
        if (entry->state.restart_count < entry->config.max_restarts) {
            entry->state.restart_count++;
            entry->state.status = ProcessState::Status::RESTARTING;

            std::cout << std::format("[supervisor] restarting {} (attempt {}/{})\n",
                                     name, entry->state.restart_count,
                                     entry->config.max_restarts);

            emit_event(ProcessEvent::Type::RESTARTED, name, -1, 0,
                      std::format("Restart attempt {}", entry->state.restart_count));

            std::this_thread::sleep_for(
                std::chrono::milliseconds(entry->config.restart_delay_ms));

            spawn_process(name);
        } else {
            entry->state.status = ProcessState::Status::FAILED;
            entry->state.last_error = std::format("Exceeded max restarts ({})",
                                                  entry->config.max_restarts);

            std::cerr << std::format("[supervisor] {} failed: {}\n",
                                     name, entry->state.last_error);
        }
    }
}

bool ProcessSupervisor::stop(const std::string& name, uint32_t timeout_ms) {
    std::lock_guard<std::mutex> lock(processes_mutex_);
    auto it = processes_.find(name);
    if (it == processes_.end()) return false;

    auto& entry = it->second;
    if (entry->state.status != ProcessState::Status::RUNNING) {
        return false;
    }

    entry->running = false;

    // Send SIGTERM
    kill(entry->state.pid, SIGTERM);

    // Wait for graceful shutdown
    auto start = std::chrono::steady_clock::now();
    while (entry->state.status == ProcessState::Status::RUNNING) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed > timeout_ms) {
            // Force kill
            kill(entry->state.pid, SIGKILL);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    entry->state.status = ProcessState::Status::STOPPED;
    std::cout << std::format("[supervisor] stopped: {}\n", name);
    return true;
}

bool ProcessSupervisor::restart(const std::string& name) {
    stop(name);
    return start(name);
}

bool ProcessSupervisor::stop_all() {
    std::lock_guard<std::mutex> lock(processes_mutex_);
    for (auto& [name, entry] : processes_) {
        if (entry->state.status == ProcessState::Status::RUNNING) {
            entry->running = false;
            kill(entry->state.pid, SIGTERM);
        }
    }
    return true;
}

ProcessState ProcessSupervisor::get_state(const std::string& name) const {
    std::lock_guard<std::mutex> lock(processes_mutex_);
    auto it = processes_.find(name);
    if (it == processes_.end()) return ProcessState{};
    return it->second->state;
}

std::vector<std::pair<std::string, ProcessState>>
ProcessSupervisor::get_all_states() const {
    std::lock_guard<std::mutex> lock(processes_mutex_);
    std::vector<std::pair<std::string, ProcessState>> result;
    for (const auto& [name, entry] : processes_) {
        result.push_back({name, entry->state});
    }
    return result;
}

void ProcessSupervisor::on_event(std::function<void(const ProcessEvent&)> callback) {
    event_callback_ = std::move(callback);
}

void ProcessSupervisor::emit_event(ProcessEvent::Type type, const std::string& name,
                                   pid_t pid, int exit_code, const std::string& msg) {
    if (event_callback_) {
        ProcessEvent event;
        event.type = type;
        event.process_name = name;
        event.pid = pid;
        event.exit_code = exit_code;
        event.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        event.message = msg;
        event_callback_(event);
    }
}

std::optional<ProcessSupervisor::ProcessInfo>
ProcessSupervisor::get_info(const std::string& name) const {
    std::lock_guard<std::mutex> lock(processes_mutex_);
    auto it = processes_.find(name);
    if (it == processes_.end()) return std::nullopt;

    const auto& entry = it->second;
    ProcessInfo info;
    info.name = name;
    info.state = entry->state;
    info.config = entry->config;

    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    info.uptime_ms = entry->state.start_time > 0 ? now - entry->state.start_time : 0;
    info.recent_logs = entry->logs;

    return info;
}

bool ProcessSupervisor::start_watchdog(uint32_t interval_ms) {
    if (watchdog_running_) return false;
    watchdog_running_ = true;
    watchdog_thread_ = std::thread([this, interval_ms]() {
        while (watchdog_running_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
            std::lock_guard<std::mutex> lock(processes_mutex_);
            for (auto& [name, entry] : processes_) {
                if (entry->state.status == ProcessState::Status::RUNNING && entry->state.pid > 0) {
                    int status;
                    pid_t result = waitpid(entry->state.pid, &status, WNOHANG);
                    if (result > 0) {
                        handle_exit(name, WEXITSTATUS(status));
                    }
                }
            }
        }
    });
    return true;
}

bool ProcessSupervisor::stop_watchdog() {
    watchdog_running_ = false;
    if (watchdog_thread_.joinable()) watchdog_thread_.join();
    return true;
}

} // namespace devops
