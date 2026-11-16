// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "testing.hpp"

#include <chrono>
#include <exception>

#include "model/errors.hpp"
#include "util/log.hpp"

namespace tcrm::testing {
namespace {

int g_current_failures = 0;

}  // namespace

std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

void record_failure(const char* file, int line, const std::string& message) {
    ++g_current_failures;
    std::cerr << "    " << file << ":" << line << ": " << message << "\n";
}

int failures_in_current_test() {
    return g_current_failures;
}

int run_all(const std::string& suite_filter, const std::string& name_filter) {
    int passed = 0, failed = 0;
    const auto start = std::chrono::steady_clock::now();
    for (const TestCase& t : registry()) {
        if (!suite_filter.empty() && t.suite != suite_filter) continue;
        if (!name_filter.empty() && t.name.find(name_filter) == std::string::npos) continue;
        g_current_failures = 0;
        try {
            t.fn();
        } catch (const AbortTest&) {
            // Already recorded.
        } catch (const ApiError& e) {
            record_failure(t.suite.c_str(), 0, "unexpected ApiError " + e.code() + ": " + e.what());
        } catch (const std::exception& e) {
            record_failure(t.suite.c_str(), 0, std::string("unexpected exception: ") + e.what());
        }
        if (g_current_failures == 0) {
            ++passed;
        } else {
            ++failed;
            std::cerr << "FAILED " << t.suite << "." << t.name << "\n";
        }
    }
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::cout << passed << " passed, " << failed << " failed (" << secs << "s)\n";
    if (passed + failed == 0) {
        std::cerr << "no tests matched\n";
        return 1;
    }
    return failed == 0 ? 0 : 1;
}

}  // namespace tcrm::testing

int main(int argc, char** argv) {
    std::string suite, name;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--suite" && i + 1 < argc) {
            suite = argv[++i];
        } else if (arg == "--name" && i + 1 < argc) {
            name = argv[++i];
        } else if (arg == "--list") {
            for (const auto& t : tcrm::testing::registry()) std::cout << t.suite << "." << t.name << "\n";
            return 0;
        } else {
            std::cerr << "usage: tropicrm_tests [--suite SUITE] [--name SUBSTRING] [--list]\n";
            return 2;
        }
    }
    tcrm::log::Logger::instance().set_level(tcrm::log::Level::Off);
    return tcrm::testing::run_all(suite, name);
}
