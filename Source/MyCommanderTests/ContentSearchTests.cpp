#include "TestFramework.h"

#include "ContentSearch.h"
#include "TextCase.h"

#include <chrono>
#include <string>

using mc::ContentMatcher;
using mc::ContentQuery;

namespace {
std::optional<ContentMatcher> Make(const std::wstring& text, bool regex = false, bool matchCase = false) {
    std::wstring error;
    return ContentMatcher::Create(ContentQuery{text, regex, matchCase}, error);
}
}  // namespace

// --- Plain text (SRC-004) ---------------------------------------------------------

TEST_CASE(ContentMatcher_Plain_CaseInsensitiveByDefault_SRC004) {
    auto m = Make(L"todo");
    CHECK(m.has_value());
    if (!m) return;
    CHECK(m->Matches(L"  // TODO: fix this"));
    CHECK(m->Matches(L"ToDo"));
    CHECK(!m->Matches(L"to do"));
}

TEST_CASE(ContentMatcher_Plain_UnicodeCaseFolding_SRC004) {
    auto m = Make(L"\x00E1rv\x00EDzt\x0171r\x0151");  // árvíztűrő
    CHECK(m.has_value() && m->Matches(L"\x00C1RV\x00CDZT\x0170R\x0150 t\x00FCk\x00F6rf\x00FAr\x00F3g\x00E9p"));
}

TEST_CASE(ContentMatcher_Plain_MatchCase_SRC004) {
    auto m = Make(L"TODO", false, true);
    CHECK(m.has_value());
    if (!m) return;
    CHECK(m->Matches(L"x TODO y"));
    CHECK(!m->Matches(L"x todo y"));
}

TEST_CASE(ContentMatcher_EmptyTextIsAnError_SRC004) {
    std::wstring error;
    CHECK(!ContentMatcher::Create(ContentQuery{L"", false, false}, error).has_value());
    CHECK(!error.empty());
}

// --- Regular expressions (SRC-005) --------------------------------------------------

TEST_CASE(ContentMatcher_Regex_Basics_SRC005) {
    auto alt = Make(L"TODO|FIXME", true);
    CHECK(alt.has_value() && alt->Matches(L"// fixme later") && !alt->Matches(L"done"));
    auto anchored = Make(L"^\\s*#include", true, true);
    CHECK(anchored.has_value() && anchored->Matches(L"   #include <x>") && !anchored->Matches(L"x #include"));
    auto date = Make(L"\\d{4}-\\d{2}", true);
    CHECK(date.has_value() && date->Matches(L"on 2026-10 it") && !date->Matches(L"on 26-10"));
}

TEST_CASE(ContentMatcher_Regex_CaseInsensitiveWithAccents_SRC005) {
    auto m = Make(L"\x0151r\x00FC", true);  // őrü
    CHECK(m.has_value() && m->Matches(L"Z\x0150R\x00DCLET"));
    auto exact = Make(L"\x0151r\x00FC", true, true);
    CHECK(exact.has_value() && !exact->Matches(L"Z\x0150R\x00DCLET"));
}

TEST_CASE(ContentMatcher_Regex_InvalidPatternReportsError_SRC005) {
    for (const wchar_t* bad : {L"(", L"[a-", L"a{2,1}"}) {
        std::wstring error;
        CHECK(!ContentMatcher::Create(ContentQuery{bad, true, false}, error).has_value());
        CHECK(error.find(L"invalid regular expression") == 0);
    }
}

TEST_CASE(ContentMatcher_LongAndPathologicalLinesDontHang_SRC005) {
    std::wstring huge(100000, L'x');
    huge.replace(50000, 6, L"needle");
    auto plain = Make(L"needle");
    CHECK(plain.has_value() && plain->Matches(huge));  // within the first 64 K
    std::wstring late(100000, L'x');
    late.replace(90000, 6, L"needle");
    CHECK(plain.has_value() && !plain->Matches(late));  // beyond the 64 K limit

    auto evil = Make(L"(a+)+b", true);
    CHECK(evil.has_value());
    auto start = std::chrono::steady_clock::now();
    bool matched = evil && evil->Matches(std::wstring(28, L'a') + L"c");
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    CHECK(!matched);
    wprintf(L"  (pathological regex gave up after %.2f s)\n", seconds);
    CHECK(seconds < 3.0);  // gives up instead of running for ages (Debug build; Release is much faster)
}

// The lowered complexity limit must not reject ordinary patterns, even
// quadratic-ish ones, anywhere inside the part of a line a regex looks at;
// and a regex never looks past it (bounded time per line).
TEST_CASE(ContentMatcher_Regex_LongLines_MatchWithinLimitAndStayBounded_SRC005) {
    std::wstring line(1000, L'x');
    line.replace(980, 8, L"ID-12345");
    for (const wchar_t* pattern : {L"ID-\\d+", L"[A-Z]{2}-[0-9]{5}", L"x+ID", L"(ID|XX)-1", L".*ID"}) {
        auto m = Make(pattern, true);
        CHECK(m.has_value() && m->Matches(line));
    }
    std::wstring beyond(60000, L'x');
    beyond.replace(59000, 8, L"ID-12345");
    auto start = std::chrono::steady_clock::now();
    auto quadratic = Make(L".*ID", true);
    CHECK(quadratic.has_value() && !quadratic->Matches(beyond));  // past the regex line limit
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    wprintf(L"  (quadratic regex on a 60 K line took %.2f s)\n", seconds);
    CHECK(seconds < 3.0);  // Debug build; Release is far faster
}

// --- Binary detection -----------------------------------------------------------------

TEST_CASE(LooksBinary_NulMeansBinaryUnlessUtf16_SRC004) {
    const unsigned char text[] = {'h', 'e', 'l', 'l', 'o', '\n'};
    const unsigned char bin[] = {'M', 'Z', 0x90, 0x00, 0x03};
    CHECK(!mc::LooksBinary(text, sizeof(text), false));
    CHECK(mc::LooksBinary(bin, sizeof(bin), false));
    CHECK(!mc::LooksBinary(bin, sizeof(bin), true));
}

TEST_CASE(TextCase_InvariantMapping_SRC004) {
    CHECK(mc::ToLowerInvariant(L"\x00C1RV\x00CDZ") == L"\x00E1rv\x00EDz");
    CHECK(mc::ToUpperInvariant(L"\x0151\x0171") == L"\x0150\x0170");
}
