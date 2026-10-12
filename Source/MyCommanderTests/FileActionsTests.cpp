#include "TestFixtures.h"
#include "TestFramework.h"

#include "FileActions.h"

#include <filesystem>

using mc::DefaultFileAction;
using mc::ExpandFileActionArguments;
using mc::FileAction;
using mc::FileActionPatternMatches;
using mc::FileActionType;
using mc::FindFileActions;
using mc::ParseFileActions;

namespace {

const std::wstring kSample =
    L"# comment\n"
    L"; another comment\n"
    L"\n"
    L"[.txt .LOG]\n"
    L"view  = Notepad++ | C:\\Tools\\npp.exe | -ro \"%f\"\n"
    L"view* = Built-in  | internal:viewer |\n"
    L"open  = Notepad   | notepad.exe | \"%f\"\n"
    L"\n"
    L"[*.tar.gz readme*]\n"
    L"open* = 7-Zip | \"C:\\Program Files\\7-Zip\\7zFM.exe\" | \"%f\"\n"
    L"\n"
    L"[*]\n"
    L"open = Fallback | system: |\n";

}  // namespace

// --- Parsing -----------------------------------------------------------------

TEST_CASE(FileActions_PluginViewer_ViewOnly_EXT003) {
    auto parsed = ParseFileActions(L"[.bin]\n"
                                   L"view* = Hex | plugin:com.mycommander.exampleplugin |\n"
                                   L"open  = Bad | plugin:com.x |\n"
                                   L"view  = Empty | plugin: |\n"
                                   L"view  = Unnamed | PLUGIN:com.y |\n");
    auto views = FindFileActions(parsed.table, L"a.bin", FileActionType::View);
    CHECK(views.size() == 2);
    CHECK(FindFileActions(parsed.table, L"a.bin", FileActionType::Open).empty());
    CHECK(parsed.warnings.size() == 2);  // plugin: on an open action, and an empty plugin id
    auto def = DefaultFileAction(parsed.table, L"a.bin", FileActionType::View);
    CHECK(def && mc::FileActionPluginId(*def) == std::optional<std::wstring>(L"com.mycommander.exampleplugin"));
    CHECK(views.size() == 2 && mc::FileActionPluginId(views[1]) == std::optional<std::wstring>(L"com.y"));
    FileAction unnamed;
    unnamed.program = L"plugin:com.y";
    CHECK(mc::FileActionLabel(unnamed) == L"Plugin viewer (com.y)");
    FileAction other;
    other.program = L"notepad.exe";
    CHECK(!mc::FileActionPluginId(other).has_value());
}

TEST_CASE(FileActions_PluginViewer_AddLineRoundTrips_EXT003) {
    FileAction action;
    action.type = FileActionType::View;
    action.description = L"Hex dump";
    action.program = L"plugin:com.mycommander.exampleplugin";
    action.isDefault = true;
    std::wstring text = mc::AddFileActionLine(L"", L".bin", action);
    auto parsed = ParseFileActions(text);
    CHECK(parsed.warnings.empty());
    auto def = DefaultFileAction(parsed.table, L"x.bin", FileActionType::View);
    CHECK(def && def->description == L"Hex dump" &&
          mc::FileActionPluginId(*def) == std::optional<std::wstring>(L"com.mycommander.exampleplugin"));
}

// The first-run sample is comments only: it must never compete with a
// user's own blocks (the 1.11.0 sample's active block did).
TEST_CASE(FileActions_DefaultSample_IsCommentsOnly_FEA0009) {
    auto parsed = ParseFileActions(mc::DefaultFileActionsText());
    CHECK(parsed.warnings.empty());
    CHECK(parsed.table.blocks.empty());
    CHECK(!mc::IsUnmodifiedLegacyFileActionsSample(mc::DefaultFileActionsText()));
    CHECK(mc::MigrateLegacyFileActionsText(mc::DefaultFileActionsText()) == mc::DefaultFileActionsText());
}

// --- 1.11.0 sample cleanup -----------------------------------------------------

