#pragma once
// Minimal Result<T> — the same shape as C++23's std::expected, so migrating
// later is a find-and-replace. Errors carry a category plus a human message;
// everything that can fail returns Result<T> instead of bool/optional, which
// forces call sites to acknowledge failure explicitly (`if (!r) ...`).
//
//   auto cfg = parse_args(argc, argv);
//   if (!cfg) { std::cerr << cfg.error().message; return 1; }
//   use(cfg->host);

#include <cerrno>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace devops {

enum class Errc : uint8_t {
    system,    // OS call failed (see .message for strerror)
    protocol,  // malformed wire data from the peer
    config,    // bad user input / configuration
    state,     // operation invalid in current state
};

struct Error {
    Errc code = Errc::system;
    std::string message;

    static Error system(const std::string& what) {
        return {Errc::system, std::string(what) + ": " + std::strerror(errno)};
    }
};

template <class T>
class [[nodiscard]] Result {
public:
    Result(T value) : v_(std::move(value)) {}
    Result(Error err) : v_(std::move(err)) {}

    explicit operator bool() const { return v_.index() == 0; }
    bool is_err() const { return v_.index() != 0; }

    T& operator*() { return std::get<0>(v_); }
    const T& operator*() const { return std::get<0>(v_); }
    T* operator->() { return &std::get<0>(v_); }
    const T* operator->() const { return &std::get<0>(v_); }

    T& value() { return std::get<0>(v_); }
    const T& value() const { return std::get<0>(v_); }
    const Error& error() const { return std::get<1>(v_); }

private:
    std::variant<T, Error> v_;
};

/// Void specialization for operations that fail or succeed without a payload.
template <>
class [[nodiscard]] Result<void> {
public:
    Result() = default;
    Result(Error err) : err_(std::move(err)) {}

    explicit operator bool() const { return !err_; }
    bool is_err() const { return err_.has_value(); }

    const Error& error() const { return *err_; }

private:
    std::optional<Error> err_;
};

} // namespace devops
