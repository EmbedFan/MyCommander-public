#include "TestFramework.h"

#include "MultiRename.h"

#include <set>
#include <string>
#include <vector>

using mc::ApplyRenameCase;
using mc::ApplySearchReplace;
using mc::BuildRenamePlan;
using mc::ExpandMask;
using mc::MaskContext;
using mc::MultiRenameOptions;
using mc::OrderRenames;
using mc::RenameCase;
using mc::RenamePlan;
using mc::RenameSource;
using mc::RenameStatus;
using mc::RenameStep;
using mc::ValidateNewFileName;

namespace {

MaskContext PhotoContext() {
    MaskContext ctx;
    mc::SplitNameAndExtension(L"Holiday Photo.JPG", ctx.baseName, ctx.extension);
    ctx.parentName = L"Pictures";
    ctx.counter = L"007";
    ctx.lastWrite = {2026, 10, 7, 15, 30, 12};
    return ctx;
}

// The expansion, or "<error>" if the mask doesn't expand (so a failing
// expansion shows up as a mismatch in the caller's CHECK).
std::wstring Expand(const std::wstring& mask) {
    std::wstring out, error;
    return ExpandMask(mask, PhotoContext(), out, error) ? out : L"<error>";
}

std::vector<RenameSource> Sources(const std::vector<std::wstring>& names) {
    std::vector<RenameSource> s;
    for (const auto& n : names) s.push_back({n, {2026, 10, 7, 9, 5, 3}});
    return s;
}

// Replays `steps` on a set of names, the way RenameItems would, failing if
// any step targets a name still held by something else.
bool Replay(std::vector<std::wstring>& names, const std::vector<RenameStep>& steps) {
    for (const auto& step : steps) {
        auto from = std::find(names.begin(), names.end(), step.from);
        if (from == names.end()) return false;
        for (const auto& n : names)
            if (n != step.from && _wcsicmp(n.c_str(), step.to.c_str()) == 0) return false;  // collision
        *from = step.to;
    }
    return true;
}

}  // namespace

// --- Masks -----------------------------------------------------------------------

TEST_CASE(MultiRename_Mask_NameAndExtensionTokens_FOP017) {
    CHECK(Expand(L"[N]") == L"Holiday Photo");
    CHECK(Expand(L"[N2-5]") == L"olid");
    CHECK(Expand(L"[N3]") == L"l");
    CHECK(Expand(L"[N3-]") == L"liday Photo");
    CHECK(Expand(L"[N10-99]") == L"hoto");   // clipped at the end
    CHECK(Expand(L"[N50]") == L"");          // entirely past the end
    CHECK(Expand(L"[E]") == L"JPG");
    CHECK(Expand(L"[E1-2]") == L"JP");
}

TEST_CASE(MultiRename_Mask_CounterDatePathBracketsAndText_FOP017) {
    CHECK(Expand(L"IMG_[C]") == L"IMG_007");
    CHECK(Expand(L"[YMD]") == L"20261007");
    CHECK(Expand(L"[hms]") == L"153012");
    CHECK(Expand(L"[P]-[N]") == L"Pictures-Holiday Photo");
    CHECK(Expand(L"[[x]]") == L"[x]");
    CHECK(Expand(L"plain") == L"plain");
}

TEST_CASE(MultiRename_Mask_Errors_FOP017) {
    std::wstring out, error;
    CHECK(!ExpandMask(L"[X]", PhotoContext(), out, error));
    CHECK(error.find(L"[X]") != std::wstring::npos);
    CHECK(!ExpandMask(L"[N5-2]", PhotoContext(), out, error));  // reversed range
    CHECK(!ExpandMask(L"[N0]", PhotoContext(), out, error));    // positions are 1-based
    CHECK(!ExpandMask(L"ab[N", PhotoContext(), out, error));    // unclosed
    CHECK(error.find(L"position 3") != std::wstring::npos);
}

TEST_CASE(MultiRename_SplitNameAndExtension_FOP017) {
    std::wstring b, e;
    mc::SplitNameAndExtension(L"archive.tar.gz", b, e);
    CHECK(b == L"archive.tar" && e == L"gz");
    mc::SplitNameAndExtension(L"README", b, e);
    CHECK(b == L"README" && e.empty());
    mc::SplitNameAndExtension(L".gitignore", b, e);
    CHECK(b == L".gitignore" && e.empty());
}