namespace {
// Verbatim copy of a real file-actions.conf written by 1.11.0's first run.
const std::wstring kRealLegacySample =
    L"# MyCommander file actions (FEA-0009). Save this file and the changes apply\n"
    L"# immediately. File > Assign File to App edits it from inside MyCommander too.\n"
    L"#\n"
    L"#   F3          runs a file's default \"view\" action (no match: built-in viewer)\n"
    L"#   Shift+Enter runs its default \"open\" action (no match: the Windows association)\n"
    L"#   Shift+F3    lists every action for the file, to pick a non-default one\n"
    L"#\n"
    L"# [<patterns>]  one or more extensions (.txt) or wildcard patterns (*.tar.gz,\n"
    L"#               readme*), space-separated; [*] matches every file. The first\n"
    L"#               matching block wins, so put catch-alls last.\n"
    L"# <type>[*] = <description> | <program> | <arguments>\n"
    L"#   type      : view | open\n"
    L"#   *         : marks the default action of that type in this block\n"
    L"#   program   : an executable path or bare name (found via PATH), or:\n"
    L"#                 internal:viewer   the built-in text viewer\n"
    L"#                 system:           the Windows file association\n"
    L"#   arguments : %f full path  %n file name  %b name without extension\n"
    L"#               %e extension (no dot)  %d directory  %% a literal %\n"
    L"#               If none of %f/%n/%b appears, \"%f\" is added automatically.\n"
    L"#               Quote paths yourself where they may contain spaces: \"%f\"\n"
    L"#\n"
    L"# Examples (remove the leading '#' to use them):\n"
    L"#\n"
    L"# [.txt .log .md .ini]\n"
    L"# view  = Notepad++ (read-only) | C:\\Program Files\\Notepad++\\notepad++.exe | -ro \"%f\"\n"
    L"# open* = Notepad               | notepad.exe | \"%f\"\n"
    L"#\n"
    L"# [.pdf .jpg .png]\n"
    L"# open* = Windows default       | system: |\n"
    L"\n"
    L"[.txt .log .md .ini .conf]\n"
    L"view* = Built-in viewer | internal:viewer |\n";

const std::wstring kUserBlocks =
    L"\n[.txt .py .css .js]\n"
    L"view  = Notepad++ | C:\\Program Files\\Notepad++\\notepad++.exe | \"%f\"  \n"
    L"\n[.md .html]\n"
    L"view  = Edge | \"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe\" | \"%f\"\n";
}  // namespace

TEST_CASE(FileActions_Migrate_RealUnmodifiedLegacySample_BecomesCurrentSample_FEA0009) {
    CHECK(mc::IsUnmodifiedLegacyFileActionsSample(kRealLegacySample));
    CHECK(mc::MigrateLegacyFileActionsText(kRealLegacySample) == mc::DefaultFileActionsText());
    std::wstring crlf;
    for (wchar_t c : kRealLegacySample) {
        if (c == L'\n') crlf += L'\r';
        crlf += c;
    }
    CHECK(mc::IsUnmodifiedLegacyFileActionsSample(crlf));  // re-saved by Notepad with CRLF
}

// The user-reported case: own blocks appended below the 1.11.0 block, which
// then -- matching .txt/.md first -- kept them from ever being used.
TEST_CASE(FileActions_Migrate_LegacyBlockWithUserBlocks_RemovesOnlyThatBlock_FEA0009) {
    std::wstring text = kRealLegacySample + kUserBlocks;
    std::wstring migrated = mc::MigrateLegacyFileActionsText(text);
    // Everything up to and including the comments' last line, then the user's blocks unchanged.
    std::wstring expected = kRealLegacySample.substr(0, kRealLegacySample.find(L"\n[.txt .log .md .ini .conf]")) + kUserBlocks;
    CHECK(migrated == expected);
    auto table = ParseFileActions(migrated).table;
    auto txt = DefaultFileAction(table, L"notes.txt", FileActionType::View);
    CHECK(txt.has_value() && txt->description == L"Notepad++");
    auto md = DefaultFileAction(table, L"README.md", FileActionType::View);
    CHECK(md.has_value() && md->description == L"Edge");
    CHECK(mc::MigrateLegacyFileActionsText(migrated) == migrated);  // idempotent
}

