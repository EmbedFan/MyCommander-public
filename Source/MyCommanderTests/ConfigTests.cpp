#include "TestFixtures.h"
#include "TestFramework.h"

#include "Config.h"

#include <algorithm>
#include <filesystem>

using mc::AppendMissingConfigKeys;
using mc::ColorTheme;
using mc::ColumnId;
using mc::Config;
using mc::EnsureConfigFileExistsAt;
using mc::EnterFileAction;
using mc::LoadConfigFrom;
using mc::LogLevel;

TEST_CASE(LoadConfigFrom_NonexistentFile_ReturnsDefaultsWithNoWarnings) {
    test::TempDir dir;
    auto result = LoadConfigFrom(dir.Path() / L"does-not-exist.ini");

    CHECK(result.config.confirmRecycleBinDelete);
    CHECK(result.config.confirmPermanentDelete);
    CHECK(result.config.enterFileAction == EnterFileAction::View);
    CHECK(result.config.groupDirectoriesFirst);
    CHECK(!result.config.useBasicSymbols);
    CHECK(result.config.persistCommandHistory);
    CHECK(result.config.logLevel == LogLevel::Off);
    CHECK(result.warnings.empty());
}

TEST_CASE(LoadConfigFrom_EmptyFile_ReturnsDefaultsWithNoWarnings) {
    test::TempDir dir;
    auto path = dir.Path() / L"mycommander.ini";
    test::WriteFileContent(path, "");

    auto result = LoadConfigFrom(path);

    CHECK(result.config.confirmRecycleBinDelete);
    CHECK(result.config.confirmPermanentDelete);
    CHECK(result.config.enterFileAction == EnterFileAction::View);
    CHECK(result.warnings.empty());
}

TEST_CASE(LoadConfigFrom_ParsesEnterFileActionValues) {
    test::TempDir dir;
    auto path = dir.Path() / L"mycommander.ini";
    test::WriteFileContent(path, "enterFileAction = edit\n");

    auto result = LoadConfigFrom(path);

    CHECK(result.config.enterFileAction == EnterFileAction::Edit);
    CHECK(result.warnings.empty());
}

TEST_CASE(LoadConfigFrom_ParsesExecuteEnterFileAction_IsCaseInsensitive) {
    test::TempDir dir;
    auto path = dir.Path() / L"mycommander.ini";
    test::WriteFileContent(path, "ENTERFILEACTION = EXECUTE\n");

    auto result = LoadConfigFrom(path);

    CHECK(result.config.enterFileAction == EnterFileAction::Execute);
    CHECK(result.warnings.empty());
}

TEST_CASE(LoadConfigFrom_InvalidEnterFileActionValue_IsReportedAndKeepsDefault_CFG005) {
    test::TempDir dir;
    auto path = dir.Path() / L"mycommander.ini";
    test::WriteFileContent(path, "enterFileAction = launch\n");

    auto result = LoadConfigFrom(path);

    CHECK(result.config.enterFileAction == EnterFileAction::View);  // kept the safe default
    CHECK(result.warnings.size() == 1);
    if (!result.warnings.empty()) {
        CHECK(result.warnings[0].find(L"enterfileaction") != std::wstring::npos);
        CHECK(result.warnings[0].find(L"launch") != std::wstring::npos);
    }
}

TEST_CASE(LoadConfigFrom_ParsesFalseValues) {
    test::TempDir dir;
    auto path = dir.Path() / L"mycommander.ini";
    test::WriteFileContent(path, "confirmRecycleBinDelete = false\nconfirmPermanentDelete = false\n");

    auto result = LoadConfigFrom(path);

    CHECK(!result.config.confirmRecycleBinDelete);
    CHECK(!result.config.confirmPermanentDelete);
    CHECK(result.warnings.empty());
}

