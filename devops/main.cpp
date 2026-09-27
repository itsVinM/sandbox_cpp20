#include <devops/redis_client.hpp>
#include <devops/sandbox.hpp>
#include <devops/metrics.hpp>
#include <devops/health_monitor.hpp>
#include <devops/log_aggregator.hpp>
#include <devops/resource_monitor.hpp>
#include <devops/process_supervisor.hpp>
#include <charconv>
#include <format>
#include <iostream>
#include <string_view>
#include <thread>
#include <chrono>
#include <cstring>
#include <signal.h>
#include <getopt.h>
#include <unistd.h>

static volatile sig_atomic_t g_running = 1;

static void signal_handler(int) {
    g_running = 0;
}

static void print_usage(const char* prog) {
    std::cerr << std::format(
        "Usage: {} [options]\n"
        "  -h, --host HOST       Redis host (default: 127.0.0.1)\n"
        "  -p, --port PORT       Redis port (default: 1234)\n"
        "  -i, --id ID           Sandbox ID (default: auto)\n"
        "  -t, --type TYPE       Target type (default: local)\n"
        "  -m, --metrics-port    Prometheus port (default: 9090)\n"
        "  -n, --no-seccomp      Disable seccomp filter\n"
        "  --max-restarts N      Max restarts on failure (default: 3)\n"
        "  --help                Show this help\n",
        prog);
}

struct Options {
    std::string host = "127.0.0.1";
    uint16_t port = 1234;
    std::string sandbox_id = "cpp-sandbox-1";
    std::string target_type = "local";
    uint16_t metrics_port = 9090;
    bool use_seccomp = true;
    uint32_t max_restarts = 3;
    bool help = false;
};

template <class T>
static devops::Result<T> parse_num(std::string_view raw) {
    T value{};
    auto [end, ec] = std::from_chars(raw.begin(), raw.end(), value);
    if (ec != std::errc{} || end != raw.end()) {
        return devops::Error{devops::Errc::config,
                             std::format("invalid number '{}'", raw)};
    }
    return value;
}

static devops::Result<Options> parse_args(int argc, char* argv[]) {
    Options o;

    static struct option long_opts[] = {
        {"host",          required_argument, nullptr, 'h'},
        {"port",          required_argument, nullptr, 'p'},
        {"id",            required_argument, nullptr, 'i'},
        {"type",          required_argument, nullptr, 't'},
        {"metrics-port",  required_argument, nullptr, 'm'},
        {"no-seccomp",    no_argument,       nullptr, 'n'},
        {"max-restarts",  required_argument, nullptr, 'r'},
        {"help",          no_argument,       nullptr, 'H'},
        {nullptr,         0,                 nullptr, 0},
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "h:p:i:t:m:nr:H", long_opts, nullptr)) != -1) {
        switch (opt) {
            case 'h': o.host = optarg; break;
            case 'i': o.sandbox_id = optarg; break;
            case 't': o.target_type = optarg; break;
            case 'n': o.use_seccomp = false; break;
            case 'H': o.help = true; return o;
            default:  return devops::Error{devops::Errc::config, "bad flag"};
            case 'p': {
                auto v = parse_num<uint64_t>(optarg);
                if (!v || *v > 65535)
                    return devops::Error{devops::Errc::config,
                                         std::format("invalid --port '{}'", optarg)};
                o.port = static_cast<uint16_t>(*v);
                break;
            }
            case 'm': {
                auto v = parse_num<uint64_t>(optarg);
                if (!v || *v > 65535)
                    return devops::Error{devops::Errc::config,
                                         std::format("invalid --metrics-port '{}'", optarg)};
                o.metrics_port = static_cast<uint16_t>(*v);
                break;
            }
            case 'r': {
                auto v = parse_num<uint32_t>(optarg);
                if (!v)
                    return devops::Error{devops::Errc::config,
                                         std::format("invalid --max-restarts '{}'", optarg)};
                o.max_restarts = *v;
                break;
            }
        }
    }
    return o;
}