TEST_CASE(FileActions_Migrate_LeavesUserEditedBlockAlone_FEA0009) {
    // The user added an action under the old block: it's theirs now.
    std::wstring text = kRealLegacySample + L"open = Notepad | notepad.exe | \"%f\"\n";
    CHECK(mc::MigrateLegacyFileActionsText(text) == text);
    std::wstring unrelated = L"[.txt]\nview = A | a.exe |\n";
    CHECK(mc::MigrateLegacyFileActionsText(unrelated) == unrelated);
}

TEST_CASE(FileActions_Ensure_MigratesLegacyFileOnDisk_FEA0009) {
    test::TempDir dir;
    auto path = dir.Path() / L"file-actions.conf";
    CHECK(mc::SaveFileActionsText(path, kRealLegacySample));
    mc::EnsureFileActionsFileExistsAt(path);
    CHECK(mc::ReadFileActionsText(path) == mc::DefaultFileActionsText());
}

// --- Precedence: the first matching block decides -------------------------------

TEST_CASE(FileActions_Default_EarlierUnmarkedBlockBeatsLaterMarkedBlock_FEA0009) {
    auto table = ParseFileActions(L"[.md]\nview = Edge | msedge.exe |\n\n[.txt .md]\nview* = Other | o.exe |\n").table;
    auto def = DefaultFileAction(table, L"a.md", FileActionType::View);
    CHECK(def.has_value() && def->description == L"Edge");
    // A block with no action of the asked type doesn't decide: the next one does.
    auto table2 = ParseFileActions(L"[.md]\nopen = N | n.exe |\n\n[.md]\nview = V | v.exe |\n").table;
    auto view = DefaultFileAction(table2, L"a.md", FileActionType::View);
    CHECK(view.has_value() && view->description == L"V");
}

TEST_CASE(FileActions_Parse_BlocksFieldsDefaultsAndLineNumbers_FEA0009) {
    auto parsed = ParseFileActions(kSample);
    CHECK(parsed.warnings.empty());
    CHECK(parsed.table.blocks.size() == 3);
    if (parsed.table.blocks.size() != 3) return;
    const auto& txt = parsed.table.blocks[0];
    CHECK(txt.headerLine == 4);
    CHECK(txt.patterns.size() == 2 && txt.patterns[1] == L".LOG");
    CHECK(txt.actions.size() == 3);
    if (txt.actions.size() == 3) {
        CHECK(txt.actions[0].description == L"Notepad++");
        CHECK(txt.actions[0].program == L"C:\\Tools\\npp.exe");
        CHECK(txt.actions[0].arguments == L"-ro \"%f\"");
        CHECK(!txt.actions[0].isDefault);
        CHECK(txt.actions[0].sourceLine == 5);
        CHECK(txt.actions[1].isDefault && txt.actions[1].arguments.empty());
        CHECK(txt.actions[2].type == FileActionType::Open);
    }
    // A quoted program path is unquoted.
    const auto& tar = parsed.table.blocks[1];
    CHECK(!tar.actions.empty() && tar.actions[0].program == L"C:\\Program Files\\7-Zip\\7zFM.exe");
}

TEST_CASE(FileActions_Parse_CrLfLinesAreTrimmed_FEA0009) {
    auto parsed = ParseFileActions(L"[.txt]\r\nopen* = N | notepad.exe | \"%f\"\r\n");
    CHECK(parsed.warnings.empty());
    CHECK(parsed.table.blocks.size() == 1 && parsed.table.blocks[0].actions.size() == 1);
    if (!parsed.table.blocks.empty() && !parsed.table.blocks[0].actions.empty())
        CHECK(parsed.table.blocks[0].actions[0].arguments == L"\"%f\"");
}

TEST_CASE(FileActions_Parse_BadLinesWarnAndAreSkipped_CFG005) {
    std::wstring text =
        L"view = Orphan | x.exe |\n"          // 1: before any block
        L"[.txt]\n"                           // 2
        L"edit = Later | x.exe |\n"           // 3: unknown type
        L"view = Missing fields\n"            // 4: no '|'
        L"open = No program |  |\n"           // 5: empty program
        L"just text\n"                        // 6: no '='
        L"[.md\n"                             // 7: no ']'
        L"[]\n"                               // 8: no patterns
        L"open = Good | good.exe | %q \"%f\"\n";  // 9: valid line under no block (header 8 failed) -> warning
    auto parsed = ParseFileActions(text);
    CHECK(parsed.warnings.size() == 8);
    CHECK(parsed.table.blocks.size() == 1);
    if (!parsed.table.blocks.empty()) CHECK(parsed.table.blocks[0].actions.empty());
    bool namesLine3 = false;
    for (const auto& w : parsed.warnings) namesLine3 |= w.find(L"line 3:") != std::wstring::npos && w.find(L"edit") != std::wstring::npos;
    CHECK(namesLine3);
}

