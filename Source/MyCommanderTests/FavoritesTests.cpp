#include "TestFixtures.h"
#include "TestFramework.h"

#include "Favorites.h"

#include <filesystem>

using mc::DefaultFavoriteName;
using mc::Favorite;
using mc::FindFavoriteByPath;
using mc::FormatFavorites;
using mc::IsValidFavoriteName;
using mc::ParseFavorites;

// --- Parsing -------------------------------------------------------------------

TEST_CASE(Favorites_Parse_TrimsKeepsOrderSkipsComments_NAV007) {
    auto parsed = ParseFavorites(L"# header\n; other comment\n\n  Projects  =  C:\\munka\\2026  \nDownloads = C:\\Users\\x\\Downloads\n");
    CHECK(parsed.warnings.empty());
    CHECK(parsed.favorites.size() == 2);
    if (parsed.favorites.size() == 2) {
        CHECK(parsed.favorites[0].name == L"Projects");
        CHECK(parsed.favorites[0].path == std::filesystem::path(L"C:\\munka\\2026"));
        CHECK(parsed.favorites[1].name == L"Downloads");
    }
}

TEST_CASE(Favorites_Parse_FirstEqualsSplits_PathMayContainEquals_NAV007) {
    auto parsed = ParseFavorites(L"Odd = C:\\a=b\\c\n");
    CHECK(parsed.warnings.empty());
    CHECK(parsed.favorites.size() == 1 && parsed.favorites[0].path.wstring() == L"C:\\a=b\\c");
}

TEST_CASE(Favorites_Parse_UnicodeUncRootAndQuotedPaths_NAV007) {
    auto parsed = ParseFavorites(L"\x00C1rv\x00EDzt\x0171r\x0151 = D:\\Dokumentumok\\\x00C1rv\x00EDzt\x0171r\x0151\r\n"
                                 L"Share = \\\\server\\public\\team\r\n"
                                 L"Root = C:\\\r\n"
                                 L"Quoted = \"C:\\Program Files\"\r\n");
    CHECK(parsed.warnings.empty());
    CHECK(parsed.favorites.size() == 4);
    if (parsed.favorites.size() == 4) {
        CHECK(parsed.favorites[0].name == L"\x00C1rv\x00EDzt\x0171r\x0151");
        CHECK(parsed.favorites[1].path.wstring() == L"\\\\server\\public\\team");
        CHECK(parsed.favorites[2].path.wstring() == L"C:\\");
        CHECK(parsed.favorites[3].path.wstring() == L"C:\\Program Files");  // CRLF trimmed, quotes removed
    }
}

TEST_CASE(Favorites_Parse_BadLinesWarnAndAreSkipped_CFG005) {
    auto parsed = ParseFavorites(L"no equals sign\n = C:\\nameless\nEmpty =   \nGood = C:\\good\n");
    CHECK(parsed.warnings.size() == 3);
    CHECK(parsed.favorites.size() == 1 && parsed.favorites[0].name == L"Good");
    if (parsed.warnings.size() == 3) {
        CHECK(parsed.warnings[0].find(L"line 1") != std::wstring::npos);
        CHECK(parsed.warnings[2].find(L"line 3") != std::wstring::npos);
    }
}

// --- Format / load / save --------------------------------------------------------

TEST_CASE(Favorites_Format_RoundTripsAndAlignsEquals_NAV007) {
    std::vector<Favorite> favs = {{L"A", L"C:\\a"}, {L"Longer name", L"\\\\srv\\share"}, {L"\x00C1", L"D:\\\x00E1"}};
    std::wstring text = FormatFavorites(favs);
    CHECK(text.find(L"# MyCommander directory hotlist") == 0);
    CHECK(text.find(L"A           = C:\\a\n") != std::wstring::npos);
    auto parsed = ParseFavorites(text);
    CHECK(parsed.warnings.empty());
    CHECK(parsed.favorites.size() == 3);
    for (size_t i = 0; i < parsed.favorites.size() && i < favs.size(); ++i) {
        CHECK(parsed.favorites[i].name == favs[i].name);
        CHECK(parsed.favorites[i].path == favs[i].path);
    }
    auto empty = ParseFavorites(FormatFavorites({}));  // header only
    CHECK(empty.favorites.empty() && empty.warnings.empty());
}

TEST_CASE(Favorites_LoadMissingFile_IsEmptyWithoutWarnings_CFG003) {
    test::TempDir dir;
    auto parsed = mc::LoadFavoritesFrom(dir.Path() / L"nope.conf");
    CHECK(parsed.favorites.empty());
    CHECK(parsed.warnings.empty());
}

TEST_CASE(Favorites_SaveAndLoad_RoundTripsUnicodeAsUtf8_CreatesFolder_NAV007) {
    test::TempDir dir;
    auto path = dir.Path() / L"sub" / L"favorites.conf";
    std::vector<Favorite> favs = {{L"Projekt \x00C1rv\x00EDzt\x0171r\x0151", L"D:\\\x6F22\x5B57"}, {L"Share", L"\\\\srv\\x"}};
    CHECK(mc::SaveFavoritesTo(path, favs));
    auto loaded = mc::LoadFavoritesFrom(path);
    CHECK(loaded.warnings.empty());
    CHECK(loaded.favorites.size() == 2);
    if (loaded.favorites.size() == 2) {
        CHECK(loaded.favorites[0].name == favs[0].name);
        CHECK(loaded.favorites[0].path == favs[0].path);
    }
    CHECK(test::ReadFileContent(path).find("\xC3\x81") != std::string::npos);  // UTF-8 bytes on disk
}

// --- Helpers ---------------------------------------------------------------------

TEST_CASE(Favorites_FindByPath_CaseSeparatorAndTrailingSlashInsensitive_NAV007) {
    std::vector<Favorite> favs = {{L"M", L"C:\\Munka"}, {L"S", L"\\\\server\\share\\dir"}, {L"R", L"D:\\"}};
    CHECK(FindFavoriteByPath(favs, L"c:\\munka\\") == 0);
    CHECK(FindFavoriteByPath(favs, L"C:/Munka") == 0);
    CHECK(FindFavoriteByPath(favs, L"C:\\Munka\\sub\\..") == 0);
    CHECK(FindFavoriteByPath(favs, L"\\\\SERVER\\share\\dir\\") == 1);
    CHECK(FindFavoriteByPath(favs, L"d:\\") == 2);
    CHECK(FindFavoriteByPath(favs, L"C:\\Munka2") == -1);
    CHECK(FindFavoriteByPath({}, L"C:\\") == -1);
}

TEST_CASE(Favorites_DefaultName_LeafOrRoot_NAV007) {
    CHECK(DefaultFavoriteName(L"C:\\munka\\2026") == L"2026");
    CHECK(DefaultFavoriteName(L"C:\\munka\\2026\\") == L"2026");
    CHECK(DefaultFavoriteName(L"C:\\") == L"C:\\");
    CHECK(DefaultFavoriteName(L"\\\\server\\share") == L"\\\\server\\share");
    CHECK(DefaultFavoriteName(L"\\\\server\\share\\team") == L"team");
}

TEST_CASE(Favorites_IsValidName_NAV007) {
    CHECK(!IsValidFavoriteName(L""));
    CHECK(!IsValidFavoriteName(L"   "));
    CHECK(!IsValidFavoriteName(L"a=b"));
    CHECK(IsValidFavoriteName(L"Projekt \x00C1rv\x00EDzt\x0171r\x0151"));
}
