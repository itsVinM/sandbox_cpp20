#include <devops/redis_client.hpp>

#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <format>
#include <utility>

namespace devops {
namespace {

constexpr uint32_t MAX_FRAME = 64 * 1024 * 1024;

template <class T>
T load_le(const uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

void put_le(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }

Result<Response> parse_value(std::string_view buf, size_t& pos);

} // namespace

RedisClient::~RedisClient() { close(); }

RedisClient::RedisClient(RedisClient&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

RedisClient& RedisClient::operator=(RedisClient&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
}

Result<void> RedisClient::connect(std::string_view host, uint16_t port) {
    close();

    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
        return Error::system("socket");
    }

    int flag = 1;
    ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, std::string(host).c_str(), &addr.sin_addr) != 1) {
        int saved = EINVAL;
        close();
        errno = saved;
        return Error::system(std::format("invalid host '{}'", host));
    }

    if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        return Error::system(std::format("connect {}:{}", host, port));
    }
    return {};
}

void RedisClient::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool RedisClient::is_connected() const { return fd_ >= 0; }

Result<void> RedisClient::write_all(const uint8_t* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd_, data + sent, len - sent, 0);
        if (n <= 0) {
            return Error::system("send");
        }
        sent += static_cast<size_t>(n);
    }
    return {};
}

Result<void> RedisClient::read_exact(uint8_t* buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = ::recv(fd_, buf + got, len - got, 0);
        if (n <= 0) {
            return Error::system("recv");
        }
        got += static_cast<size_t>(n);
    }
    return {};
}

Result<Response> RedisClient::send(std::vector<std::string> args) {
    if (fd_ < 0) {
        return Error{Errc::state, "not connected"};
    }

    uint32_t payload = 4;
    for (const auto& arg : args) {
        payload += 4 + static_cast<uint32_t>(arg.size());
    }

    std::vector<uint8_t> msg(4 + payload);
    put_le(msg.data(), payload);
    put_le(msg.data() + 4, static_cast<uint32_t>(args.size()));
    size_t off = 8;
    for (const auto& arg : args) {
        put_le(msg.data() + off, static_cast<uint32_t>(arg.size()));
        std::memcpy(msg.data() + off + 4, arg.data(), arg.size());
        off += 4 + arg.size();
    }

    if (auto w = write_all(msg.data(), msg.size()); w.is_err()) {
        return w.error();
    }
    return read_frame();
}

Result<Response> RedisClient::read_frame() {
    uint8_t header[4];
    if (auto r = read_exact(header, 4); r.is_err()) {
        return r.error();
    }
    const uint32_t len = load_le<uint32_t>(header);
    if (len > MAX_FRAME) {
        return Error{Errc::protocol, std::format("frame of {} bytes exceeds limit", len)};
    }

    std::vector<uint8_t> body(len);
    if (auto r = read_exact(body.data(), len); r.is_err()) {
        return r.error();
    }

    size_t pos = 0;
    return parse_value({reinterpret_cast<const char*>(body.data()), body.size()}, pos);
}

namespace {

Result<Response> parse_value(std::string_view buf, size_t& pos) {
    auto need = [&](size_t n) -> Result<void> {
        if (buf.size() - pos < n) {
            return Error{Errc::protocol, "truncated response"};
        }
        return {};
    };

    if (pos >= buf.size()) {
        return Error{Errc::protocol, "empty response body"};
    }

    Response out;
    out.tag = static_cast<ResponseTag>(buf[pos++]);
    const uint8_t* p = reinterpret_cast<const uint8_t*>(buf.data()) + pos;

    switch (out.tag) {
        case ResponseTag::Nil:
            return out;

        case ResponseTag::Error:
            if (auto g = need(8); g.is_err()) return g.error();
            out.err_code = static_cast<int32_t>(load_le<uint32_t>(p));
            pos += 4;
            p += 4;
            {
                const uint32_t msg_len = load_le<uint32_t>(p);
                pos += 4;
                p += 4;
                if (auto g = need(msg_len); g.is_err()) return g.error();
                out.err_msg.assign(buf.data() + pos, msg_len);
                pos += msg_len;
            }
            return out;

        case ResponseTag::Str: {
            if (auto g = need(4); g.is_err()) return g.error();
            const uint32_t slen = load_le<uint32_t>(p);
            pos += 4;
            p += 4;
            if (auto g = need(slen); g.is_err()) return g.error();
            out.str.assign(buf.data() + pos, slen);
            pos += slen;
            return out;
        }

        case ResponseTag::Int:
            if (auto g = need(8); g.is_err()) return g.error();
            out.integer = static_cast<int64_t>(load_le<uint64_t>(p));
            pos += 8;
            return out;

        case ResponseTag::Dbl:
            if (auto g = need(8); g.is_err()) return g.error();
            std::memcpy(&out.dbl, p, 8);
            pos += 8;
            return out;

        case ResponseTag::Arr: {
            if (auto g = need(4); g.is_err()) return g.error();
            const uint32_t count = load_le<uint32_t>(p);
            pos += 4;
            out.arr.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                auto elem = parse_value(buf, pos);
                if (elem.is_err()) {
                    return elem;
                }
                out.arr.push_back(std::move(elem.value()));
            }
            return out;
        }
    }
    return Error{Errc::protocol, std::format("unknown tag {}", static_cast<int>(out.tag))};
}

} // namespace

Result<Response> RedisClient::set(std::string_view key, std::string_view val) {
    return send({std::string("set"), std::string(key), std::string(val)});
}

Result<Response> RedisClient::get(std::string_view key) {
    return send({std::string("get"), std::string(key)});
}

Result<Response> RedisClient::del(std::string_view key) {
    return send({std::string("del"), std::string(key)});
}

Result<Response> RedisClient::lpush(std::string_view key, std::string_view val) {
    return send({std::string("lpush"), std::string(key), std::string(val)});
}

Result<Response> RedisClient::rpop(std::string_view key) {
    return send({std::string("rpop"), std::string(key)});
}

Result<Response> RedisClient::lrange(std::string_view key, int64_t start, int64_t stop) {
    return send({std::string("lrange"), std::string(key), std::to_string(start),
                 std::to_string(stop)});
}

Result<Response> RedisClient::llen(std::string_view key) {
    return send({std::string("llen"), std::string(key)});
}

Result<Response> RedisClient::job_next() {
    return send({std::string("job next")});
}

Result<Response> RedisClient::job_status(std::string_view id) {
    return send({std::string("job status"), std::string(id)});
}

Result<Response> RedisClient::job_result(std::string_view id, int exit_code,
                                         int64_t duration_ms) {
    return send({std::string("job result"), std::string(id), std::to_string(exit_code),
                 std::to_string(duration_ms)});
}

Result<Response> RedisClient::job_log(std::string_view id, std::string_view line) {
    return send({std::string("job log"), std::string(id), std::string(line)});
}

Result<Response> RedisClient::sandbox_register(std::string_view id, std::string_view type,
                                               std::string_view addr) {
    return send({std::string("sandbox register"), std::string(id), std::string(type),
                 std::string(addr)});
}

Result<Response> RedisClient::sandbox_claim(std::string_view id, std::string_view job_id) {
    return send({std::string("sandbox claim"), std::string(id), std::string(job_id)});
}

Result<Response> RedisClient::sandbox_release(std::string_view id) {
    return send({std::string("sandbox release"), std::string(id)});
}

} // namespace devops