TEST_CASE(FileActions_Parse_UnknownPlaceholderWarnsButKeepsAction_FEA0009) {
    auto parsed = ParseFileActions(L"[.txt]\nopen = A | a.exe | %q \"%f\"\n");
    CHECK(parsed.warnings.size() == 1);
    CHECK(parsed.table.blocks.size() == 1 && parsed.table.blocks[0].actions.size() == 1);
}

TEST_CASE(FileActions_Parse_SecondDefaultInBlockWarns_FirstWins_FEA0009) {
    auto parsed = ParseFileActions(L"[.txt]\nopen* = A | a.exe |\nopen* = B | b.exe |\nview* = V | v.exe |\n");
    CHECK(parsed.warnings.size() == 1);
    auto def = DefaultFileAction(parsed.table, L"x.txt", FileActionType::Open);
    CHECK(def.has_value() && def->description == L"A");
}

TEST_CASE(FileActions_Parse_UnknownTypeIsNotFatal_ForwardCompatible_CFG004) {
    auto parsed = ParseFileActions(L"[.txt]\nprint = P | p.exe |\nopen = A | a.exe |\n");
    CHECK(parsed.warnings.size() == 1);
    CHECK(parsed.table.blocks.size() == 1 && parsed.table.blocks[0].actions.size() == 1);
}

TEST_CASE(FileActions_ArgumentsMayContainPipe_FEA0009) {
    auto parsed = ParseFileActions(L"[.txt]\nopen = A | a.exe | --filter a|b \"%f\"\n");
    CHECK(parsed.warnings.empty());
    if (!parsed.table.blocks.empty() && !parsed.table.blocks[0].actions.empty())
        CHECK(parsed.table.blocks[0].actions[0].arguments == L"--filter a|b \"%f\"");
}

TEST_CASE(FileActions_MissingFile_IsEmptyWithoutWarnings_CFG003) {
    test::TempDir dir;
    auto parsed = mc::LoadFileActionsFrom(dir.Path() / L"nope.conf");
    CHECK(parsed.warnings.empty());
    CHECK(parsed.table.blocks.empty());
}

TEST_CASE(FileActions_EnsureFile_WritesSampleOnceAndNeverOverwrites_CFG001) {
    test::TempDir dir;
    auto path = dir.Path() / L"sub" / L"file-actions.conf";
    mc::EnsureFileActionsFileExistsAt(path);
    CHECK(mc::ReadFileActionsText(path) == mc::DefaultFileActionsText());
    CHECK(mc::SaveFileActionsText(path, L"[.x]\n"));
    mc::EnsureFileActionsFileExistsAt(path);
    CHECK(mc::ReadFileActionsText(path) == L"[.x]\n");
}

TEST_CASE(FileActions_SaveAndRead_RoundTripsUnicodeAsUtf8_FEA0009) {
    test::TempDir dir;
    auto path = dir.Path() / L"file-actions.conf";
    std::wstring text = L"[.txt]\nopen = \x00C1rv\x00EDzt\x0171r\x0151 | C:\\Eszk\x00F6z\\\x6F22.exe | \"%f\"\n";
    CHECK(mc::SaveFileActionsText(path, text));
    CHECK(mc::ReadFileActionsText(path) == text);
    CHECK(test::ReadFileContent(path).find("\xC3\x81") != std::string::npos);  // UTF-8 bytes on disk
}

// --- Matching and defaults -----------------------------------------------------