TEST_CASE(LoadConfigFrom_ParsesUseBasicSymbolsForAsciiFrameFallback_UI014_ACC006) {
    test::TempDir dir;
    auto path = dir.Path() / L"mycommander.ini";
    test::WriteFileContent(path, "USEBASICSYMBOLS = yes\n");

    auto result = LoadConfigFrom(path);

    CHECK(result.config.useBasicSymbols);
    CHECK(result.warnings.empty());
}

TEST_CASE(LoadConfigFrom_ParsesPersistCommandHistoryDisabled_CLI004) {
    test::TempDir dir;
    auto path = dir.Path() / L"mycommander.ini";
    test::WriteFileContent(path, "persistCommandHistory = false\n");

    auto result = LoadConfigFrom(path);

    CHECK(!result.config.persistCommandHistory);
    CHECK(result.warnings.empty());
}

TEST_CASE(LoadConfigFrom_InvalidPersistCommandHistory_IsReportedAndKeepsDefault_CLI004) {
    test::TempDir dir;
    auto path = dir.Path() / L"mycommander.ini";
    test::WriteFileContent(path, "persistCommandHistory = sometimes\n");

    auto result = LoadConfigFrom(path);

    CHECK(result.config.persistCommandHistory);  // kept the default
    CHECK(result.warnings.size() == 1);
    if (!result.warnings.empty()) {
        CHECK(result.warnings[0].find(L"persistcommandhistory") != std::wstring::npos);
    }
}

TEST_CASE(LoadConfigFrom_ParsesLogLevel_ERR004) {
    test::TempDir dir;
    auto path = dir.Path() / L"mycommander.ini";
    test::WriteFileContent(path, "logLevel = DEBUG\n");

    auto result = LoadConfigFrom(path);

    CHECK(result.config.logLevel == LogLevel::Debug);
    CHECK(result.warnings.empty());
}

TEST_CASE(LoadConfigFrom_InvalidLogLevel_IsReportedAndKeepsOffDefault_ERR004) {
    test::TempDir dir;
    auto path = dir.Path() / L"mycommander.ini";
    test::WriteFileContent(path, "logLevel = verbose\n");

    auto result = LoadConfigFrom(path);

    CHECK(result.config.logLevel == LogLevel::Off);  // kept the default
    CHECK(result.warnings.size() == 1);
    if (!result.warnings.empty()) {
        CHECK(result.warnings[0].find(L"loglevel") != std::wstring::npos);
        CHECK(result.warnings[0].find(L"verbose") != std::wstring::npos);
    }
}

// --- UI-005: configurable visible columns and widths -----------------------

TEST_CASE(LoadConfigFrom_NonexistentFile_DefaultsToAllColumnsInOriginalOrder_UI005) {
    test::TempDir dir;
    auto result = LoadConfigFrom(dir.Path() / L"does-not-exist.ini");
    std::vector<ColumnId> expected = {ColumnId::Type, ColumnId::Attributes, ColumnId::ModifiedTime, ColumnId::Size};
    CHECK(result.config.visibleColumns == expected);
    CHECK(result.config.typeColumnWidth == 6);
    CHECK(result.config.mtimeColumnWidth == 16);
    CHECK(result.config.sizeColumnWidth == 10);
}

TEST_CASE(LoadConfigFrom_ParsesVisibleColumnsSubsetAndOrder_UI005) {
    test::TempDir dir;
    auto path = dir.Path() / L"mycommander.ini";
    test::WriteFileContent(path, "visibleColumns = size,mtime\n");

    auto result = LoadConfigFrom(path);

    std::vector<ColumnId> expected = {ColumnId::Size, ColumnId::ModifiedTime};
    CHECK(result.config.visibleColumns == expected);
    CHECK(result.warnings.empty());
}

TEST_CASE(LoadConfigFrom_EmptyVisibleColumns_MeansNameOnly_UI005) {
    test::TempDir dir;
    auto path = dir.Path() / L"mycommander.ini";
    test::WriteFileContent(path, "visibleColumns = \n");

    auto result = LoadConfigFrom(path);

    CHECK(result.config.visibleColumns.empty());
    CHECK(result.warnings.empty());
}

