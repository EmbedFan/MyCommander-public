#include "TestFramework.h"

#include "MarkdownRenderer.h"

using mc::MarkdownLine;
using mc::MarkdownLineKind;
using mc::ParseMinimalMarkdown;

// --- ParseMinimalMarkdown (FEA-0002 §7) --------------------------------------

TEST_CASE(ParseMinimalMarkdown_EmptySource_ReturnsNoLines) {
    CHECK(ParseMinimalMarkdown(L"").empty());
}

TEST_CASE(ParseMinimalMarkdown_Heading_StripsMarkerAndSpace) {
    auto lines = ParseMinimalMarkdown(L"# Title");
    CHECK(lines.size() == 1);
    CHECK(lines[0].kind == MarkdownLineKind::Heading);
    CHECK(lines[0].text == L"Title");
}

TEST_CASE(ParseMinimalMarkdown_MultiHashHeading_LevelNotTracked) {
    auto lines = ParseMinimalMarkdown(L"## Sub-heading");
    CHECK(lines.size() == 1);
    CHECK(lines[0].kind == MarkdownLineKind::Heading);
    CHECK(lines[0].text == L"Sub-heading");
}

TEST_CASE(ParseMinimalMarkdown_HashWithNoFollowingSpace_IsPlainParagraph) {
    auto lines = ParseMinimalMarkdown(L"#nospace");
    CHECK(lines.size() == 1);
    CHECK(lines[0].kind == MarkdownLineKind::Paragraph);
    CHECK(lines[0].text == L"#nospace");
}

TEST_CASE(ParseMinimalMarkdown_DashBullet_StripsMarkerAndSpace) {
    auto lines = ParseMinimalMarkdown(L"- first item");
    CHECK(lines.size() == 1);
    CHECK(lines[0].kind == MarkdownLineKind::Bullet);
    CHECK(lines[0].text == L"first item");
}

TEST_CASE(ParseMinimalMarkdown_StarBullet_StripsMarkerAndSpace) {
    auto lines = ParseMinimalMarkdown(L"* second item");
    CHECK(lines.size() == 1);
    CHECK(lines[0].kind == MarkdownLineKind::Bullet);
    CHECK(lines[0].text == L"second item");
}

TEST_CASE(ParseMinimalMarkdown_PlainLine_IsParagraph) {
    auto lines = ParseMinimalMarkdown(L"Just a sentence.");
    CHECK(lines.size() == 1);
    CHECK(lines[0].kind == MarkdownLineKind::Paragraph);
    CHECK(lines[0].text == L"Just a sentence.");
}

TEST_CASE(ParseMinimalMarkdown_BlankLine_IsBlankWithEmptyText) {
    auto lines = ParseMinimalMarkdown(L"   ");
    CHECK(lines.size() == 1);
    CHECK(lines[0].kind == MarkdownLineKind::Blank);
    CHECK(lines[0].text.empty());
}

TEST_CASE(ParseMinimalMarkdown_InlineCodeSpan_BackticksStripped) {
    auto lines = ParseMinimalMarkdown(L"Run `mc --version` to check.");
    CHECK(lines.size() == 1);
    CHECK(lines[0].kind == MarkdownLineKind::Paragraph);
    CHECK(lines[0].text == L"Run mc --version to check.");
}

TEST_CASE(ParseMinimalMarkdown_InlineCodeInHeadingAndBullet_AlsoStripped) {
    auto headingLines = ParseMinimalMarkdown(L"# `Code` Title");
    CHECK(headingLines[0].text == L"Code Title");
    auto bulletLines = ParseMinimalMarkdown(L"- `Code` item");
    CHECK(bulletLines[0].text == L"Code item");
}

TEST_CASE(ParseMinimalMarkdown_MultipleLines_SplitOnLF) {
    auto lines = ParseMinimalMarkdown(L"# Title\n\n- item one\n- item two\nParagraph text");
    CHECK(lines.size() == 5);
    CHECK(lines[0].kind == MarkdownLineKind::Heading);
    CHECK(lines[1].kind == MarkdownLineKind::Blank);
    CHECK(lines[2].kind == MarkdownLineKind::Bullet);
    CHECK(lines[2].text == L"item one");
    CHECK(lines[3].kind == MarkdownLineKind::Bullet);
    CHECK(lines[3].text == L"item two");
    CHECK(lines[4].kind == MarkdownLineKind::Paragraph);
    CHECK(lines[4].text == L"Paragraph text");
}

TEST_CASE(ParseMinimalMarkdown_CRLFLineEndings_TrimsTrailingCR) {
    auto lines = ParseMinimalMarkdown(L"# Title\r\n- item\r\n");
    CHECK(lines.size() == 3);
    CHECK(lines[0].kind == MarkdownLineKind::Heading);
    CHECK(lines[0].text == L"Title");
    CHECK(lines[1].kind == MarkdownLineKind::Bullet);
    CHECK(lines[1].text == L"item");
    CHECK(lines[2].kind == MarkdownLineKind::Blank);  // trailing newline produces a trailing blank line
}

TEST_CASE(ParseMinimalMarkdown_UnrecognizedSyntax_DegradesToParagraph) {
    auto lines = ParseMinimalMarkdown(L"| table | cell |");
    CHECK(lines.size() == 1);
    CHECK(lines[0].kind == MarkdownLineKind::Paragraph);
    CHECK(lines[0].text == L"| table | cell |");
}