TEST_CASE(FileActions_PatternMatching_FEA0009) {
    CHECK(FileActionPatternMatches(L".txt", L"Notes.TXT"));
    CHECK(FileActionPatternMatches(L".LOG", L"app.log"));
    CHECK(!FileActionPatternMatches(L".txt", L"notes.txt.bak"));
    CHECK(!FileActionPatternMatches(L".txt", L"txt"));
    CHECK(FileActionPatternMatches(L"*.tar.gz", L"src.TAR.GZ"));
    CHECK(!FileActionPatternMatches(L"*.tar.gz", L"src.gz"));
    CHECK(FileActionPatternMatches(L"readme*", L"README.md"));
    CHECK(FileActionPatternMatches(L"*", L"anything"));
    CHECK(FileActionPatternMatches(L".gitignore", L".gitignore"));  // dotfile: whole-name match
    CHECK(FileActionPatternMatches(L"Makefile", L"makefile"));
    CHECK(!FileActionPatternMatches(L"Makefile", L"Makefile.am"));
}

TEST_CASE(FileActions_Find_CollectsMatchingBlocksInFileOrder_FEA0009) {
    auto table = ParseFileActions(kSample).table;
    auto open = FindFileActions(table, L"notes.txt", FileActionType::Open);
    CHECK(open.size() == 2);  // [.txt] Notepad, then the [*] catch-all
    if (open.size() == 2) {
        CHECK(open[0].description == L"Notepad");
        CHECK(open[1].description == L"Fallback");
    }
    CHECK(FindFileActions(table, L"notes.txt", FileActionType::View).size() == 2);
    CHECK(FindFileActions(table, L"photo.jpg", FileActionType::View).empty());
    CHECK(FindFileActions(table, L"noextension", FileActionType::Open).size() == 1);  // only [*]
}

TEST_CASE(FileActions_Default_MarkedWinsElseFirst_PerType_FEA0009) {
    auto table = ParseFileActions(kSample).table;
    auto view = DefaultFileAction(table, L"a.txt", FileActionType::View);
    CHECK(view.has_value() && view->program == mc::kFileActionProgramInternalViewer);  // the '*' one, not the first
    auto open = DefaultFileAction(table, L"a.txt", FileActionType::Open);
    CHECK(open.has_value() && open->description == L"Notepad");  // none marked: first match
    auto tar = DefaultFileAction(table, L"x.tar.gz", FileActionType::Open);
    CHECK(tar.has_value() && tar->description == L"7-Zip");
    CHECK(!DefaultFileAction(table, L"photo.jpg", FileActionType::View).has_value());  // -> built-in fallback
}

// --- Placeholder expansion -----------------------------------------------------

TEST_CASE(FileActions_Expand_EveryPlaceholder_FEA0009) {
    std::filesystem::path file = L"C:\\My Docs\\report.final.pdf";
    CHECK(ExpandFileActionArguments(L"\"%f\"", file) == L"\"C:\\My Docs\\report.final.pdf\"");
    CHECK(ExpandFileActionArguments(L"%n|%b|%e|%d|100%%", file) ==
          L"report.final.pdf|report.final|pdf|C:\\My Docs|100%");
}

TEST_CASE(FileActions_Expand_AppendsQuotedPathWhenNoFilePlaceholder_FEA0009) {
    std::filesystem::path file = L"C:\\a b\\c.txt";
    CHECK(ExpandFileActionArguments(L"", file) == L"\"C:\\a b\\c.txt\"");
    CHECK(ExpandFileActionArguments(L"-ro", file) == L"-ro \"C:\\a b\\c.txt\"");
    CHECK(ExpandFileActionArguments(L"--ext=%e", file) == L"--ext=txt \"C:\\a b\\c.txt\"");
    CHECK(ExpandFileActionArguments(L"%n", file) == L"c.txt");  // %n counts as a file placeholder
}

TEST_CASE(FileActions_Expand_UnknownPlaceholderKeptAndUnicodePath_FEA0009) {
    std::filesystem::path file = L"D:\\\x6F22\x5B57\\\x00E1rv\x00ED.txt";
    CHECK(ExpandFileActionArguments(L"%q %f", file) == L"%q D:\\\x6F22\x5B57\\\x00E1rv\x00ED.txt");
    CHECK(ExpandFileActionArguments(L"trailing %", file) == L"trailing % \"D:\\\x6F22\x5B57\\\x00E1rv\x00ED.txt\"");
}

