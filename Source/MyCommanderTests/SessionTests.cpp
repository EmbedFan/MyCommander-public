#include "TestFixtures.h"
#include "TestFramework.h"

#include "Session.h"

using mc::LoadSessionFrom;
using mc::SaveSessionTo;
using mc::SessionState;

TEST_CASE(Session_NonexistentFile_HasNoSavedLocations_NAV008) {
    test::TempDir directory;
    const SessionState state = LoadSessionFrom(directory.Path() / L"missing.ini");

    CHECK(!state.leftPath.has_value());
    CHECK(!state.rightPath.has_value());
}

TEST_CASE(Session_RoundTripsBothPanelLocations_NAV008) {
    test::TempDir directory;
    const auto sessionPath = directory.Path() / L"nested" / L"session.ini";
    const SessionState saved{directory.Path() / L"left panel", directory.Path() / L"right panel"};

    CHECK(SaveSessionTo(sessionPath, saved));
    const SessionState loaded = LoadSessionFrom(sessionPath);

    CHECK(loaded.leftPath.has_value());
    CHECK(loaded.rightPath.has_value());
    if (loaded.leftPath) CHECK(*loaded.leftPath == *saved.leftPath);
    if (loaded.rightPath) CHECK(*loaded.rightPath == *saved.rightPath);
}

TEST_CASE(Session_IgnoresMalformedAndIncompleteValues_NAV008) {
    test::TempDir directory;
    const auto sessionPath = directory.Path() / L"session.ini";
    test::WriteFileContent(sessionPath, "not a setting\nleftPath = C:\\saved-left\nrightPath = \n");

    const SessionState loaded = LoadSessionFrom(sessionPath);

    CHECK(loaded.leftPath.has_value());
    if (loaded.leftPath) CHECK(*loaded.leftPath == std::filesystem::path(L"C:\\saved-left"));
    CHECK(!loaded.rightPath.has_value());
}

TEST_CASE(Session_RemembersTheSinglePanelLayout_UI013) {
    test::TempDir directory;
    const auto sessionPath = directory.Path() / L"session.ini";
    SessionState saved{directory.Path() / L"l", directory.Path() / L"r", /*singlePanel=*/true};
    CHECK(SaveSessionTo(sessionPath, saved));
    CHECK(LoadSessionFrom(sessionPath).singlePanel);
    saved.singlePanel = false;
    CHECK(SaveSessionTo(sessionPath, saved));
    CHECK(!LoadSessionFrom(sessionPath).singlePanel);
}

TEST_CASE(Session_OlderFileOrBadValueMeansDualPane_UI013) {
    test::TempDir directory;
    const auto sessionPath = directory.Path() / L"session.ini";
    test::WriteFileContent(sessionPath, "leftPath = C:\\a\nrightPath = C:\\b\n");
    CHECK(!LoadSessionFrom(sessionPath).singlePanel);
    test::WriteFileContent(sessionPath, "leftPath = C:\\a\nrightPath = C:\\b\nsinglePanel = maybe\n");
    CHECK(!LoadSessionFrom(sessionPath).singlePanel);
    test::WriteFileContent(sessionPath, "singlePanel = YES\n");
    CHECK(LoadSessionFrom(sessionPath).singlePanel);
}

TEST_CASE(Session_RemembersBriefViewPerPanel_UI017) {
    test::TempDir directory;
    const auto sessionPath = directory.Path() / L"session.ini";
    SessionState saved{directory.Path() / L"l", directory.Path() / L"r"};
    saved.leftBrief = true;
    saved.rightBrief = false;
    CHECK(SaveSessionTo(sessionPath, saved));
    SessionState loaded = LoadSessionFrom(sessionPath);
    CHECK(loaded.leftBrief);
    CHECK(!loaded.rightBrief);
}

TEST_CASE(Session_OlderFileOrBadValueMeansFullView_UI017) {
    test::TempDir directory;
    const auto sessionPath = directory.Path() / L"session.ini";
    test::WriteFileContent(sessionPath, "leftPath = C:\\a\nrightPath = C:\\b\n");
    CHECK(!LoadSessionFrom(sessionPath).leftBrief);
    CHECK(!LoadSessionFrom(sessionPath).rightBrief);
    test::WriteFileContent(sessionPath, "leftPath = C:\\a\nrightPath = C:\\b\nleftBrief = maybe\n");
    CHECK(!LoadSessionFrom(sessionPath).leftBrief);
    test::WriteFileContent(sessionPath, "leftPath = C:\\a\nrightPath = C:\\b\nleftBrief = YES\nrightBrief = true\n");
    CHECK(LoadSessionFrom(sessionPath).leftBrief);
    CHECK(LoadSessionFrom(sessionPath).rightBrief);
}
