#pragma once

#include <cstdio>
#include <string>

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
}

#define CHECK(condition) ::Manifold::Tests::Report(static_cast<bool>(condition), __FILE__, __LINE__, #condition)
#define CHECK_EQ(actual, expected) ::Manifold::Tests::Report((actual) == (expected), __FILE__, __LINE__, std::string(#actual " == " #expected))
#define CHECK_MSG(condition, message) ::Manifold::Tests::Report(static_cast<bool>(condition), __FILE__, __LINE__, std::string(#condition) + "  " + (message))