TEST_CASE(FileActions_Label_FallsBackToProgramName_FEA0009) {
    FileAction a;
    a.program = L"C:\\Tools\\npp.exe";
    CHECK(mc::FileActionLabel(a) == L"npp.exe");
    a.description = L"Notepad++";
    CHECK(mc::FileActionLabel(a) == L"Notepad++");
    FileAction b;
    b.program = mc::kFileActionProgramSystem;
    CHECK(mc::FileActionLabel(b) == L"Windows default");
}

// --- Phase 2: line-level edits -------------------------------------------------

TEST_CASE(FileActions_AddToDedicatedBlock_KeepsEverythingElse_VEE013) {
    const std::wstring text = L"# c\n[.txt]\nview* = A | a.exe |\n\n[.md]\nopen = M | m.exe |\n";
    FileAction a;
    a.type = FileActionType::Open;
    a.description = L"Word";
    a.program = L"winword.exe";
    a.arguments = L"\"%f\"";
    std::wstring edited = mc::AddFileActionLine(text, L".TXT", a);  // pattern match is case-insensitive
    CHECK(edited == L"# c\n[.txt]\nview* = A | a.exe |\nopen = Word | winword.exe | \"%f\"\n\n[.md]\nopen = M | m.exe |\n");
}

TEST_CASE(FileActions_AddAsDefault_MovesTheStarWithinTheDedicatedBlock_VEE013) {
    const std::wstring text = L"[.txt]\nview  = A | a.exe |\nview* = B | b.exe |\nopen* = O | o.exe |\n";
    FileAction a;
    a.type = FileActionType::View;
    a.description = L"Less";
    a.program = L"less.exe";
    a.isDefault = true;
    std::wstring edited = mc::AddFileActionLine(text, L".txt", a);
    auto def = DefaultFileAction(ParseFileActions(edited).table, L"a.txt", FileActionType::View);
    CHECK(def.has_value() && def->description == L"Less");
    CHECK(edited.find(L"view  = B") != std::wstring::npos);  // star removed, '=' column kept
    CHECK(edited.find(L"open* = O") != std::wstring::npos);  // other type untouched
}

// The first-run sample shares one block across .txt/.log/.md/...: adding a
// .md action must not change what .txt or .log do.
TEST_CASE(FileActions_AddForSharedPattern_InsertsDedicatedBlockBefore_SharedUntouched_VEE013) {
    FileAction a;
    a.type = FileActionType::View;
    a.description = L"Less";
    a.program = L"less.exe";
    a.isDefault = true;
    std::wstring edited = mc::AddFileActionLine(kSample, L".txt", a);
    std::wstring expected = kSample;
    expected.insert(expected.find(L"[.txt .LOG]"), L"[.txt]\nview* = Less | less.exe |\n\n");
    CHECK(edited == expected);  // the shared block itself is byte-for-byte unchanged
    auto table = ParseFileActions(edited).table;
    auto txt = DefaultFileAction(table, L"a.txt", FileActionType::View);
    CHECK(txt.has_value() && txt->description == L"Less");
    auto log = DefaultFileAction(table, L"a.log", FileActionType::View);
    CHECK(log.has_value() && log->program == mc::kFileActionProgramInternalViewer);  // .log unaffected

    // A second add for the same pattern goes into the new dedicated block.
    FileAction b;
    b.type = FileActionType::Open;
    b.description = L"Word";
    b.program = L"winword.exe";
    std::wstring twice = mc::AddFileActionLine(edited, L".txt", b);
    expected.insert(expected.find(L"\n\n[.txt .LOG]") + 1, L"open = Word | winword.exe |\n");
    CHECK(twice == expected);
}

