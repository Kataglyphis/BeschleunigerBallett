// Deterministic twin of the reader fuzz tests: fuzzing pins no return values and skips the Windows CPU lane.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <vulkan/vulkan.hpp>

import kataglyphis.shared.util.file_reader;
import kataglyphis.shared.util.resource_paths;
import kataglyphis.vulkan.texture;

using Kataglyphis::Shared::fileExists;
using Kataglyphis::Shared::getBaseDir;
using Kataglyphis::Shared::isWindowsReservedDeviceName;
using Kataglyphis::Shared::readBinaryFile;
using Kataglyphis::Shared::readTextFile;
using Kataglyphis::Shared::resolveResourceRelativePath;

namespace {

std::filesystem::path uniqueTempPath(const std::string &name) { return std::filesystem::temp_directory_path() / name; }

// The working directory is process-global and resolveResourceRelativePath reads it, so always restore it.
class ScopedWorkingDirectory
{
  public:
    explicit ScopedWorkingDirectory(const std::filesystem::path &newCwd)
    {
        std::error_code ec;
        original_ = std::filesystem::current_path(ec);
        std::filesystem::current_path(newCwd, ec);
    }
    ~ScopedWorkingDirectory()
    {
        std::error_code ec;
        std::filesystem::current_path(original_, ec);
    }
    ScopedWorkingDirectory(const ScopedWorkingDirectory &) = delete;
    ScopedWorkingDirectory &operator=(const ScopedWorkingDirectory &) = delete;

  private:
    std::filesystem::path original_;
};

}// namespace

TEST(FileReaderUnit, FileExistsFalseForMissingPath)
{
    const auto missing = uniqueTempPath("kat_filereader_does_not_exist.txt");
    std::error_code ec;
    std::filesystem::remove(missing, ec);

    EXPECT_FALSE(fileExists(missing.string()));
}

TEST(FileReaderUnit, FileExistsFalseNotCrashForUnstattablePath)
{
    // An overlong name makes the OS query fail; with exceptions disabled the throwing exists() would terminate.
    const std::string overlong_component(300, 'a');
    const std::string unstattable = (std::filesystem::temp_directory_path() / overlong_component).string();

    EXPECT_FALSE(fileExists(unstattable));
}

TEST(FileReaderUnit, ReadTextFileEmptyForMissingPath)
{
    const auto missing = uniqueTempPath("kat_filereader_missing.txt");
    std::error_code ec;
    std::filesystem::remove(missing, ec);

    EXPECT_TRUE(readTextFile(missing.string()).empty());
}

TEST(FileReaderUnit, ReadTextFileEmptyForDirectoryPath)
{
    const auto dir = uniqueTempPath("kat_filereader_text_dir");
    std::error_code ec;
    std::filesystem::create_directory(dir, ec);

    EXPECT_TRUE(readTextFile(dir.string()).empty());

    std::filesystem::remove(dir, ec);
}

// Opening a DOS device name such as "con" on Windows attaches the console and blocks the reader forever.
TEST(FileReaderUnit, WindowsDeviceNamesAreRecognisedInEveryForm)
{
    for (const char *device : { "con",
           "CON",
           "Con.TXT",
           "shaders/con",
           "shaders\\con.spv",
           "con . ",
           "nul",
           "prn",
           "aux",
           "com0",
           "com1",
           "COM9",
           "lpt3" }) {
        EXPECT_TRUE(isWindowsReservedDeviceName(device)) << device << " is a Windows device name";
    }

    // Windows resolves only an exact stem, so names that merely contain a device name are ordinary files.
    for (const char *ordinary : { "console",
           "connect.spv",
           "acon",
           "my.con",
           "com",
           "com10",
           "lpt",
           "",
           "Resources/ShadersSlang/build/spirv/rasterizer/rasterizer.fs_main.spv" }) {
        EXPECT_FALSE(isWindowsReservedDeviceName(ordinary)) << ordinary << " is an ordinary filename";
    }
}

// A character device opens cleanly and never finishes reading; the Windows twin would need the console to prove.
#ifndef _WIN32
TEST(FileReaderUnit, ReadersRefuseCharacterDevicesInsteadOfBlocking)
{
    EXPECT_TRUE(readTextFile("/dev/zero").empty()) << "readTextFile must not read a character device";
    EXPECT_TRUE(readBinaryFile("/dev/zero").empty()) << "readBinaryFile must not read a character device";
}
#endif

TEST(FileReaderUnit, ReadBinaryFileEmptyForMissingPath)
{
    const auto missing = uniqueTempPath("kat_filereader_missing.bin");
    std::error_code ec;
    std::filesystem::remove(missing, ec);

    EXPECT_TRUE(readBinaryFile(missing.string()).empty());
}

TEST(FileReaderUnit, ReadBinaryFileEmptyForDirectoryPath)
{
    const auto dir = uniqueTempPath("kat_filereader_binary_dir");
    std::error_code ec;
    std::filesystem::create_directory(dir, ec);

    EXPECT_TRUE(readBinaryFile(dir.string()).empty());

    std::filesystem::remove(dir, ec);
}

