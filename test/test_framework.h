/*
 * Minimal C11 test framework. Header-only. No external deps.
 *
 * Usage:
 *   #include "test_framework.h"
 *   SP_TEST(category, name) { ... ASSERT_EQ(a, b); ... }
 *   int main(void) { return sp_run_all_tests(); }
 *
 * Tests register themselves at static-init time via constructor attrs.
 * GCC, Clang, and MSVC all support the relevant ctors.
 */
#ifndef SPIRITTY_TEST_FRAMEWORK_H
#define SPIRITTY_TEST_FRAMEWORK_H

#include <inttypes.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
#  define SP_TEST_CTOR __attribute__((constructor))
#  define SP_TEST_UNUSED __attribute__((unused))
#else
#  define SP_TEST_CTOR
#  define SP_TEST_UNUSED
#endif

typedef void (*sp_test_fn)(void);

typedef struct sp_test_case {
    const char* name;
    sp_test_fn  fn;
} sp_test_case;

#ifndef SP_TEST_MAX
#  define SP_TEST_MAX 1024
#endif

static sp_test_case sp_test_registry_[SP_TEST_MAX];
static size_t       sp_test_count_ = 0;
static jmp_buf      sp_test_jmp_;
static const char*  sp_test_fail_msg_ = NULL;

static inline void sp_test_register(const char* name, sp_test_fn fn) {
    if (sp_test_count_ >= SP_TEST_MAX) {
        fprintf(stderr, "test framework: SP_TEST_MAX exceeded\n");
        abort();
    }
    sp_test_registry_[sp_test_count_].name = name;
    sp_test_registry_[sp_test_count_].fn   = fn;
    sp_test_count_++;
}

#define SP_TEST(category, name)                                            \
    static void test_##category##_##name##_fn(void);                       \
    static void SP_TEST_CTOR test_##category##_##name##_reg(void) {        \
        sp_test_register(#category ":" #name, test_##category##_##name##_fn); \
    }                                                                      \
    static void test_##category##_##name##_fn(void)

#define SP_FAIL(msg)                       \
    do {                                   \
        sp_test_fail_msg_ = (msg);         \
        longjmp(sp_test_jmp_, 1);          \
    } while (0)

#define ASSERT_TRUE(expr)                                                  \
    do {                                                                   \
        if (!(expr)) {                                                     \
            static char _msg[256];                                         \
            snprintf(_msg, sizeof _msg,                                    \
                     "ASSERT_TRUE(%s) at %s:%d", #expr, __FILE__, __LINE__);\
            SP_FAIL(_msg);                                                 \
        }                                                                  \
    } while (0)

#define ASSERT_FALSE(expr) ASSERT_TRUE(!(expr))

#define ASSERT_EQ_INT(a, b)                                                \
    do {                                                                   \
        long long _a = (long long)(a), _b = (long long)(b);                \
        if (_a != _b) {                                                    \
            static char _msg[256];                                         \
            snprintf(_msg, sizeof _msg,                                    \
                     "ASSERT_EQ(%s, %s) at %s:%d (%lld != %lld)",          \
                     #a, #b, __FILE__, __LINE__, _a, _b);                  \
            SP_FAIL(_msg);                                                 \
        }                                                                  \
    } while (0)

#define ASSERT_NE_INT(a, b)                                                \
    do {                                                                   \
        long long _a = (long long)(a), _b = (long long)(b);                \
        if (_a == _b) {                                                    \
            static char _msg[256];                                         \
            snprintf(_msg, sizeof _msg,                                    \
                     "ASSERT_NE(%s, %s) at %s:%d (both = %lld)",           \
                     #a, #b, __FILE__, __LINE__, _a);                      \
            SP_FAIL(_msg);                                                 \
        }                                                                  \
    } while (0)

#define ASSERT_LT(a, b) ASSERT_TRUE((a) < (b))
#define ASSERT_LE(a, b) ASSERT_TRUE((a) <= (b))
#define ASSERT_GT(a, b) ASSERT_TRUE((a) > (b))
#define ASSERT_GE(a, b) ASSERT_TRUE((a) >= (b))

#define ASSERT_STR_EQ(a, b)                                                \
    do {                                                                   \
        const char* _a = (a);                                              \
        const char* _b = (b);                                              \
        if (!_a || !_b || strcmp(_a, _b) != 0) {                           \
            static char _msg[512];                                         \
            snprintf(_msg, sizeof _msg,                                    \
                     "ASSERT_STR_EQ at %s:%d (\"%s\" != \"%s\")",          \
                     __FILE__, __LINE__,                                   \
                     _a ? _a : "(null)", _b ? _b : "(null)");              \
            SP_FAIL(_msg);                                                 \
        }                                                                  \
    } while (0)

static inline int sp_run_all_tests(void) {
    size_t passed = 0, failed = 0;
    printf("Running %zu tests...\n\n", sp_test_count_);
    for (size_t i = 0; i < sp_test_count_; ++i) {
        const sp_test_case* tc = &sp_test_registry_[i];
        printf("[ RUN    ] %s\n", tc->name);
        sp_test_fail_msg_ = NULL;
        if (setjmp(sp_test_jmp_) == 0) {
            tc->fn();
            printf("[     OK ] %s\n", tc->name);
            ++passed;
        } else {
            printf("[ FAILED ] %s — %s\n", tc->name,
                   sp_test_fail_msg_ ? sp_test_fail_msg_ : "(no message)");
            ++failed;
        }
    }
    printf("\n============================================\n");
    printf("Tests passed: %zu\n", passed);
    printf("Tests failed: %zu\n", failed);
    printf("============================================\n");
    return failed == 0 ? 0 : 1;
}

#endif /* SPIRITTY_TEST_FRAMEWORK_H */
