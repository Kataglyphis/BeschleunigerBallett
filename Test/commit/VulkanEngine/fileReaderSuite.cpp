// Deterministic twin of the reader fuzz tests: fuzzing pins no return values and skips the Windows CPU lane.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <vulkan/vulkan.hpp>

#include "scene/TextureFit.hpp"

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

// Past maxImageDimension2D vmaCreateImage failed and vkCheck aborted; uploadRgba now starts at the first level that fits.
TEST(TextureLoadUnit, AnOversizedTextureIsHalvedUntilItFitsTheDeviceLimit)
{
    const std::vector<unsigned char> grey(8U * 4U * 4U, 200);
    const Kataglyphis::FittedRgba8 fitted = Kataglyphis::fitRgba8ToLimit(grey.data(), 8, 4, 2, false);
    EXPECT_EQ(fitted.width, 2U);
    EXPECT_EQ(fitted.height, 1U);
    EXPECT_EQ(fitted.halvings, 2U);
    ASSERT_EQ(fitted.texels.size(), 2U * 1U * 4U);
    for (const unsigned char texel : fitted.texels) { EXPECT_EQ(texel, 200); }
    EXPECT_EQ(Kataglyphis::mipLevelCount(fitted.width, fitted.height), 2U) << "the mips must follow the fitted extent";
    EXPECT_EQ(Kataglyphis::mipLevelCount(8, 4), 4U);

    // An odd side drops its last row or column, as a mip chain does: 5x3 is 2x1 one level down.
    const std::vector<unsigned char> odd(5U * 3U * 4U, 10);
    const Kataglyphis::FittedRgba8 odd_fitted = Kataglyphis::fitRgba8ToLimit(odd.data(), 5, 3, 2, true);
    EXPECT_EQ(odd_fitted.width, 2U);
    EXPECT_EQ(odd_fitted.height, 1U);
    EXPECT_EQ(odd_fitted.halvings, 1U);
}

// createFromLevels uploads the planned levels; a chain no level of which fits once read front() of nothing.
TEST(TextureLoadUnit, AChainPlanKeepsTheLevelsThatFitAndNoneWhenNothingFits)
{
    const Kataglyphis::ChainPlan all = Kataglyphis::planChainUpload(4, 2, 3, 16);
    ASSERT_EQ(all.levels.size(), 3U);
    EXPECT_EQ(all.totalBytes, 32U + 8U + 4U);
    EXPECT_EQ(all.firstOffset, 0U);
    EXPECT_EQ(all.levels[1].offset, 32U);
    EXPECT_EQ(all.levels[2].width, 1U);

    const Kataglyphis::ChainPlan fitted = Kataglyphis::planChainUpload(4, 2, 3, 2);
    ASSERT_EQ(fitted.levels.size(), 2U) << "the 4x2 base is over a 2-texel side";
    EXPECT_EQ(fitted.firstOffset, 32U);
    EXPECT_EQ(fitted.levels[0].offset, 0U);
    EXPECT_EQ(fitted.levels[0].width, 2U);

    const Kataglyphis::ChainPlan none = Kataglyphis::planChainUpload(4, 2, 3, 0);
    EXPECT_TRUE(none.levels.empty());
    EXPECT_EQ(none.totalBytes, 44U);

    const Kataglyphis::ChainPlan deep = Kataglyphis::planChainUpload(2, 2, 40, 16);
    EXPECT_EQ(deep.levels.size(), 32U) << "no shift past a 32-bit side";
}

TEST(TextureLoadUnit, ATextureWithinTheDeviceLimitKeepsItsTexels)
{
    std::vector<unsigned char> rgba(4U * 4U * 4U);
    for (size_t i = 0; i < rgba.size(); ++i) { rgba[i] = static_cast<unsigned char>(i); }
    const Kataglyphis::FittedRgba8 fitted = Kataglyphis::fitRgba8ToLimit(rgba.data(), 4, 4, 4, true);
    EXPECT_EQ(fitted.halvings, 0U);
    EXPECT_EQ(fitted.width, 4U);
    EXPECT_EQ(fitted.height, 4U);
    EXPECT_EQ(fitted.texels, rgba);
}

