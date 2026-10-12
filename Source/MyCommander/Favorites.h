#pragma once

// NAV-007: the directory hotlist -- named favorite folders, opened with
// Ctrl+D (or Commands > Directory Hotlist) and jumped to with Enter. Pure
// logic (no Console/Dialog dependency, CLAUDE.md's pure-module precedent):
// parsing/formatting the per-user favorites.conf, and the small list
// operations the hotlist dialog in main.cpp needs.
//
// Stored in its own %APPDATA%\MyCommander\favorites.conf, not in
// mycommander.ini -- an ordered list of user records doesn't fit that file's
// one-line-per-key machinery (the same reasoning as DEC-011). Format,
// DEC-007-style, one favorite per line in list order:
//
//   # comment
//   Projects  = C:\munka\2026
//   Share     = \\server\public\team
//
// The first '=' splits a line, so a name can't contain '=' but a path can.
// Unlike session.ini, it's written on every change (a favorite only ever
// navigates when the user picks it, so there's no ERR-006 replay concern).

#include <filesystem>
#include <string>
#include <vector>

namespace mc {

struct Favorite {
    std::wstring name;
    std::filesystem::path path;
};

struct ParseFavoritesResult {
    std::vector<Favorite> favorites;
    std::vector<std::wstring> warnings;  // CFG-005 style: line number + content
};

ParseFavoritesResult ParseFavorites(const std::wstring& text);

// A header comment plus one "name = path" line per favorite, the names
// padded so the '=' signs line up (display-width-aware, UI-011). Saving
// rewrites the whole file: comments typed between entries by hand are not
// kept (the header is).
std::wstring FormatFavorites(const std::vector<Favorite>& favorites);

// %APPDATA%\MyCommander\favorites.conf, or empty if %APPDATA% can't be resolved.
std::filesystem::path FavoritesFilePath();

// A missing or empty file is an empty list with no warnings (CFG-003).
ParseFavoritesResult LoadFavoritesFrom(const std::filesystem::path& path);

// UTF-8, atomic (Utf8TextFile.h). False on any failure.
bool SaveFavoritesTo(const std::filesystem::path& path, const std::vector<Favorite>& favorites);

// Index of the favorite naming the same folder as `folder` -- compared
// lexically (no filesystem access, so an offline share can't block it),
// case-insensitively, '/' and '\' alike, a trailing separator ignored -- or
// -1 if there is none.
int FindFavoriteByPath(const std::vector<Favorite>& favorites, const std::filesystem::path& folder);

// The name the "add" prompt starts with: the folder's own name, or the
// whole path for a drive root ("C:\") or UNC share root ("\\server\share").
std::wstring DefaultFavoriteName(const std::filesystem::path& folder);

// Non-empty after trimming, and no '=' (which would split the line wrongly).
bool IsValidFavoriteName(const std::wstring& name);

}  // namespace mc
