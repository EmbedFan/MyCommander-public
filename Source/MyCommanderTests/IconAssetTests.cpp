// IS-0021: the application icon. Two checks that need no screen: the committed multi-size icon file has exactly the
// sizes the build expects (each a PNG of that size), and the MyCommander.exe built next to this test executable
// really carries it as its first icon group. How it *looks* is a manual test (MT entry for IS-0021).
#include "TestFramework.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <vector>

namespace fs = std::filesystem;

namespace {

const std::set<int> kExpectedSizes = {16, 24, 32, 48, 64, 96, 128, 256};

struct IconDirEntry {
    int width = 0;
    int height = 0;
    int bitsPerPixel = 0;
    uint32_t bytes = 0;
    uint32_t offset = 0;
};

std::vector<uint8_t> ReadAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

uint16_t U16(const std::vector<uint8_t>& d, size_t at) { return static_cast<uint16_t>(d[at] | (d[at + 1] << 8)); }
uint32_t U32(const std::vector<uint8_t>& d, size_t at) {
    return static_cast<uint32_t>(d[at]) | (static_cast<uint32_t>(d[at + 1]) << 8) |
           (static_cast<uint32_t>(d[at + 2]) << 16) | (static_cast<uint32_t>(d[at + 3]) << 24);
}
uint32_t BigEndian32(const std::vector<uint8_t>& d, size_t at) {
    return (static_cast<uint32_t>(d[at]) << 24) | (static_cast<uint32_t>(d[at + 1]) << 16) |
           (static_cast<uint32_t>(d[at + 2]) << 8) | static_cast<uint32_t>(d[at + 3]);
}

// This source file lives in <repo>\Source\MyCommanderTests\, so the repository root is three levels up.
fs::path RepoRoot() { return fs::path(__FILE__).parent_path().parent_path().parent_path(); }

fs::path TestExeDirectory() {
    wchar_t buf[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return (len > 0 && len < MAX_PATH) ? fs::path(std::wstring(buf, len)).parent_path() : fs::path();
}

}  // namespace

TEST_CASE(AppIconFile_HasExactlyTheExpectedSizesEachAPngOfThatSize_IS0021) {
    const fs::path file = RepoRoot() / L"Resources" / L"mycommander.ico";
    std::error_code ec;
    if (!fs::exists(file, ec)) {
        std::printf("note: %ls not found; skipping the icon file check\n", file.c_str());
        return;
    }
    const std::vector<uint8_t> data = ReadAll(file);
    CHECK(data.size() > 6 + 16);
    if (data.size() <= 6 + 16) return;
    CHECK(U16(data, 0) == 0);  // reserved
    CHECK(U16(data, 2) == 1);  // 1 = icon (not cursor)
    const uint16_t count = U16(data, 4);
    CHECK(count == kExpectedSizes.size());
    CHECK(data.size() < 300 * 1024);  // about 130 KB expected; a runaway size means the wrong inputs were merged

    std::set<int> seen;
    for (uint16_t i = 0; i < count && 6 + 16 * (i + 1u) <= data.size(); ++i) {
        const size_t at = 6 + 16u * i;
        IconDirEntry e;
        e.width = data[at] == 0 ? 256 : data[at];
        e.height = data[at + 1] == 0 ? 256 : data[at + 1];
        e.bitsPerPixel = U16(data, at + 6);
        e.bytes = U32(data, at + 8);
        e.offset = U32(data, at + 12);

        CHECK(e.width == e.height);
        CHECK(seen.insert(e.width).second);  // each size once
        CHECK(e.bitsPerPixel == 32);
        CHECK(static_cast<size_t>(e.offset) + e.bytes <= data.size());
        if (static_cast<size_t>(e.offset) + 24 > data.size()) continue;
        static const uint8_t kPng[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        CHECK(std::memcmp(data.data() + e.offset, kPng, 8) == 0);
        // The PNG's own IHDR width/height (big-endian, right after the 8-byte signature, the length and "IHDR").
        CHECK(static_cast<int>(BigEndian32(data, e.offset + 16)) == e.width);
        CHECK(static_cast<int>(BigEndian32(data, e.offset + 20)) == e.height);
    }
    CHECK(seen == kExpectedSizes);
}

TEST_CASE(MyCommanderExe_CarriesTheAppIconAsItsFirstIconGroup_IS0021) {
    const fs::path exe = TestExeDirectory() / L"MyCommander.exe";
    std::error_code ec;
    if (!fs::exists(exe, ec)) {
        std::printf("note: %ls not found next to the tests; skipping the executable icon check\n", exe.c_str());
        return;
    }
    HMODULE module = LoadLibraryExW(exe.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    CHECK(module != nullptr);
    if (!module) return;

    // IDI_APPICON is 1 in MyCommander.rc.
    HRSRC group = FindResourceW(module, MAKEINTRESOURCEW(1), reinterpret_cast<LPCWSTR>(static_cast<ULONG_PTR>(14)));  // RT_GROUP_ICON
    CHECK(group != nullptr);
    if (group) {
        HGLOBAL loaded = LoadResource(module, group);
        const uint8_t* bytes = loaded ? static_cast<const uint8_t*>(LockResource(loaded)) : nullptr;
        CHECK(bytes != nullptr);
        if (bytes) {
            // GRPICONDIR: 6 bytes, then 14-byte GRPICONDIRENTRY records (the last field is the RT_ICON id, not an offset).
            const uint16_t count = static_cast<uint16_t>(bytes[4] | (bytes[5] << 8));
            CHECK(count == kExpectedSizes.size());
            std::set<int> sizes;
            for (uint16_t i = 0; i < count; ++i) {
                const uint8_t* entry = bytes + 6 + 14u * i;
                sizes.insert(entry[0] == 0 ? 256 : entry[0]);
                const uint16_t iconId = static_cast<uint16_t>(entry[12] | (entry[13] << 8));
                CHECK(FindResourceW(module, MAKEINTRESOURCEW(iconId), reinterpret_cast<LPCWSTR>(static_cast<ULONG_PTR>(3))) !=
                      nullptr);  // RT_ICON
            }
            CHECK(sizes == kExpectedSizes);
        }
    }
    FreeLibrary(module);
}
