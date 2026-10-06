// A truncated or empty .spv reaching vkCreateShaderModule is undefined driver behaviour in release.

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <type_traits>
#include <vector>

import kataglyphis.vulkan.shader_helper;

using Kataglyphis::ShaderStagePair;
using Kataglyphis::validateSpirvBlob;

namespace {

// SPIR-V magic number, little endian - a real shader's first four bytes.
std::vector<char> spirvMagicBytes()
{ return { static_cast<char>(0x03), static_cast<char>(0x02), static_cast<char>(0x23), static_cast<char>(0x07) }; }

}// namespace

TEST(ShaderBlobUnit, EmptyBlobIsRejected) { EXPECT_FALSE(validateSpirvBlob(std::span<const char>{})); }

TEST(ShaderBlobUnit, SizeNotAMultipleOfFourIsRejected)
{
    // Correct magic, but six bytes is not a multiple of 4.
    std::vector<char> blob = spirvMagicBytes();
    blob.push_back(0);
    blob.push_back(0);
    EXPECT_FALSE(validateSpirvBlob(blob));
}

TEST(ShaderBlobUnit, CorrectSizeWithWrongMagicIsRejected)
{
    const std::vector<char> blob(8, 0);// 8 bytes, all zero - wrong magic
    EXPECT_FALSE(validateSpirvBlob(blob));
}

TEST(ShaderBlobUnit, BlobWithCorrectMagicIsAccepted)
{
    const std::vector<char> blob = spirvMagicBytes();
    EXPECT_TRUE(validateSpirvBlob(blob));
}

// Tests run with the repo root as the working directory.
TEST(ShaderBlobUnit, RealCompiledShaderIsAccepted)
{
    const std::filesystem::path spv = "Resources/ShadersSlang/build/spirv/rasterizer/rasterizer.vs_main.spv";
    if (!std::filesystem::exists(spv)) {
        GTEST_SKIP() << spv.string() << " not found - run compile-slang-shaders before this test.";
    }

    std::ifstream file(spv, std::ios::binary | std::ios::ate);
    ASSERT_TRUE(file.is_open());
    const auto size = static_cast<std::size_t>(file.tellg());
    file.seekg(0);
    std::vector<char> bytes(size);
    file.read(bytes.data(), static_cast<std::streamsize>(size));

    EXPECT_TRUE(validateSpirvBlob(bytes));
}

// A copy would double-destroy the modules and a move would leave create-infos on handles it no longer owns.
TEST(ShaderBlobUnit, ShaderStagePairIsNeitherCopyableNorMovable)
{
    static_assert(!std::is_copy_constructible_v<ShaderStagePair>, "ShaderStagePair must not be copy-constructible");
    static_assert(!std::is_copy_assignable_v<ShaderStagePair>, "ShaderStagePair must not be copy-assignable");
    static_assert(!std::is_move_constructible_v<ShaderStagePair>, "ShaderStagePair must not be move-constructible");
    static_assert(!std::is_move_assignable_v<ShaderStagePair>, "ShaderStagePair must not be move-assignable");
}