TEST_CASE(MultiRename_Counter_StartStepDigitsNeverTruncated_FOP017) {
    MultiRenameOptions o;
    o.counterStart = 9;
    o.counterStep = 1;
    o.counterDigits = 1;
    CHECK(mc::FormatRenameCounter(o, 0) == L"9");
    CHECK(mc::FormatRenameCounter(o, 1) == L"10");  // grows past its digits
    o.counterStart = 1;
    o.counterStep = 5;
    o.counterDigits = 3;
    CHECK(mc::FormatRenameCounter(o, 2) == L"011");
}

// --- Search & replace, case --------------------------------------------------------

TEST_CASE(MultiRename_SearchReplace_CaseInsensitiveAllOccurrences_FOP017) {
    CHECK(ApplySearchReplace(L"IMG_img_Img.jpg", L"img", L"pic") == L"pic_pic_pic.jpg");
    CHECK(ApplySearchReplace(L"draft final draft", L"draft ", L"") == L"final draft");
    CHECK(ApplySearchReplace(L"keep.txt", L"", L"x") == L"keep.txt");
}

TEST_CASE(MultiRename_CaseModes_IncludingUnicode_FOP017) {
    CHECK(ApplyRenameCase(L"Hello World.TXT", RenameCase::Unchanged) == L"Hello World.TXT");
    CHECK(ApplyRenameCase(L"Hello World.TXT", RenameCase::Lower) == L"hello world.txt");
    CHECK(ApplyRenameCase(L"Hello World.txt", RenameCase::Upper) == L"HELLO WORLD.TXT");
    CHECK(ApplyRenameCase(L"hELLO wORLD.TXT", RenameCase::FirstLetter) == L"Hello world.txt");
    CHECK(ApplyRenameCase(L"my_holiday-photo one.JPG", RenameCase::EachWord) == L"My_Holiday-Photo One.Jpg");
    CHECK(ApplyRenameCase(L"\x00E1rv\x00EDzt\x0171r\x0151", RenameCase::Upper) == L"\x00C1RV\x00CDZT\x0170R\x0150");
    CHECK(ApplyRenameCase(L"\x00E1rv\x00EDzt\x0171r\x0151", RenameCase::FirstLetter) == L"\x00C1rv\x00EDzt\x0171r\x0151");
}

// --- Validation ----------------------------------------------------------------------

TEST_CASE(MultiRename_ValidateNewFileName_FOP017) {
    CHECK(ValidateNewFileName(L"ok name.txt").empty());
    CHECK(ValidateNewFileName(L"CONSOLE.txt").empty());
    for (const wchar_t* bad : {L"", L".", L"..", L"CON", L"con.txt", L"COM1.log", L"NUL", L"lpt9", L"name.",
                               L"name ", L"a:b", L"a?b", L"a|b"})
        CHECK(!ValidateNewFileName(bad).empty());
    CHECK(!ValidateNewFileName(std::wstring(256, L'a')).empty());
    CHECK(ValidateNewFileName(std::wstring(255, L'a')).empty());
}

// --- Plan --------------------------------------------------------------------------------

TEST_CASE(MultiRename_Plan_DefaultMasksChangeNothing_FOP017) {
    MultiRenameOptions o;
    auto plan = BuildRenamePlan(o, Sources({L"a.txt", L"b.jpg"}), L"dir", {L"a.txt", L"b.jpg"});
    CHECK(!plan.HasProblems());
    CHECK(plan.RenameCount() == 0);
    CHECK(plan.items[0].status == RenameStatus::Unchanged);
}

TEST_CASE(MultiRename_Plan_CounterRenameAndCaseOnly_FOP017) {
    MultiRenameOptions o;
    o.nameMask = L"[YMD]_[C]";
    o.counterDigits = 3;
    auto plan = BuildRenamePlan(o, Sources({L"x.jpg", L"y.jpg"}), L"dir", {L"x.jpg", L"y.jpg"});
    CHECK(!plan.HasProblems());
    CHECK(plan.items[0].newName == L"20261007_001.jpg");
    CHECK(plan.items[1].newName == L"20261007_002.jpg");
    CHECK(plan.items[0].status == RenameStatus::Ok);

    MultiRenameOptions up;
    up.caseMode = RenameCase::Upper;
    auto casePlan = BuildRenamePlan(up, Sources({L"a.txt"}), L"dir", {L"a.txt"});
    CHECK(casePlan.items[0].status == RenameStatus::CaseOnly);
    CHECK(!casePlan.HasProblems());
}

