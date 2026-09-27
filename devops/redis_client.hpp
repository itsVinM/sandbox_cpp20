#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <devops/result.hpp>

namespace devops {

enum class ResponseTag : uint8_t {
    Nil   = 0,
    Error = 1,
    Str   = 2,
    Int   = 3,
    Dbl   = 4,
    Arr   = 5,
};

struct Response {
    ResponseTag tag;
    int32_t err_code = 0;
    std::string err_msg;
    std::string str;
    int64_t integer = 0;
    double dbl = 0.0;
    std::vector<Response> arr;

    bool is_nil() const { return tag == ResponseTag::Nil; }
    bool is_error() const { return tag == ResponseTag::Error; }
    const std::string& as_str() const { return str; }
    int64_t as_int() const { return integer; }
    double as_dbl() const { return dbl; }
    const std::vector<Response>& as_arr() const { return arr; }
};

class RedisClient {
public:
    RedisClient() = default;
    ~RedisClient();

    RedisClient(const RedisClient&) = delete;
    RedisClient& operator=(const RedisClient&) = delete;
    RedisClient(RedisClient&& other) noexcept;
    RedisClient& operator=(RedisClient&& other) noexcept;

    Result<void> connect(std::string_view host, uint16_t port);
    void close();
    bool is_connected() const;

    Result<Response> send(std::vector<std::string> args);

    Result<Response> set(std::string_view key, std::string_view val);
    Result<Response> get(std::string_view key);
    Result<Response> del(std::string_view key);
    Result<Response> lpush(std::string_view key, std::string_view val);
    Result<Response> rpop(std::string_view key);
    Result<Response> lrange(std::string_view key, int64_t start, int64_t stop);
    Result<Response> llen(std::string_view key);

    Result<Response> job_next();
    Result<Response> job_status(std::string_view id);
    Result<Response> job_result(std::string_view id, int exit_code, int64_t duration_ms);
    Result<Response> job_log(std::string_view id, std::string_view line);
    Result<Response> sandbox_register(std::string_view id, std::string_view type,
                                      std::string_view addr);
    Result<Response> sandbox_claim(std::string_view id, std::string_view job_id);
    Result<Response> sandbox_release(std::string_view id);

private:
    Result<void> write_all(const uint8_t* data, size_t len);
    Result<void> read_exact(uint8_t* buf, size_t len);
    Result<Response> read_frame();

    int fd_ = -1;
};

} // namespace devops
