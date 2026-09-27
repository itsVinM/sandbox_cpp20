#pragma once
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <functional>
#include <regex>

namespace devops {

struct LogEntry {
    uint64_t timestamp_ms;
    std::string source;
    std::string level;
    std::string message;
};

class LogAggregator {
public:
    LogAggregator();
    ~LogAggregator();

    void push(const std::string& source, const std::string& level, const std::string& message);
    void push_raw(const std::string& source, const std::string& raw_line);

    std::vector<LogEntry> search(const std::string& query, size_t limit = 100) const;
    std::vector<LogEntry> search_regex(const std::string& pattern, size_t limit = 100) const;
    std::vector<LogEntry> get_recent(size_t count = 50) const;
    std::vector<LogEntry> get_by_source(const std::string& source, size_t limit = 100) const;

    void set_max_entries(size_t max);
    size_t count() const;
    void clear();

private:
    std::deque<LogEntry> entries_;
    mutable std::mutex mutex_;
    size_t max_entries_ = 10000;
};

} // namespace devops