TEST_CASE(LoadConfigFrom_VisibleColumns_IsCaseInsensitiveAndTrimsWhitespace_UI005) {
    test::TempDir dir;
    auto path = dir.Path() / L"mycommander.ini";
    test::WriteFileContent(path, "visibleColumns = TYPE, Attr , sIzE\n");

    auto result = LoadConfigFrom(path);

    std::vector<ColumnId> expected = {ColumnId::Type, ColumnId::Attributes, ColumnId::Size};
    CHECK(result.config.visibleColumns == expected);
    CHECK(result.warnings.empty());
}

TEST_CASE(LoadConfigFrom_VisibleColumns_CollapsesDuplicatesToFirstOccurrence_UI005) {
    test::TempDir dir;
    auto path = dir.Path() / L"mycommander.ini";
    test::WriteFileContent(path, "visibleColumns = size,type,size\n");

    auto result = LoadConfigFrom(path);

    std::vector<ColumnId> expected = {ColumnId::Size, ColumnId::Type};
    CHECK(result.config.visibleColumns == expected);
}

TEST_CASE(LoadConfigFrom_InvalidVisibleColumnsToken_IsReportedAndKeepsDefault_UI005) {
    test::TempDir dir;
    auto path = dir.Path() / L"mycommander.ini";
    test::WriteFileContent(path, "visibleColumns = size,bogus\n");

    auto result = LoadConfigFrom(path);

    std::vector<ColumnId> expected = {ColumnId::Type, ColumnId::Attributes, ColumnId::ModifiedTime, ColumnId::Size};
    CHECK(result.config.visibleColumns == expected);  // kept the default
    CHECK(result.warnings.size() == 1);
    if (!result.warnings.empty()) {
        CHECK(result.warnings[0].find(L"visiblecolumns") != std::wstring::npos);
        CHECK(result.warnings[0].find(L"bogus") != std::wstring::npos);
    }
}

TEST_CASE(LoadConfigFrom_ParsesColumnWidths_UI005) {
    test::TempDir dir;
    auto path = dir.Path() / L"mycommander.ini";
    test::WriteFileContent(path, "typeColumnWidth = 8\nmtimeColumnWidth = 19\nsizeColumnWidth = 14\n");

    auto result = LoadConfigFrom(path);

    CHECK(result.config.typeColumnWidth == 8);
    CHECK(result.config.mtimeColumnWidth == 19);
    CHECK(result.config.sizeColumnWidth == 14);
    CHECK(result.warnings.empty());
}

TEST_CASE(LoadConfigFrom_ColumnWidthBelowMinimum_IsRejectedAndKeepsDefault_UI005) {
    test::TempDir dir;
    auto path = dir.Path() / L"mycommander.ini";
    // mtime's real minimum is 16 (the fixed "YYYY-MM-DD HH:MM" format) --
    // a smaller configured value would truncate a real timestamp.
    test::WriteFileContent(path, "mtimeColumnWidth = 10\n");

    auto result = LoadConfigFrom(path);

    CHECK(result.config.mtimeColumnWidth == 16);  // kept the default
    CHECK(result.warnings.size() == 1);
    if (!result.warnings.empty()) {
        CHECK(result.warnings[0].find(L"mtimecolumnwidth") != std::wstring::npos);
    }
}

TEST_CASE(LoadConfigFrom_NonNumericColumnWidth_IsRejectedAndKeepsDefault_UI005) {
    test::TempDir dir;
    auto path = dir.Path() / L"mycommander.ini";
    test::WriteFileContent(path, "sizeColumnWidth = wide\n");

    auto result = LoadConfigFrom(path);

    CHECK(result.config.sizeColumnWidth == 10);  // kept the default
    CHECK(result.warnings.size() == 1);
}

// --- UI-010: configurable color theme ---------------------------------------