TEST_CASE(MultiRename_Plan_Problems_FOP017) {
    // Two items to the same name.
    MultiRenameOptions same;
    same.nameMask = L"same";
    auto dup = BuildRenamePlan(same, Sources({L"a.txt", L"b.txt"}), L"dir", {L"a.txt", L"b.txt"});
    CHECK(dup.HasProblems());
    CHECK(dup.items[0].status == RenameStatus::DuplicateTarget && dup.items[1].status == RenameStatus::DuplicateTarget);

    // A clash with a file that isn't part of the batch.
    MultiRenameOptions clash;
    clash.nameMask = L"taken";
    auto c = BuildRenamePlan(clash, Sources({L"a.txt"}), L"dir", {L"a.txt", L"TAKEN.txt"});
    CHECK(c.items[0].status == RenameStatus::ClashesWithExisting);

    // An invalid result, and a mask error.
    MultiRenameOptions invalid;
    invalid.nameMask = L"CON";
    CHECK(BuildRenamePlan(invalid, Sources({L"a.txt"}), L"dir", {L"a.txt"}).items[0].status == RenameStatus::Invalid);
    MultiRenameOptions broken;
    broken.nameMask = L"[Q]";
    auto b = BuildRenamePlan(broken, Sources({L"a.txt"}), L"dir", {L"a.txt"});
    CHECK(!b.maskError.empty() && b.HasProblems());
}

TEST_CASE(MultiRename_Plan_SwapIsNotAClash_FOP017) {
    // A counter running backwards swaps the two names.
    MultiRenameOptions o;
    o.nameMask = L"[C]";
    o.counterStart = 2;
    o.counterStep = -1;  // "1.txt" -> "2", "2.txt" -> "1"
    auto plan = BuildRenamePlan(o, Sources({L"1.txt", L"2.txt"}), L"dir", {L"1.txt", L"2.txt"});
    CHECK(!plan.HasProblems());
    CHECK(plan.items[0].newName == L"2.txt" && plan.items[1].newName == L"1.txt");
}

// --- Ordering -------------------------------------------------------------------------

TEST_CASE(MultiRename_Order_ChainRenamesTheBlockerFirst_FOP017) {
    RenamePlan plan;
    plan.items = {{L"a", L"b", RenameStatus::Ok, L""}, {L"b", L"c", RenameStatus::Ok, L""}};
    auto steps = OrderRenames(plan, {L"a", L"b"});
    CHECK(steps.size() == 2);
    if (steps.size() == 2) CHECK(steps[0].from == L"b" && steps[1].from == L"a");
    std::vector<std::wstring> names{L"a", L"b"};
    CHECK(Replay(names, steps));
    CHECK((std::set<std::wstring>(names.begin(), names.end()) == std::set<std::wstring>{L"b", L"c"}));
}

TEST_CASE(MultiRename_Order_SwapUsesOneTemporaryName_FOP017) {
    RenamePlan plan;
    plan.items = {{L"a", L"b", RenameStatus::Ok, L""}, {L"b", L"a", RenameStatus::Ok, L""}};
    auto steps = OrderRenames(plan, {L"a", L"b", L"~mcren.1.tmp"});
    CHECK(steps.size() == 3);
    if (steps.size() == 3) {
        CHECK(steps[0].toTemp && steps[0].to == L"~mcren.2.tmp");  // ~mcren.1.tmp already exists
        CHECK(steps[2].fromTemp && steps[2].originalName == L"a");
    }
    std::vector<std::wstring> names{L"a", L"b", L"~mcren.1.tmp"};
    CHECK(Replay(names, steps));
    CHECK(names[0] == L"b" && names[1] == L"a");
}

TEST_CASE(MultiRename_Order_ThreeCycleAndIndependentKeepOrder_FOP017) {
    RenamePlan cycle;
    cycle.items = {{L"a", L"b", RenameStatus::Ok, L""}, {L"b", L"c", RenameStatus::Ok, L""},
                   {L"c", L"a", RenameStatus::Ok, L""}};
    std::vector<std::wstring> names{L"a", L"b", L"c"};
    auto steps = OrderRenames(cycle, names);
    CHECK(Replay(names, steps));
    CHECK(names[0] == L"b" && names[1] == L"c" && names[2] == L"a");

    RenamePlan indep;
    indep.items = {{L"x", L"x1", RenameStatus::Ok, L""}, {L"y", L"y1", RenameStatus::Ok, L""},
                   {L"z", L"z", RenameStatus::Unchanged, L""}, {L"w", L"W", RenameStatus::CaseOnly, L""}};
    auto s2 = OrderRenames(indep, {L"x", L"y", L"z", L"w"});
    CHECK(s2.size() == 3);
    if (s2.size() == 3) CHECK(s2[0].from == L"x" && s2[1].from == L"y" && s2[2].to == L"W");
}
