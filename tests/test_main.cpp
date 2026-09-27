#include "test.hpp"

#include <stdexcept>

#include "devops/log_aggregator.hpp"
#include "devops/metrics.hpp"

// ── MetricsExporter ──────────────────────────────────────────────────────

TEST(metrics_counter_prometheus_render) {
    devops::MetricsExporter m;
    m.counter("jobs_total", 3, {{"type", "local"}});

    const auto out = m.render_prometheus();
    ASSERT_TRUE(out.find("# TYPE jobs_total counter") != std::string::npos);
    ASSERT_TRUE(out.find("jobs_total{type=\"local\"} 3") != std::string::npos);
}

TEST(metrics_gauge_and_histogram) {
    devops::MetricsExporter m;
    m.gauge("queue_depth", 7);
    m.histogram("latency_ms", 0.25);

    const auto out = m.render_prometheus();
    ASSERT_TRUE(out.find("# TYPE queue_depth gauge") != std::string::npos);
    ASSERT_TRUE(out.find("queue_depth 7") != std::string::npos);
    ASSERT_TRUE(out.find("latency_ms 0.25") != std::string::npos);
}

TEST(metrics_json_render_contains_series) {
    devops::MetricsExporter m;
    m.counter("errors_total", 42);

    const auto out = m.render_json();
    ASSERT_TRUE(out.find("errors_total") != std::string::npos);
    ASSERT_TRUE(out.find("42") != std::string::npos);
}

// ── LogAggregator ────────────────────────────────────────────────────────

TEST(log_push_and_count) {
    devops::LogAggregator log;
    log.push("api", "info", "request handled");
    log.push("worker", "error", "job timeout");

    ASSERT_EQ(log.count(), 2u);
}

TEST(log_search_substring) {
    devops::LogAggregator log;
    log.push("api", "info", "request handled");
    log.push("worker", "error", "job timeout after 30s");

    const auto hits = log.search("timeout");
    ASSERT_EQ(hits.size(), 1u);
    ASSERT_STREQ(hits[0].source, "worker");
}

TEST(log_search_regex_invalid_pattern_is_empty) {
    devops::LogAggregator log;
    log.push("api", "error", "boom");

    ASSERT_TRUE(log.search_regex("([invalid").empty());
}

TEST(log_get_by_source) {
    devops::LogAggregator log;
    log.push("api", "info", "one");
    log.push("worker", "info", "two");
    log.push("api", "warn", "three");

    const auto hits = log.get_by_source("api");
    ASSERT_EQ(hits.size(), 2u);
    ASSERT_STREQ(hits[0].message, "one");
}

TEST(log_max_entries_enforced) {
    devops::LogAggregator log;
    log.set_max_entries(2);
    log.push("s", "info", "a");
    log.push("s", "info", "b");
    log.push("s", "info", "c");

    ASSERT_EQ(log.count(), 2u); // oldest ("a") evicted
}

int main() {
    std::cout << "sandbox unit tests:\n";
    return test::run_all();
}