TEST_CASE(LoadConfigFrom_DefaultsToDefaultTheme_UI010) {
    test::TempDir dir;
    auto result = LoadConfigFrom(dir.Path() / L"does-not-exist.ini");
    CHECK(result.config.colorTheme == ColorTheme::Default);
}

TEST_CASE(LoadConfigFrom_ParsesHighContrastTheme_UI010) {
    test::TempDir dir;
    auto path = dir.Path() / L"mycommander.ini";
    test::WriteFileContent(path, "colorTheme = highcontrast\n");

    auto result = LoadConfigFrom(path);

    CHECK(result.config.colorTheme == ColorTheme::HighContrast);
    CHECK(result.warnings.empty());
}

TEST_CASE(LoadConfigFrom_ColorTheme_IsCaseInsensitive_UI010) {
    test::TempDir dir;
    auto path = dir.Path() / L"mycommander.ini";
    test::WriteFileContent(path, "colorTheme = HighContrast\n");

    auto result = LoadConfigFrom(path);

    CHECK(result.config.colorTheme == ColorTheme::HighContrast);
}

TEST_CASE(LoadConfigFrom_ExplicitDefaultTheme_UI010) {
    test::TempDir dir;
    auto path = dir.Path() / L"mycommander.ini";
    test::WriteFileContent(path, "colorTheme = default\n");

    auto result = LoadConfigFrom(path);

    CHECK(result.config.colorTheme == ColorTheme::Default);
    CHECK(result.warnings.empty());
}

TEST_CASE(LoadConfigFrom_InvalidColorTheme_IsReportedAndKeepsDefault_UI010) {
    test::TempDir dir;
    auto path = dir.Path() / L"mycommander.ini";
    test::WriteFileContent(path, "colorTheme = rainbow\n");

    auto result = LoadConfigFrom(path);

    CHECK(result.config.colorTheme == ColorTheme::Default);  // kept the default
    CHECK(result.warnings.size() == 1);
    if (!result.warnings.empty()) {
        CHECK(result.warnings[0].find(L"colortheme") != std::wstring::npos);
        CHECK(result.warnings[0].find(L"rainbow") != std::wstring::npos);
    }
}

// --- IS-0002: whether F4 waits for the external editor to close -----------

TEST_CASE(LoadConfigFrom_DefaultsToNotWaitingForEditor_IS0002) {
    test::TempDir dir;
    auto result = LoadConfigFrom(dir.Path() / L"does-not-exist.ini");
    CHECK(!result.config.waitForEditorToClose);
}

TEST_CASE(LoadConfigFrom_ParsesWaitForEditorToCloseTrue_IS0002) {
    test::TempDir dir;
    auto path = dir.Path() / L"mycommander.ini";
    test::WriteFileContent(path, "waitForEditorToClose = true\n");

    auto result = LoadConfigFrom(path);

    CHECK(result.config.waitForEditorToClose);
    CHECK(result.warnings.empty());
}

TEST_CASE(LoadConfigFrom_InvalidWaitForEditorToClose_IsReportedAndKeepsDefault_IS0002) {
    test::TempDir dir;
    auto path = dir.Path() / L"mycommander.ini";
    test::WriteFileContent(path, "waitForEditorToClose = maybe\n");

    auto result = LoadConfigFrom(path);

    CHECK(!result.config.waitForEditorToClose);  // kept the default
    CHECK(result.warnings.size() == 1);
}

TEST_CASE(LoadConfigFrom_ParsesDirectoryGrouping_LIST004) {
    test::TempDir dir;
    auto path = dir.Path() / L"mycommander.ini";
    test::WriteFileContent(path, "groupDirectoriesFirst = false\n");

    auto result = LoadConfigFrom(path);

    CHECK(!result.config.groupDirectoriesFirst);
    CHECK(result.warnings.empty());
}

