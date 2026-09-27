#include <devops/log_aggregator.hpp>
#include <chrono>
#include <algorithm>

namespace devops {

LogAggregator::LogAggregator() = default;
LogAggregator::~LogAggregator() = default;

static uint64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void LogAggregator::push(const std::string& source, const std::string& level, const std::string& message) {
    std::lock_guard lock(mutex_);
    entries_.push_back({now_ms(), source, level, message});
    while (entries_.size() > max_entries_) {
        entries_.pop_front();
    }
}

void LogAggregator::push_raw(const std::string& source, const std::string& raw_line) {
    std::string level = "info";
    std::string msg = raw_line;

    // Detect log level from prefix
    if (raw_line.starts_with("[ERROR]") || raw_line.starts_with("ERROR:")) {
        level = "error";
        msg = raw_line.substr(raw_line.find(' ') + 1);
    } else if (raw_line.starts_with("[WARN]") || raw_line.starts_with("WARN:")) {
        level = "warn";
        msg = raw_line.substr(raw_line.find(' ') + 1);
    } else if (raw_line.starts_with("[DEBUG]") || raw_line.starts_with("DEBUG:")) {
        level = "debug";
        msg = raw_line.substr(raw_line.find(' ') + 1);
    }

    push(source, level, msg);
}

std::vector<LogEntry> LogAggregator::search(const std::string& query, size_t limit) const {
    std::lock_guard lock(mutex_);
    std::vector<LogEntry> results;
    for (auto it = entries_.rbegin(); it != entries_.rend() && results.size() < limit; ++it) {
        if (it->message.find(query) != std::string::npos) {
            results.push_back(*it);
        }
    }
    std::reverse(results.begin(), results.end());
    return results;
}

std::vector<LogEntry> LogAggregator::search_regex(const std::string& pattern, size_t limit) const {
    std::lock_guard lock(mutex_);
    std::vector<LogEntry> results;
    try {
        std::regex re(pattern);
        for (auto it = entries_.rbegin(); it != entries_.rend() && results.size() < limit; ++it) {
            if (std::regex_search(it->message, re)) {
                results.push_back(*it);
            }
        }
    } catch (...) {
        // Invalid regex, return empty
    }
    std::reverse(results.begin(), results.end());
    return results;
}

std::vector<LogEntry> LogAggregator::get_recent(size_t count) const {
    std::lock_guard lock(mutex_);
    std::vector<LogEntry> results;
    size_t start = entries_.size() > count ? entries_.size() - count : 0;
    for (size_t i = start; i < entries_.size(); i++) {
        results.push_back(entries_[i]);
    }
    return results;
}

std::vector<LogEntry> LogAggregator::get_by_source(const std::string& source, size_t limit) const {
    std::lock_guard lock(mutex_);
    std::vector<LogEntry> results;
    for (auto it = entries_.rbegin(); it != entries_.rend() && results.size() < limit; ++it) {
        if (it->source == source) {
            results.push_back(*it);
        }
    }
    std::reverse(results.begin(), results.end());
    return results;
}

void LogAggregator::set_max_entries(size_t max) {
    std::lock_guard lock(mutex_);
    max_entries_ = max;
}

size_t LogAggregator::count() const {
    std::lock_guard lock(mutex_);
    return entries_.size();
}

void LogAggregator::clear() {
    std::lock_guard lock(mutex_);
    entries_.clear();
}

} // namespace devops
