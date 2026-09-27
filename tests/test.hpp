#pragma once
#include <iostream>
#include <string>
#include <vector>
#include <utility>

namespace test {

struct TestResult {
    std::string name;
    bool passed;
    std::string error;
};

inline std::vector<TestResult>& results() {
    static std::vector<TestResult> r;
    return r;
}

inline void record_failure(const std::string& msg) {
    if (!results().empty())
        results().back().error = msg;
}

struct TestCase {
    std::string name;
    void (*fn)();
};

inline std::vector<TestCase>& cases() {
    static std::vector<TestCase> c;
    return c;
}

struct Registrar {
    Registrar(std::string name, void (*fn)()) {
        cases().push_back({std::move(name), fn});
    }
};

#define TEST(name)                                        \
    static void test_##name();                            \
    static ::test::Registrar reg_##name{#name, &test_##name}; \
    static void test_##name()

#define ASSERT_TRUE(expr) \
    do { \
        if (!(expr)) { \
            test::record_failure(std::string("ASSERT_TRUE failed: ") + #expr + " at " + __FILE__ + ":" + std::to_string(__LINE__)); \
            return; \
        } \
    } while(0)

#define ASSERT_FALSE(expr) ASSERT_TRUE(!(expr))

#define ASSERT_EQ(a, b) \
    do { \
        if ((a) != (b)) { \
            test::record_failure(std::string("ASSERT_EQ failed: ") + #a + " != " + #b + " at " + __FILE__ + ":" + std::to_string(__LINE__)); \
            return; \
        } \
    } while(0)

#define ASSERT_NEQ(a, b) \
    do { \
        if ((a) == (b)) { \
            test::record_failure(std::string("ASSERT_NEQ failed: ") + #a + " == " + #b + " at " + __FILE__ + ":" + std::to_string(__LINE__)); \
            return; \
        } \
    } while(0)

#define ASSERT_STREQ(a, b) \
    do { \
        if (std::string(a) != std::string(b)) { \
            test::record_failure(std::string("ASSERT_STREQ failed: \"") + (a) + "\" != \"" + (b) + "\" at " + __FILE__ + ":" + std::to_string(__LINE__)); \
            return; \
        } \
    } while(0)

#define ASSERT_LE(a, b) ASSERT_TRUE((a) <= (b))
#define ASSERT_GT(a, b) ASSERT_TRUE((a) > (b))

inline int run_all() {
    int failed = 0;
    for (const auto& tc : cases()) {
        results().push_back({tc.name, true, ""});
        tc.fn();
        auto& r = results().back();
        r.passed = r.error.empty();
        if (r.passed) {
            std::cout << "  ok   " << r.name << "\n";
        } else {
            ++failed;
            std::cout << "  FAIL " << r.name << ": " << r.error << "\n";
        }
    }
    std::cout << results().size() << " tests, " << failed << " failures\n";
    return failed == 0 ? 0 : 1;
}

} // namespace test