TEST_CASE(LoadConfigFrom_InvalidUseBasicSymbols_IsReportedAndKeepsBoxDrawingDefault_UI014) {
    test::TempDir dir;
    auto path = dir.Path() / L"mycommander.ini";
    test::WriteFileContent(path, "useBasicSymbols = unavailable\n");

    auto result = LoadConfigFrom(path);

    CHECK(!result.config.useBasicSymbols);
    CHECK(result.warnings.size() == 1);
    if (!result.warnings.empty()) {
        CHECK(result.warnings[0].find(L"usebasicsymbols") != std::wstring::npos);
    }
}

TEST_CASE(LoadConfigFrom_IsCaseInsensitiveForKeysAndValues) {
    test::TempDir dir;
    auto path = dir.Path() / L"mycommander.ini";
    test::WriteFileContent(path, "CONFIRMRECYCLEBINDELETE = FALSE\nConfirmPermanentDelete = No\n");

    auto result = LoadConfigFrom(path);

    CHECK(!result.config.confirmRecycleBinDelete);
    CHECK(!result.config.confirmPermanentDelete);
    CHECK(result.warnings.empty());
}

TEST_CASE(LoadConfigFrom_IgnoresCommentsAndBlankLines) {
    test::TempDir dir;
    auto path = dir.Path() / L"mycommander.ini";
    test::WriteFileContent(path,
                           "# a comment\n"
                           "; another comment style\n"
                           "\n"
                           "confirmRecycleBinDelete = false\n");

    auto result = LoadConfigFrom(path);

    CHECK(!result.config.confirmRecycleBinDelete);
    CHECK(result.config.confirmPermanentDelete);  // untouched key keeps its default
    CHECK(result.warnings.empty());
}

TEST_CASE(LoadConfigFrom_UnrecognizedKey_IsIgnoredWithoutWarning_CFG004) {
    test::TempDir dir;
    auto path = dir.Path() / L"mycommander.ini";
    test::WriteFileContent(path, "someFutureSetting = whatever\nconfirmRecycleBinDelete = false\n");

    auto result = LoadConfigFrom(path);

    CHECK(!result.config.confirmRecycleBinDelete);
    CHECK(result.warnings.empty());
}

TEST_CASE(LoadConfigFrom_InvalidBooleanValue_IsReportedAndKeepsDefault_CFG005) {
    test::TempDir dir;
    auto path = dir.Path() / L"mycommander.ini";
    test::WriteFileContent(path, "confirmRecycleBinDelete = maybe\n");

    auto result = LoadConfigFrom(path);

    CHECK(result.config.confirmRecycleBinDelete);  // kept the safe default
    CHECK(result.warnings.size() == 1);
    if (!result.warnings.empty()) {
        CHECK(result.warnings[0].find(L"confirmrecyclebindelete") != std::wstring::npos);
        CHECK(result.warnings[0].find(L"maybe") != std::wstring::npos);
    }
}

TEST_CASE(LoadConfigFrom_LineWithoutEquals_IsReportedAsAWarning) {
    test::TempDir dir;
    auto path = dir.Path() / L"mycommander.ini";
    test::WriteFileContent(path, "this line has no equals sign\n");

    auto result = LoadConfigFrom(path);

    CHECK(result.warnings.size() == 1);
}

TEST_CASE(EnsureConfigFileExistsAt_CreatesFileWhenMissing) {
    test::TempDir dir;
    auto path = dir.Path() / L"subdir" / L"mycommander.ini";

    EnsureConfigFileExistsAt(path);

    CHECK(std::filesystem::exists(path));
    auto result = LoadConfigFrom(path);
    CHECK(result.config.confirmRecycleBinDelete);
    CHECK(result.config.confirmPermanentDelete);
    CHECK(result.config.enterFileAction == EnterFileAction::View);
    CHECK(!result.config.useBasicSymbols);
    CHECK(result.config.persistCommandHistory);
    CHECK(result.config.logLevel == LogLevel::Off);
    CHECK(result.warnings.empty());
}

