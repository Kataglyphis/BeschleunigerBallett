// buildIntegritySuite.cpp relies on these readers for every source scan.

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "RepoFiles.hpp"

using Kataglyphis::TestSupport::joinViolations;
using Kataglyphis::TestSupport::readFileLines;
using Kataglyphis::TestSupport::readFileText;

namespace {

std::filesystem::path uniqueTempPath(const std::string &name) { return std::filesystem::temp_directory_path() / name; }

void writeFile(const std::filesystem::path &path, const std::string &contents)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

}// namespace

TEST(RepoFilesUnit, ReadFileLinesNulloptForMissingPath)
{
    const auto missing = uniqueTempPath("kat_repofiles_missing.txt");
    std::error_code ec;
    std::filesystem::remove(missing, ec);

    EXPECT_FALSE(readFileLines(missing).has_value());
}

TEST(RepoFilesUnit, ReadFileLinesNulloptForDirectoryPath)
{
    const auto dir = uniqueTempPath("kat_repofiles_dir");
    std::error_code ec;
    std::filesystem::create_directory(dir, ec);

    EXPECT_FALSE(readFileLines(dir).has_value());

    std::filesystem::remove(dir, ec);
}

// On Linux a directory opens fine and only fails on read.
TEST(RepoFilesUnit, ReadFileTextNulloptForDirectoryPath)
{
    const auto dir = uniqueTempPath("kat_repofiles_text_dir");
    std::error_code ec;
    std::filesystem::create_directory(dir, ec);

    EXPECT_FALSE(readFileText(dir).has_value());

    std::filesystem::remove(dir, ec);
}

TEST(RepoFilesUnit, ReadFileTextNulloptForMissingPath)
{
    const auto missing = uniqueTempPath("kat_repofiles_text_missing.txt");
    std::error_code ec;
    std::filesystem::remove(missing, ec);

    EXPECT_FALSE(readFileText(missing).has_value());
}

TEST(RepoFilesUnit, ReadFileTextEmptyFileReturnsEmptyStringNotNullopt)
{
    const auto path = uniqueTempPath("kat_repofiles_text_empty.txt");
    writeFile(path, "");

    const auto text = readFileText(path);
    ASSERT_TRUE(text.has_value());
    EXPECT_TRUE(text->empty());

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(RepoFilesUnit, ReadFileTextKeepsBytesVerbatim)
{
    const auto path = uniqueTempPath("kat_repofiles_text_verbatim.txt");
    writeFile(path, "a\r\nb\n");

    const auto text = readFileText(path);
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(*text, "a\r\nb\n") << "binary mode must not translate line endings on Windows";

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(RepoFilesUnit, ReadFileLinesEmptyFileReturnsEmptyVectorNotNullopt)
{
    const auto path = uniqueTempPath("kat_repofiles_empty.txt");
    writeFile(path, "");

    const auto lines = readFileLines(path);
    ASSERT_TRUE(lines.has_value());
    EXPECT_TRUE(lines->empty());

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(RepoFilesUnit, ReadFileLinesSplitsOnLf)
{
    const auto path = uniqueTempPath("kat_repofiles_lf.txt");
    writeFile(path, "a\nb\n");

    const auto lines = readFileLines(path);
    ASSERT_TRUE(lines.has_value());
    EXPECT_EQ(*lines, (std::vector<std::string>{ "a", "b" }));

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(RepoFilesUnit, ReadFileLinesStripsCrlf)
{
    const auto path = uniqueTempPath("kat_repofiles_crlf.txt");
    writeFile(path, "a\r\nb\r\n");

    const auto lines = readFileLines(path);
    ASSERT_TRUE(lines.has_value());
    EXPECT_EQ(*lines, (std::vector<std::string>{ "a", "b" }))
      << "CRLF line endings must produce the same lines on Linux and Windows";

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(RepoFilesUnit, ReadFileLinesReturnsLastLineWithNoTrailingNewline)
{
    const auto path = uniqueTempPath("kat_repofiles_no_trailing_newline.txt");
    writeFile(path, "a\nb");

    const auto lines = readFileLines(path);
    ASSERT_TRUE(lines.has_value());
    EXPECT_EQ(*lines, (std::vector<std::string>{ "a", "b" }));

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(RepoFilesUnit, JoinViolationsEmptyVectorReturnsEmptyString)
{
    EXPECT_EQ(joinViolations({}), "");
}

TEST(RepoFilesUnit, JoinViolationsDefaultPrefixesEachEntryWithIndentedNewline)
{
    EXPECT_EQ(joinViolations({ "one" }), "\n  one");
    EXPECT_EQ(joinViolations({ "one", "two" }), "\n  one\n  two");
}

TEST(RepoFilesUnit, JoinViolationsSupportsExplicitPrefixAndSuffix)
{
    EXPECT_EQ(joinViolations({ "one", "two" }, "", "\n"), "one\ntwo\n");
    EXPECT_EQ(joinViolations({ "x" }, "\n  add `", "` to the srgb row's .mtl column"),
      "\n  add `x` to the srgb row's .mtl column");
}
