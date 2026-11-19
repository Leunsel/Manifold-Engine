#pragma once

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace Manifold::Tests
{
    struct Counter
    {
        int Checks = 0;
        int Failures = 0;
        int Skipped = 0;
    };

    Counter& Totals();

    inline void Report(bool ok, const char* file, int line, const std::string& what)
    {
        ++Totals().Checks;
        if (ok)
            return;
        ++Totals().Failures;
        std::printf("  FAIL %s:%d  %s\n", file, line, what.c_str());
    }

    inline void Skip(const std::string& what)
    {
        ++Totals().Skipped;
        std::printf("  SKIP %s\n", what.c_str());
    }

    struct SuiteContext
    {
        std::filesystem::path Target;
    };

    struct Suite
    {
        const char* Name;
        const char* Title;
        int Order;
        int (*Run)(const SuiteContext& context);
    };

    struct Mode
    {
        const char* Flag;
        int (*Run)(const std::vector<std::string>& arguments);
    };

    std::vector<Suite>& Suites();
    std::vector<Mode>& Modes();

    struct SuiteRegistration
    {
        explicit SuiteRegistration(Suite suite) { Suites().push_back(suite); }
    };

    struct ModeRegistration
    {
        explicit ModeRegistration(Mode mode) { Modes().push_back(mode); }
    };
}

#define CHECK(condition) ::Manifold::Tests::Report(static_cast<bool>(condition), __FILE__, __LINE__, #condition)
#define CHECK_EQ(actual, expected) ::Manifold::Tests::Report((actual) == (expected), __FILE__, __LINE__, std::string(#actual " == " #expected))
#define CHECK_MSG(condition, message) ::Manifold::Tests::Report(static_cast<bool>(condition), __FILE__, __LINE__, std::string(#condition) + "  " + (message))

#define MANIFOLD_TEST_JOIN2(a, b) a##b
#define MANIFOLD_TEST_JOIN(a, b) MANIFOLD_TEST_JOIN2(a, b)
#define MANIFOLD_SUITE(name, title, order, ...) \
    static const ::Manifold::Tests::SuiteRegistration MANIFOLD_TEST_JOIN(g_suite, __LINE__)(::Manifold::Tests::Suite{ name, title, order, __VA_ARGS__ })
#define MANIFOLD_MODE(flag, ...) \
    static const ::Manifold::Tests::ModeRegistration MANIFOLD_TEST_JOIN(g_mode, __LINE__)(::Manifold::Tests::Mode{ flag, __VA_ARGS__ })
