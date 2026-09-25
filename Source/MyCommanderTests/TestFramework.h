#pragma once

// Minimal self-contained test framework: no third-party dependency, matches
// the project's "no third-party libraries" stance (DEC-002's spirit extended
// to tooling). A TEST_CASE registers itself at static-init time; RunAll()
// executes every registered case and returns a process exit code.

#include <functional>
#include <string>
#include <vector>

namespace test {

struct Failure {
    std::wstring detail;
    std::wstring file;
    int line = 0;
};

class TestContext {
public:
    void Check(bool condition, const wchar_t* expr, const wchar_t* file, int line) {
        ++checksRun_;
        if (!condition) {
            failures_.push_back({expr, file, line});
        }
    }

    void Fail(const std::wstring& detail, const wchar_t* file, int line) {
        ++checksRun_;
        failures_.push_back({detail, file, line});
    }

    bool Passed() const { return failures_.empty(); }
    int ChecksRun() const { return checksRun_; }
    const std::vector<Failure>& Failures() const { return failures_; }

private:
    int checksRun_ = 0;
    std::vector<Failure> failures_;
};

using TestFn = std::function<void(TestContext&)>;

struct TestCase {
    std::wstring name;
    TestFn fn;
};

std::vector<TestCase>& Registry();

struct Registrar {
    Registrar(const wchar_t* name, TestFn fn) { Registry().push_back({name, std::move(fn)}); }
};

// Runs every registered test case, prints a report, and returns a process
// exit code (0 if every test passed).
int RunAll();

} // namespace test

#define TEST_WIDEN2(x) L##x
#define TEST_WIDEN(x) TEST_WIDEN2(x)

#define TEST_CASE(name)                                                        \
    static void name(test::TestContext& tc);                                   \
    static ::test::Registrar registrar_##name(L#name, name);                   \
    static void name(test::TestContext& tc)

#define CHECK(cond) tc.Check(!!(cond), L#cond, TEST_WIDEN(__FILE__), __LINE__)
#define FAIL(msg) tc.Fail((msg), TEST_WIDEN(__FILE__), __LINE__)
