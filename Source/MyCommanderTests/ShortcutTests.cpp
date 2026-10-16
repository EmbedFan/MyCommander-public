#include "TestFixtures.h"
#include "TestFramework.h"

#include "Shortcut.h"

#include <windows.h>

#include <objbase.h>
#include <shobjidl.h>

#include <set>

namespace fs = std::filesystem;

TEST_CASE(ShortcutNameFor_FreeAndNumbered_FOP026) {
    std::set<std::wstring> taken;
    auto exists = [&](const std::wstring& n) { return taken.count(n) > 0; };
    CHECK(mc::ShortcutNameFor(L"notes.txt", exists) == L"notes.txt - Shortcut.lnk");
    taken.insert(L"notes.txt - Shortcut.lnk");
    CHECK(mc::ShortcutNameFor(L"notes.txt", exists) == L"notes.txt - Shortcut (2).lnk");
    taken.insert(L"notes.txt - Shortcut (2).lnk");
    CHECK(mc::ShortcutNameFor(L"notes.txt", exists) == L"notes.txt - Shortcut (3).lnk");
    CHECK(mc::ShortcutNameFor(L"other", exists) == L"other - Shortcut.lnk");
    CHECK(mc::ShortcutNameFor(L"", exists).empty());
}

TEST_CASE(ShortcutNameFor_TooLongIsRefused_FOP026) {
    auto never = [](const std::wstring&) { return false; };
    CHECK(mc::ShortcutNameFor(std::wstring(250, L'a'), never).empty());
    CHECK(!mc::ShortcutNameFor(std::wstring(200, L'a'), never).empty());
}

TEST_CASE(CreateShortcutFile_RoundTripsTargetAndWorkingDir_FOP026) {
    test::TempDir dir;
    const fs::path folder = dir.Path() / L"project";
    fs::create_directories(folder);
    const fs::path target = folder / L"readme.txt";
    test::WriteFileContent(target, "hello");
    const fs::path link = dir.Path() / L"readme.txt - Shortcut.lnk";

    CHECK(mc::CreateShortcutFile(link, target, folder, L"the readme").empty());
    CHECK(fs::exists(link));

    // Read it back through IShellLinkW.
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IShellLinkW* sl = nullptr;
    bool opened = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                   reinterpret_cast<void**>(&sl)))) {
        IPersistFile* pf = nullptr;
        if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&pf)))) {
            if (SUCCEEDED(pf->Load(link.c_str(), STGM_READ))) {
                opened = true;
                wchar_t path[MAX_PATH] = {};
                CHECK(SUCCEEDED(sl->GetPath(path, MAX_PATH, nullptr, SLGP_RAWPATH)));
                CHECK(_wcsicmp(path, target.c_str()) == 0);
                wchar_t wd[MAX_PATH] = {};
                CHECK(SUCCEEDED(sl->GetWorkingDirectory(wd, MAX_PATH)));
                CHECK(_wcsicmp(wd, folder.c_str()) == 0);
                wchar_t desc[256] = {};
                CHECK(SUCCEEDED(sl->GetDescription(desc, 256)));
                CHECK(std::wstring(desc) == L"the readme");
            }
            pf->Release();
        }
        sl->Release();
    }
    CHECK(opened);
    if (SUCCEEDED(init)) CoUninitialize();
}

TEST_CASE(CreateShortcutFile_FolderTarget_AndBadLocationReportsError_FOP026) {
    test::TempDir dir;
    const fs::path folder = dir.Path() / L"f";
    fs::create_directories(folder);
    CHECK(mc::CreateShortcutFile(dir.Path() / L"f - Shortcut.lnk", folder, folder, L"").empty());
    CHECK(fs::exists(dir.Path() / L"f - Shortcut.lnk"));
    // A link inside a folder that does not exist cannot be saved: a message, no exception.
    CHECK(!mc::CreateShortcutFile(dir.Path() / L"missing" / L"x.lnk", folder, folder, L"").empty());
}