TEST(TextureLoadUnit, HalvingAveragesSrgbColourInLinearSpace)
{
    // Black and white: linear 0.5 encodes to 188, where a byte average gives 128; Rust's generate_mips does the same.
    const std::vector<unsigned char> rgba = { 0, 0, 0, 0, 255, 255, 255, 255 };
    const Kataglyphis::FittedRgba8 srgb = Kataglyphis::halveRgba8(rgba.data(), 2, 1, true);
    ASSERT_EQ(srgb.texels.size(), 4U);
    EXPECT_EQ(srgb.texels[0], 188);
    EXPECT_EQ(srgb.texels[2], 188);
    EXPECT_EQ(srgb.texels[3], 128) << "alpha is linear in an sRGB texture too";
    const Kataglyphis::FittedRgba8 unorm = Kataglyphis::halveRgba8(rgba.data(), 2, 1, false);
    EXPECT_EQ(unorm.texels[0], 128);
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

// The cached Resources roots must not stop the walk at the nearest tree: an outer one can hold what an inner lacks.
TEST(ResourcePathsUnit, FindsAnOuterResourcesTreeWhenTheInnerOneLacksTheFile)
{
    const auto root = uniqueTempPath("kat_respath_nested");
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root / "inner" / "Resources", ec);
    std::filesystem::create_directories(root / "Resources" / "Models", ec);
    ASSERT_FALSE(ec);

    ScopedWorkingDirectory cwdGuard(root / "inner");
    const auto resolved = resolveResourceRelativePath("Models");
    ASSERT_TRUE(resolved.has_value());
    EXPECT_TRUE(std::filesystem::equivalent(*resolved, root / "Resources" / "Models"));

    std::filesystem::remove_all(root, ec);
}

// Only the Resources directories are cached, never an answer: a file that appears after a miss is found.
TEST(ResourcePathsUnit, AFileCreatedAfterAMissResolves)
{
    const auto root = uniqueTempPath("kat_respath_late_file");
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root / "Resources", ec);
    ASSERT_FALSE(ec);

    ScopedWorkingDirectory cwdGuard(root);
    EXPECT_FALSE(resolveResourceRelativePath("Models").has_value());
    std::filesystem::create_directories(root / "Resources" / "Models", ec);
    ASSERT_FALSE(ec);
    const auto resolved = resolveResourceRelativePath("Models");
    ASSERT_TRUE(resolved.has_value());
    EXPECT_TRUE(std::filesystem::equivalent(*resolved, root / "Resources" / "Models"));

    std::filesystem::remove_all(root, ec);
}

// The cache is keyed on the working directory, so a chdir must not keep answering from the old tree.
TEST(ResourcePathsUnit, FollowsAWorkingDirectoryChange)
{
    const auto first = uniqueTempPath("kat_respath_cwd_a");
    const auto second = uniqueTempPath("kat_respath_cwd_b");
    std::error_code ec;
    std::filesystem::remove_all(first, ec);
    std::filesystem::remove_all(second, ec);
    std::filesystem::create_directories(first / "Resources" / "OnlyInFirst", ec);
    std::filesystem::create_directories(second / "Resources" / "OnlyInSecond", ec);
    ASSERT_FALSE(ec);

    {
        ScopedWorkingDirectory cwdGuard(first);
        EXPECT_TRUE(resolveResourceRelativePath("OnlyInFirst").has_value());
        EXPECT_FALSE(resolveResourceRelativePath("OnlyInSecond").has_value());
    }
    {
        ScopedWorkingDirectory cwdGuard(second);
        EXPECT_TRUE(resolveResourceRelativePath("OnlyInSecond").has_value());
        EXPECT_FALSE(resolveResourceRelativePath("OnlyInFirst").has_value());
    }

    std::filesystem::remove_all(first, ec);
    std::filesystem::remove_all(second, ec);
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