TEST_CASE(FileActions_SetDefaultForPattern_OnSharedAction_CopiesIntoOwnBlock_SharedUntouched_VEE013) {
    FileAction a;
    a.type = FileActionType::View;
    a.description = L"Less";
    a.program = L"less.exe";
    a.isDefault = true;
    std::wstring edited = mc::AddFileActionLine(kSample, L".txt", a);  // [.txt] block, before the shared one
    int nppLine = 0;
    for (const auto& block : ParseFileActions(edited).table.blocks)
        for (const auto& act : block.actions)
            if (act.description == L"Notepad++") nppLine = act.sourceLine;
    CHECK(nppLine > 0);
    std::wstring result = mc::SetDefaultFileActionForPattern(edited, L".txt", nppLine);
    auto table = ParseFileActions(result).table;
    auto txt = DefaultFileAction(table, L"a.txt", FileActionType::View);
    CHECK(txt.has_value() && txt->description == L"Notepad++");
    CHECK(result.find(L"[.txt]\nview  = Less | less.exe |\nview* = Notepad++ |") != std::wstring::npos);
    // The shared [.txt .LOG] block is byte-for-byte as before, so .log is unaffected.
    std::wstring sharedBefore = edited.substr(edited.find(L"[.txt .LOG]"));
    CHECK(result.substr(result.find(L"[.txt .LOG]")) == sharedBefore);
    auto log = DefaultFileAction(table, L"a.log", FileActionType::View);
    CHECK(log.has_value() && log->program == mc::kFileActionProgramInternalViewer);
    // Again: the existing copy is re-marked, not duplicated.
    CHECK(mc::SetDefaultFileActionForPattern(result, L".txt", nppLine + 1) == result);
}

TEST_CASE(FileActions_SetDefaultForPattern_OnDedicatedAction_IsPlainSetDefault_VEE013) {
    std::wstring text = L"[.txt]\nview* = A | a.exe |\nview = B | b.exe |\n";
    CHECK(mc::SetDefaultFileActionForPattern(text, L".txt", 3) == mc::SetDefaultFileActionLine(text, 3));
    CHECK(mc::SetDefaultFileActionForPattern(text, L".txt", 3) == L"[.txt]\nview  = A | a.exe |\nview* = B | b.exe |\n");
}

TEST_CASE(FileActions_AddCreatesNewBlockAtEnd_VEE013) {
    FileAction a;
    a.type = FileActionType::Open;
    a.description = L"Viewer";
    a.program = L"img.exe";
    std::wstring edited = mc::AddFileActionLine(kSample, L".jpg", a);
    CHECK(edited.substr(0, kSample.size()) == kSample);
    CHECK(edited.substr(kSample.size()) == L"\n[.jpg]\nopen = Viewer | img.exe |\n");
    CHECK(mc::AddFileActionLine(L"", L".jpg", a) == L"[.jpg]\nopen = Viewer | img.exe |\n");
}

TEST_CASE(FileActions_AddKeepsCrLfLineEndings_VEE013) {
    std::wstring crlf = L"# c\r\n[.txt]\r\nopen = A | a.exe |\r\n";
    FileAction a;
    a.type = FileActionType::Open;
    a.description = L"B";
    a.program = L"b.exe";
    CHECK(mc::AddFileActionLine(crlf, L".txt", a) == L"# c\r\n[.txt]\r\nopen = A | a.exe |\r\nopen = B | b.exe |\r\n");
}

TEST_CASE(FileActions_RemoveLine_VEE013) {
    std::wstring edited = mc::RemoveFileActionLine(kSample, 5);
    CHECK(edited.find(L"Notepad++") == std::wstring::npos);
    CHECK(ParseFileActions(edited).table.blocks[0].actions.size() == 2);
    CHECK(mc::RemoveFileActionLine(kSample, 0) == kSample);
    CHECK(mc::RemoveFileActionLine(kSample, 999) == kSample);
}

TEST_CASE(FileActions_SetDefault_MovesStarSameTypeSameBlock_VEE013) {
    std::wstring edited = mc::SetDefaultFileActionLine(kSample, 5);  // Notepad++ view
    auto table = ParseFileActions(edited).table;
    auto def = DefaultFileAction(table, L"a.txt", FileActionType::View);
    CHECK(def.has_value() && def->description == L"Notepad++");
    CHECK(edited.find(L"view* = Notepad++") != std::wstring::npos);
    CHECK(edited.find(L"view  = Built-in") != std::wstring::npos);
    CHECK(edited.find(L"open* = 7-Zip") != std::wstring::npos);  // other block untouched
    CHECK(mc::SetDefaultFileActionLine(kSample, 1) == kSample);   // not an action line: no-op
}