int main(int argc, char* argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    auto parsed = parse_args(argc, argv);
    if (!parsed) {
        print_usage(argv[0]);
        std::cerr << std::format("[sandbox] {}\n", parsed.error().message);
        return 1;
    }

    auto [host, port, sandbox_id_default, target_type, metrics_port,
          use_seccomp, max_restarts, help_requested] = *parsed;

    if (help_requested) {
        print_usage(argv[0]);
        return 0;
    }

    std::string sandbox_id = sandbox_id_default.empty()
                                 ? std::format("cpp-sandbox-{}", ::getpid())
                                 : sandbox_id_default;

    std::cout << std::format(
        "[sandbox] id={} type={} host={}:{} metrics={} seccomp={}\n",
        sandbox_id, target_type, host, port, metrics_port, use_seccomp);

    devops::MetricsExporter metrics;
    devops::MetricsServer metrics_server(metrics_port);
    devops::HealthMonitor health_monitor;
    devops::LogAggregator log_agg;
    devops::ResourceManager resource_manager;
    devops::ProcessSupervisor process_supervisor;

    metrics_server.set_exporter(&metrics);

    health_monitor.register_check("redis", [&](std::string& msg) -> bool {
        msg = "ok";
        return true;
    });
    health_monitor.register_check("sandbox", [&](std::string& msg) -> bool {
        msg = "running";
        return true;
    });
    health_monitor.register_check("resources", [&](std::string& msg) -> bool {
        auto stats = resource_manager.get_global_stats();
        if (stats.sandboxes_over_limit > 0) {
            msg = std::format("{} sandboxes over limit", stats.sandboxes_over_limit);
            return false;
        }
        msg = "ok";
        return true;
    });

    if (metrics_server.start()) {
        std::cout << std::format("[sandbox] metrics on :{}/metrics\n", metrics_port);
    }

    health_monitor.set_interval_ms(5000);
    health_monitor.start();

    process_supervisor.on_event([&](const devops::ProcessEvent& e) {
        std::string type_str;
        switch (e.type) {
            case devops::ProcessEvent::Type::STARTED:    type_str = "STARTED"; break;
            case devops::ProcessEvent::Type::STOPPED:    type_str = "STOPPED"; break;
            case devops::ProcessEvent::Type::CRASHED:    type_str = "CRASHED"; break;
            case devops::ProcessEvent::Type::RESTARTED:  type_str = "RESTARTED"; break;
            case devops::ProcessEvent::Type::HEALTH_CHECK_FAILED: type_str = "HEALTH_FAIL"; break;
        }
        log_agg.push("supervisor", type_str,
            std::format("{}: pid={} exit={}", e.process_name, e.pid, e.exit_code));
        metrics.counter("supervisor.events_total", 1, {{"type", type_str}});
    });

    devops::RedisClient redis;
    if (auto conn = redis.connect(host, port); conn.is_err()) {
        std::cerr << std::format("[sandbox] failed to connect to {}:{}: {}\n",
                                 host, port, conn.error().message);
        return 1;
    }
    std::cout << "[sandbox] connected to Redis\n";

    auto reg = redis.sandbox_register(sandbox_id, target_type,
                                      std::format("{}:{}", host, port));
    if (!reg || reg->is_error()) {
        std::cerr << "[sandbox] failed to register\n";
        return 1;
    }
    std::cout << "[sandbox] registered\n";

    std::cout << "[sandbox] polling for jobs...\n";
    uint64_t jobs_completed = 0;
    uint64_t jobs_failed = 0;

    while (g_running) {
        auto job_resp = redis.job_next();
        if (!job_resp || job_resp->is_nil()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        if (job_resp->is_error()) {
            std::cerr << std::format("[sandbox] job_next error: {}\n", job_resp->err_msg);
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        const auto& arr = job_resp->as_arr();
        if (arr.size() < 5) {
            std::cerr << "[sandbox] malformed job response\n";
            continue;
        }

        std::string job_id = arr[0].as_str();
        std::string job_name = arr[1].as_str();
        std::string target = arr[2].as_str();
        std::string command = arr[3].as_str();

        log_agg.push("scheduler", "info", std::format("running job: {} ({})", job_id, job_name));
        metrics.counter("sandbox.jobs_started_total", 1, {{"target", target}});

        (void)redis.sandbox_claim(sandbox_id, job_id);

        devops::SandboxConfig cfg;
        cfg.id = job_id;
        cfg.enable_seccomp = use_seccomp;
        cfg.enable_network = (target == "network");

        (void)redis.job_log(job_id, "sandbox: starting execution");

        auto start = std::chrono::steady_clock::now();

        devops::Sandbox sandbox(cfg);
        auto result = sandbox.run(command, {}, [&](const std::string& line) {
            std::string truncated = line.substr(0, 1000);
            (void)redis.job_log(job_id, truncated);
            log_agg.push_raw("sandbox:" + job_id, line);
        });

        auto end = std::chrono::steady_clock::now();
        int64_t duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

        (void)redis.job_result(job_id, result.exit_code, duration_ms);
        metrics.counter("sandbox.jobs_completed_total", 1,
                       {{"target", target}, {"exit_code", std::to_string(result.exit_code)}});
        metrics.histogram("sandbox.execution_duration_ms", static_cast<double>(duration_ms));

        if (result.exit_code == 0) {
            jobs_completed++;
            log_agg.push("scheduler", "info", std::format("job {} PASSED ({}ms)", job_id, duration_ms));
        } else {
            jobs_failed++;
            log_agg.push("scheduler", "error", std::format("job {} FAILED (exit={})", job_id, result.exit_code));
        }

        metrics.gauge("sandbox.jobs_completed", static_cast<double>(jobs_completed));
        metrics.gauge("sandbox.jobs_failed", static_cast<double>(jobs_failed));

        (void)redis.sandbox_release(sandbox_id);

        std::string status = (result.exit_code == 0) ? "PASSED" : "FAILED";
        std::cout << std::format("[sandbox] job {} {} (exit={}, {}ms)\n",
                                 job_id, status, result.exit_code, duration_ms);

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "[sandbox] shutting down...\n";
    health_monitor.stop();
    metrics_server.stop();
    process_supervisor.stop_all();
    redis.close();

    std::cout << std::format("[sandbox] completed: {} jobs, {} failed\n",
                             jobs_completed, jobs_failed);
    return 0;
}
