#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <functional>

namespace devops {

enum class MetricType { Counter, Gauge, Histogram };

struct Metric {
    std::string name;
    MetricType type;
    double value = 0.0;
    std::map<std::string, std::string> labels;
};

class MetricsExporter {
public:
    MetricsExporter();

    void counter(const std::string& name, double value, const std::map<std::string, std::string>& labels = {});
    void gauge(const std::string& name, double value, const std::map<std::string, std::string>& labels = {});
    void histogram(const std::string& name, double value, const std::map<std::string, std::string>& labels = {});

    std::string render_prometheus() const;
    std::string render_json() const;

private:
    mutable std::mutex mutex_;
    std::vector<Metric> metrics_;
};

class MetricsServer {
public:
    MetricsServer(uint16_t port = 9090);
    ~MetricsServer();

    bool start();
    void stop();
    void set_exporter(MetricsExporter* exporter);

private:
    void handle_client(int client_fd);
    std::string build_response(const std::string& body, int code);

    uint16_t port_;
    int server_fd_ = -1;
    MetricsExporter* exporter_ = nullptr;
    bool running_ = false;
};

} // namespace devops
