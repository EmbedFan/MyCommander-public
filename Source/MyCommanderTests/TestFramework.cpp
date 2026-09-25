#include "TestFramework.h"

#include <cstdio>
#include <cstring>
#include <exception>

namespace test {

std::vector<TestCase>& Registry() {
    static std::vector<TestCase> registry;
    return registry;
}

int RunAll() {
    int totalChecks = 0;
    int failedTests = 0;

    for (const auto& testCase : Registry()) {
        TestContext ctx;
        wprintf(L"[ RUN      ] %ls\n", testCase.name.c_str());
        fflush(stdout);
        try {
            testCase.fn(ctx);
        } catch (const std::exception& e) {
            ctx.Fail(L"uncaught exception: " + std::wstring(e.what(), e.what() + strlen(e.what())), L"", 0);
        } catch (...) {
            ctx.Fail(L"uncaught non-standard exception", L"", 0);
        }
        totalChecks += ctx.ChecksRun();

        if (ctx.Passed()) {
            wprintf(L"[       OK ] %ls (%d checks)\n", testCase.name.c_str(), ctx.ChecksRun());
        } else {
            ++failedTests;
            wprintf(L"[  FAILED  ] %ls\n", testCase.name.c_str());
            for (const auto& failure : ctx.Failures()) {
                if (failure.line > 0) {
                    wprintf(L"    %ls(%d): %ls\n", failure.file.c_str(), failure.line, failure.detail.c_str());
                } else {
                    wprintf(L"    %ls\n", failure.detail.c_str());
                }
            }
        }
    }

    wprintf(L"\n%zu test case(s), %d check(s), %d failed\n", Registry().size(), totalChecks, failedTests);
    return failedTests == 0 ? 0 : 1;
}

} // namespace test
