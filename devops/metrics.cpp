#include <devops/metrics.hpp>
#include <sstream>
#include <format>

#ifdef __linux__
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#endif

namespace devops {

// ── MetricsExporter ──

MetricsExporter::MetricsExporter() = default;

void MetricsExporter::counter(const std::string& name, double value, const std::map<std::string, std::string>& labels) {
    std::lock_guard lock(mutex_);
    metrics_.push_back({name, MetricType::Counter, value, labels});
}

void MetricsExporter::gauge(const std::string& name, double value, const std::map<std::string, std::string>& labels) {
    std::lock_guard lock(mutex_);
    metrics_.push_back({name, MetricType::Gauge, value, labels});
}

void MetricsExporter::histogram(const std::string& name, double value, const std::map<std::string, std::string>& labels) {
    std::lock_guard lock(mutex_);
    metrics_.push_back({name, MetricType::Histogram, value, labels});
}

static std::string render_labels(const std::map<std::string, std::string>& labels) {
    if (labels.empty()) return "";
    std::ostringstream ss;
    ss << "{";
    bool first = true;
    for (const auto& [k, v] : labels) {
        if (!first) ss << ",";
        ss << k << "=\"" << v << "\"";
        first = false;
    }
    ss << "}";
    return ss.str();
}

static const char* type_str(MetricType t) {
    switch (t) {
        case MetricType::Counter: return "counter";
        case MetricType::Gauge: return "gauge";
        case MetricType::Histogram: return "histogram";
    }
    return "untyped";
}

std::string MetricsExporter::render_prometheus() const {
    std::lock_guard lock(mutex_);
    std::ostringstream ss;

    // Group by name
    std::map<std::string, std::vector<const Metric*>> grouped;
    for (const auto& m : metrics_) {
        grouped[m.name].push_back(&m);
    }

    for (const auto& [name, metrics] : grouped) {
        if (!metrics.empty()) {
            ss << "# HELP " << name << " redisops metric\n";
            ss << "# TYPE " << name << " " << type_str(metrics[0]->type) << "\n";
        }
        for (const auto* m : metrics) {
            ss << m->name << render_labels(m->labels) << " " << m->value << "\n";
        }
        ss << "\n";
    }
    return ss.str();
}

std::string MetricsExporter::render_json() const {
    std::lock_guard lock(mutex_);
    std::ostringstream ss;
    ss << "[";
    bool first = true;
    for (const auto& m : metrics_) {
        if (!first) ss << ",";
        ss << std::format(R"({{"name":"{}","value":{}}})", m.name, m.value);
        first = false;
    }
    ss << "]";
    return ss.str();
}

// ── MetricsServer (Prometheus HTTP endpoint) ──

MetricsServer::MetricsServer(uint16_t port) : port_(port) {}

MetricsServer::~MetricsServer() { stop(); }

void MetricsServer::set_exporter(MetricsExporter* exporter) {
    exporter_ = exporter;
}

bool MetricsServer::start() {
#ifdef __linux__
    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) return false;

    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);

    if (bind(server_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    if (listen(server_fd_, 5) < 0) {
        close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    running_ = true;
    return true;
#else
    return false;
#endif
}

void MetricsServer::stop() {
    running_ = false;
#ifdef __linux__
    if (server_fd_ >= 0) {
        close(server_fd_);
        server_fd_ = -1;
    }
#endif
}

std::string MetricsServer::build_response(const std::string& body, int code) {
    const char* status = (code == 200) ? "OK" : "Not Found";
    return std::format(
        "HTTP/1.1 {} {}\r\n"
        "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n"
        "Content-Length: {}\r\n"
        "Connection: close\r\n"
        "\r\n{}",
        code, status, body.size(), body);
}

void MetricsServer::handle_client(int client_fd) {
#ifdef __linux__
    char buf[4096];
    ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) { close(client_fd); return; }
    buf[n] = '\0';

    std::string request(buf);
    std::string body;
    int code = 404;

    if (request.find("GET /metrics") != std::string::npos && exporter_) {
        body = exporter_->render_prometheus();
        code = 200;
    } else if (request.find("GET /health") != std::string::npos) {
        body = "ok\n";
        code = 200;
    }

    auto response = build_response(body, code);
    send(client_fd, response.c_str(), response.size(), 0);
    close(client_fd);
#endif
}

} // namespace devops