TEST(FileReaderUnit, ReadBinaryFileRoundTripsEmbeddedNulAndCrlfBytes)
{
    const auto path = uniqueTempPath("kat_filereader_roundtrip.bin");
    const std::vector<char> contents = { 'A', '\0', '\r', '\n', 'B', '\r', '\n', '\0' };
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    }

    const std::vector<char> read_back = readBinaryFile(path.string());
    EXPECT_EQ(read_back, contents) << "binary read must not do text translation "
                                      "(CRLF collapsing / NUL truncation)";

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(FileReaderUnit, ReadBinaryFileEmptyFileReturnsEmptyVectorNotNullRead)
{
    const auto path = uniqueTempPath("kat_filereader_empty.bin");
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
    }

    EXPECT_TRUE(readBinaryFile(path.string()).empty());

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(FileReaderUnit, ReadTextFileAppendsTrailingNewlineWhenSourceHasNone)
{
    const auto path = uniqueTempPath("kat_filereader_no_trailing_newline.txt");
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "abc";// deliberately no trailing '\n'
    }

    EXPECT_EQ(readTextFile(path.string()), "abc\n");

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(FileReaderUnit, ReadTextFilePreservesEmbeddedNulByte)
{
    // std::getline stops at '\n', not NUL, so an embedded NUL must survive rather than truncate.
    const auto path = uniqueTempPath("kat_filereader_embedded_nul.bin");
    const std::vector<char> contents = { 'A', '\0', 'B', '\n' };
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    }

    const std::string text = readTextFile(path.string());
    const std::string expected(contents.data(), contents.size());
    EXPECT_EQ(text, expected) << "embedded NUL byte was dropped or truncated by a text-mode read";

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(FileReaderUnit, GetBaseDirSlashOnly) { EXPECT_EQ(getBaseDir("a/b/c.txt"), "a/b"); }

TEST(FileReaderUnit, GetBaseDirBackslashOnly) { EXPECT_EQ(getBaseDir("a\\b\\c.txt"), "a\\b"); }

TEST(FileReaderUnit, GetBaseDirMixedSeparators) { EXPECT_EQ(getBaseDir("a/b\\c.txt"), "a/b"); }

TEST(FileReaderUnit, GetBaseDirNoSeparatorReturnsEmpty) { EXPECT_TRUE(getBaseDir("c.txt").empty()); }

// Texture::loadTextureData is static and device-free, so it is tested here rather than in a GPU suite.
TEST(TextureLoadUnit, FailedDecodeZeroesAllOutputs)
{
    const auto path = uniqueTempPath("kat_texture_not_an_image.bin");
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        const std::vector<uint8_t> garbage = { 0x00, 0x01, 0x02, 0x03, 0x04 };
        out.write(reinterpret_cast<const char *>(garbage.data()), static_cast<std::streamsize>(garbage.size()));
    }

    // Sentinels, so a failed decode that leaves outputs untouched would size a staging buffer from garbage.
    int width = -1;
    int height = -1;
    vk::DeviceSize image_size = 4;
    unsigned char *pixels = Kataglyphis::Texture::loadTextureData(path.string(), &width, &height, &image_size);

    EXPECT_EQ(pixels, nullptr);
    EXPECT_EQ(width, 0);
    EXPECT_EQ(height, 0);
    EXPECT_EQ(image_size, 0U);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

// A scratch tree pins the Resources/ walk independent of the real repo layout.
TEST(ResourcePathsUnit, ResolvesAtDepthZero)
{
    const auto root = uniqueTempPath("kat_respath_depth0");
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root / "Resources" / "Models", ec);
    ASSERT_FALSE(ec);

    ScopedWorkingDirectory cwdGuard(root);
    const auto resolved = resolveResourceRelativePath("Models");
    ASSERT_TRUE(resolved.has_value());
    EXPECT_TRUE(std::filesystem::equivalent(*resolved, root / "Resources" / "Models"));

    std::filesystem::remove_all(root, ec);
}

TEST(ResourcePathsUnit, ResolvesAtDepthThree)
{
    const auto root = uniqueTempPath("kat_respath_depth3");
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    const auto leaf = root / "a" / "b" / "c";
    std::filesystem::create_directories(leaf, ec);
    std::filesystem::create_directories(root / "Resources" / "Models", ec);
    ASSERT_FALSE(ec);

    ScopedWorkingDirectory cwdGuard(leaf);
    const auto resolved = resolveResourceRelativePath("Models");
    ASSERT_TRUE(resolved.has_value());
    EXPECT_TRUE(std::filesystem::equivalent(*resolved, root / "Resources" / "Models"));

    std::filesystem::remove_all(root, ec);
}

TEST(ResourcePathsUnit, MissesPastTheDepthCap)
{
    const auto root = uniqueTempPath("kat_respath_toodeep");
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    // kResourceSearchDepth 8 is cwd plus 7 parents, so a leaf 8 levels down is one hop out of reach.
    auto leaf = root;
    for (int i = 0; i < 8; ++i) { leaf /= "d" + std::to_string(i); }
    std::filesystem::create_directories(leaf, ec);
    std::filesystem::create_directories(root / "Resources" / "Models", ec);
    ASSERT_FALSE(ec);

    ScopedWorkingDirectory cwdGuard(leaf);
    EXPECT_FALSE(resolveResourceRelativePath("Models").has_value());

    std::filesystem::remove_all(root, ec);
}

TEST(ResourcePathsUnit, RejectsRelativePathEscapingTheTree)
{
    const auto root = uniqueTempPath("kat_respath_escape");
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    ASSERT_FALSE(ec);

    ScopedWorkingDirectory cwdGuard(root);
    EXPECT_FALSE(resolveResourceRelativePath("../etc/passwd").has_value());

    std::filesystem::remove_all(root, ec);
}
