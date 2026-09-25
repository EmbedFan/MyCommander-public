#pragma once

// Test-only fixtures for file-operation tests. Every test works inside a
// fresh, uniquely-named subdirectory of the system temp directory and never
// touches a real user directory (TST-004).

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace test {

class TempDir {
public:
    TempDir() {
        namespace fs = std::filesystem;
        // GetTickCount() has ~15ms resolution, so two TempDirs constructed
        // back-to-back on the same thread (as a test with more than one
        // fixture does) can otherwise collide on the same directory name;
        // the per-process counter guarantees uniqueness regardless of timing.
        static std::atomic<uint32_t> counter{0};
        wchar_t unique[64];
        swprintf_s(unique, L"mc_filops_test_%08x_%04x_%08x", GetTickCount(), GetCurrentThreadId(),
                   counter.fetch_add(1));
        path_ = fs::temp_directory_path() / unique;
        fs::create_directories(path_);
    }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& Path() const { return path_; }

private:
    std::filesystem::path path_;
};

inline void WriteFileContent(const std::filesystem::path& p, const std::string& content) {
    std::ofstream out(p, std::ios::binary);
    out << content;
}

inline std::string ReadFileContent(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Creates a directory junction (reparse point) without requiring admin
// rights or Developer Mode — unlike symbolic links, NTFS junctions never
// need elevation. Returns false if creation failed for any reason, so
// callers can skip a reparse-point test gracefully on unusual setups
// (e.g. a temp volume that doesn't support junctions) rather than reporting
// a false failure.
inline bool CreateJunction(const std::filesystem::path& link, const std::filesystem::path& target) {
    std::wstring cmd = L"cmd /c mklink /J \"" + link.wstring() + L"\" \"" + target.wstring() + L"\" >nul 2>nul";
    int rc = _wsystem(cmd.c_str());
    std::error_code ec;
    return rc == 0 && std::filesystem::exists(link, ec);
}

} // namespace test
