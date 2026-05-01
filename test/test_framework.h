#pragma once

#include <iostream>
#include <string>
#include <vector>
#include <functional>
#include <sstream>
#include <cmath>

namespace test {

struct TestCase {
    std::string name;
    std::function<void()> func;
};

inline std::vector<TestCase>& test_registry() {
    static std::vector<TestCase> registry;
    return registry;
}

struct TestRegistrar {
    TestRegistrar(const std::string& name, std::function<void()> func) {
        test_registry().push_back({name, func});
    }
};

#define TEST(category, name) \
    static void test_##category##_##name(); \
    static test::TestRegistrar registrar_##category##_##name(#category ":" #name, test_##category##_##name); \
    static void test_##category##_##name()

#define ASSERT_TRUE(expr) \
    do { \
        if (!(expr)) { \
            std::ostringstream oss; \
            oss << "Assertion failed: " #expr " at " << __FILE__ << ":" << __LINE__; \
            throw std::runtime_error(oss.str()); \
        } \
    } while (0)

#define ASSERT_FALSE(expr) ASSERT_TRUE(!(expr))

#define ASSERT_EQ(a, b) \
    do { \
        auto _a = (a); \
        auto _b = (b); \
        if (_a != _b) { \
            std::ostringstream oss; \
            oss << "Assertion failed: " #a " == " #b " at " << __FILE__ << ":" << __LINE__ \
                << " (" << _a << " != " << _b << ")"; \
            throw std::runtime_error(oss.str()); \
        } \
    } while (0)

#define ASSERT_NE(a, b) \
    do { \
        auto _a = (a); \
        auto _b = (b); \
        if (_a == _b) { \
            std::ostringstream oss; \
            oss << "Assertion failed: " #a " != " #b " at " << __FILE__ << ":" << __LINE__; \
            throw std::runtime_error(oss.str()); \
        } \
    } while (0)

#define ASSERT_LT(a, b) ASSERT_TRUE((a) < (b))
#define ASSERT_LE(a, b) ASSERT_TRUE((a) <= (b))
#define ASSERT_GT(a, b) ASSERT_TRUE((a) > (b))
#define ASSERT_GE(a, b) ASSERT_TRUE((a) >= (b))

inline int run_all_tests() {
    const auto& tests = test_registry();
    int passed = 0;
    int failed = 0;

    std::cout << "Running " << tests.size() << " tests...\n" << std::endl;

    for (const auto& test : tests) {
        std::cout << "[ RUN    ] " << test.name << std::endl;
        try {
            test.func();
            std::cout << "[     OK ] " << test.name << std::endl;
            ++passed;
        } catch (const std::exception& e) {
            std::cout << "[ FAILED ] " << test.name << " - " << e.what() << std::endl;
            ++failed;
        } catch (...) {
            std::cout << "[ FAILED ] " << test.name << " - unknown exception" << std::endl;
            ++failed;
        }
    }

    std::cout << "\n============================================" << std::endl;
    std::cout << "Tests passed: " << passed << std::endl;
    std::cout << "Tests failed: " << failed << std::endl;
    std::cout << "============================================" << std::endl;

    return failed > 0 ? 1 : 0;
}

} // namespace test
