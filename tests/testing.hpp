// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

// A small self-contained test framework: TEST() registers a function,
// CHECK*/REQUIRE* record failures, and run_all() executes the registry.

#pragma once

#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace tcrm::testing {

struct TestCase {
    std::string suite;
    std::string name;
    std::function<void()> fn;
};

std::vector<TestCase>& registry();

struct Registrar {
    Registrar(const char* suite, const char* name, std::function<void()> fn) {
        registry().push_back({suite, name, std::move(fn)});
    }
};

/// Thrown by REQUIRE* to abandon the current test.
struct AbortTest : std::runtime_error {
    using std::runtime_error::runtime_error;
};

void record_failure(const char* file, int line, const std::string& message);
int failures_in_current_test();

/// Runs every test whose suite matches `suite_filter` (empty = all).
int run_all(const std::string& suite_filter, const std::string& name_filter);

template <typename T>
std::string show(const T& v) {
    std::ostringstream os;
    os << v;
    return os.str();
}

inline std::string show(const std::string& v) {
    return "\"" + v + "\"";
}

inline std::string show(const char* v) {
    return v ? "\"" + std::string(v) + "\"" : "null";
}

inline std::string show(bool v) {
    return v ? "true" : "false";
}

}  // namespace tcrm::testing

#define TCRM_CONCAT_INNER(a, b) a##b
#define TCRM_CONCAT(a, b) TCRM_CONCAT_INNER(a, b)

#define TEST(suite, name)                                                                                   \
    static void TCRM_CONCAT(test_, TCRM_CONCAT(suite, TCRM_CONCAT(_, name)))();                             \
    static ::tcrm::testing::Registrar TCRM_CONCAT(registrar_, TCRM_CONCAT(suite, TCRM_CONCAT(_, name)))(    \
        #suite, #name, &TCRM_CONCAT(test_, TCRM_CONCAT(suite, TCRM_CONCAT(_, name))));                      \
    static void TCRM_CONCAT(test_, TCRM_CONCAT(suite, TCRM_CONCAT(_, name)))()

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) ::tcrm::testing::record_failure(__FILE__, __LINE__, "CHECK(" #cond ") failed"); \
    } while (0)

#define REQUIRE(cond)                                                                       \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            ::tcrm::testing::record_failure(__FILE__, __LINE__, "REQUIRE(" #cond ") failed"); \
            throw ::tcrm::testing::AbortTest("required condition failed");                  \
        }                                                                                   \
    } while (0)

#define CHECK_EQ(a, b)                                                                                     \
    do {                                                                                                   \
        const auto tcrm_a_ = (a);                                                                         \
        const auto tcrm_b_ = (b);                                                                         \
        if (!(tcrm_a_ == tcrm_b_)) {                                                                       \
            ::tcrm::testing::record_failure(__FILE__, __LINE__,                                            \
                                            "CHECK_EQ(" #a ", " #b ") failed: " +                         \
                                                ::tcrm::testing::show(tcrm_a_) + " vs " +                  \
                                                ::tcrm::testing::show(tcrm_b_));                           \
        }                                                                                                  \
    } while (0)

#define REQUIRE_EQ(a, b)                                                                                   \
    do {                                                                                                   \
        const auto tcrm_a_ = (a);                                                                         \
        const auto tcrm_b_ = (b);                                                                         \
        if (!(tcrm_a_ == tcrm_b_)) {                                                                       \
            ::tcrm::testing::record_failure(__FILE__, __LINE__,                                            \
                                            "REQUIRE_EQ(" #a ", " #b ") failed: " +                       \
                                                ::tcrm::testing::show(tcrm_a_) + " vs " +                  \
                                                ::tcrm::testing::show(tcrm_b_));                           \
            throw ::tcrm::testing::AbortTest("required equality failed");                                  \
        }                                                                                                  \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                                              \
    do {                                                                                                   \
        const double tcrm_a_ = (a);                                                                        \
        const double tcrm_b_ = (b);                                                                        \
        if (!(tcrm_a_ - tcrm_b_ <= (eps) && tcrm_b_ - tcrm_a_ <= (eps))) {                                 \
            ::tcrm::testing::record_failure(__FILE__, __LINE__,                                            \
                                            "CHECK_NEAR(" #a ", " #b ") failed: " +                       \
                                                ::tcrm::testing::show(tcrm_a_) + " vs " +                  \
                                                ::tcrm::testing::show(tcrm_b_));                           \
        }                                                                                                  \
    } while (0)

/// Checks that `expr` throws an exception of type `type`.
#define CHECK_THROWS_AS(expr, type)                                                                      \
    do {                                                                                                 \
        bool tcrm_threw_ = false;                                                                        \
        try {                                                                                            \
            (void)(expr);                                                                                \
        } catch (const type&) {                                                                          \
            tcrm_threw_ = true;                                                                          \
        } catch (...) {                                                                                  \
            ::tcrm::testing::record_failure(__FILE__, __LINE__, #expr " threw the wrong exception type"); \
            tcrm_threw_ = true;                                                                          \
        }                                                                                                \
        if (!tcrm_threw_) ::tcrm::testing::record_failure(__FILE__, __LINE__, #expr " did not throw");  \
    } while (0)

/// Checks that `expr` throws a tcrm::ApiError with the given error code.
#define CHECK_API_ERROR(expr, error_code)                                                                    \
    do {                                                                                                     \
        bool tcrm_threw_ = false;                                                                            \
        try {                                                                                                \
            (void)(expr);                                                                                    \
        } catch (const ::tcrm::ApiError& tcrm_e_) {                                                          \
            tcrm_threw_ = true;                                                                              \
            if (tcrm_e_.code() != (error_code)) {                                                            \
                ::tcrm::testing::record_failure(__FILE__, __LINE__,                                          \
                                                std::string(#expr " threw ") + tcrm_e_.code() + " (" +       \
                                                    tcrm_e_.what() + "), expected " + (error_code));         \
            }                                                                                                \
        }                                                                                                    \
        if (!tcrm_threw_) ::tcrm::testing::record_failure(__FILE__, __LINE__, #expr " did not throw");      \
    } while (0)