TEST_CASE(EnsureConfigFileExistsAt_DoesNotOverwriteAnExistingFile) {
    test::TempDir dir;
    auto path = dir.Path() / L"mycommander.ini";
    test::WriteFileContent(path, "confirmRecycleBinDelete = false\n");

    EnsureConfigFileExistsAt(path);

    auto result = LoadConfigFrom(path);
    CHECK(!result.config.confirmRecycleBinDelete);  // untouched, not reset to the written default
}

// --- LoadConfigFrom's presentKeys / AppendMissingConfigKeys (IS-0006) --------

TEST_CASE(LoadConfigFrom_PresentKeys_IncludesOnlyKeysActuallyInTheFile_IS0006) {
    test::TempDir dir;
    auto path = dir.Path() / L"mycommander.ini";
    test::WriteFileContent(path, "confirmRecycleBinDelete = true\nuseBasicSymbols = false\n");

    auto result = LoadConfigFrom(path);

    CHECK(result.presentKeys.count(L"confirmrecyclebindelete") == 1);
    CHECK(result.presentKeys.count(L"usebasicsymbols") == 1);
    CHECK(result.presentKeys.count(L"colortheme") == 0);
    CHECK(result.presentKeys.size() == 2);
}

TEST_CASE(LoadConfigFrom_PresentKeys_IncludesAKeyEvenIfItsValueWasInvalid_IS0006) {
    // A typo'd value is a separate, already-reported problem (`warnings`) --
    // the key itself was still mentioned in the file, so it isn't "missing".
    test::TempDir dir;
    auto path = dir.Path() / L"mycommander.ini";
    test::WriteFileContent(path, "confirmRecycleBinDelete = maybe\n");

    auto result = LoadConfigFrom(path);

    CHECK(result.presentKeys.count(L"confirmrecyclebindelete") == 1);
    CHECK(!result.warnings.empty());
}

TEST_CASE(LoadConfigFrom_PresentKeys_ExcludesUnrecognizedKeys_IS0006) {
    test::TempDir dir;
    auto path = dir.Path() / L"mycommander.ini";
    test::WriteFileContent(path, "notARealSetting = true\n");

    auto result = LoadConfigFrom(path);

    CHECK(result.presentKeys.empty());
}

TEST_CASE(AppendMissingConfigKeys_NothingMissing_IsANoOpAndReturnsEmpty_IS0006) {
    test::TempDir dir;
    auto path = dir.Path() / L"mycommander.ini";
    EnsureConfigFileExistsAt(path);  // writes every known key
    auto before = LoadConfigFrom(path);
    auto sizeBefore = std::filesystem::file_size(path);

    auto added = AppendMissingConfigKeys(path, before.presentKeys);

    CHECK(added.empty());
    CHECK(std::filesystem::file_size(path) == sizeBefore);
}

TEST_CASE(AppendMissingConfigKeys_AppendsOnlyTheMissingOnes_LeavesExistingLinesUntouched_IS0006) {
    test::TempDir dir;
    auto path = dir.Path() / L"mycommander.ini";
    test::WriteFileContent(path, "confirmRecycleBinDelete = false\n");
    auto before = LoadConfigFrom(path);  // presentKeys = {confirmrecyclebindelete}

    auto added = AppendMissingConfigKeys(path, before.presentKeys);

    CHECK(!added.empty());
    CHECK(std::find(added.begin(), added.end(), L"colortheme") != added.end());
    CHECK(std::find(added.begin(), added.end(), L"confirmrecyclebindelete") == added.end());

    auto after = LoadConfigFrom(path);
    CHECK(!after.config.confirmRecycleBinDelete);  // the user's own value, still respected
    CHECK(after.presentKeys.size() > before.presentKeys.size());
    CHECK(after.presentKeys.count(L"colortheme") == 1);
}

TEST_CASE(AppendMissingConfigKeys_EmptyPathIsANoOp_IS0006) {
    auto added = AppendMissingConfigKeys(std::filesystem::path{}, {});
    CHECK(added.empty());
}
