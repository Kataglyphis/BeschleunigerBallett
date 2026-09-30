// Shader bytes reach VkShaderModule unvalidated, so the round trip must be byte-exact, not merely crash-free.

// Must precede fuzztest.h, which friend-declares them unincluded; a force-include would break module BMI synthesis.
#include "absl/random/internal/distribution_caller.h"// IWYU pragma: keep
#include "absl/random/internal/mock_helpers.h"// IWYU pragma: keep

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "fuzztest/fuzztest.h"

import kataglyphis.shared.util.file_reader;

namespace {

// The readers guard with fileExists, so the interesting inputs break path construction itself.
void ReadingArbitraryPathsNeverCrashes(const std::string &path)
{
    // Beyond this only the OS path limit is under test, not our logic.
    if (path.size() > 512) { return; }

    const bool exists = Kataglyphis::Shared::fileExists(path);
    const std::vector<char> bytes = Kataglyphis::Shared::readBinaryFile(path);
    const std::string text = Kataglyphis::Shared::readTextFile(path);
    const std::string base = Kataglyphis::Shared::getBaseDir(path);

    (void)exists;
    (void)bytes.size();
    (void)text.size();
    (void)base.size();
}

FUZZ_TEST(ShaderFileReaderFuzz, ReadingArbitraryPathsNeverCrashes)
  .WithSeeds({ std::string(""),
    std::string("Resources/ShadersSlang/build/spirv/rasterizer/rasterizer.fs_main.spv"),
    std::string("does/not/exist.spv"),
    std::string("../../../etc/passwd"),
    std::string("C:\\Windows\\System32\\config\\SAM"),
    std::string("\xff\xfe\x00 binary in a path"),
    std::string("con"),// reserved device name on Windows
    std::string(".") });

// Real SPIR-V holds NULs, 0x1a and CR/LF pairs, exactly what a text-mode read would damage.
void ReadingAFileReturnsItsBytesExactly(const std::vector<uint8_t> &contents)
{
    if (contents.size() > 1u << 16) { return; }

    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
    if (ec) { return; }
    const std::filesystem::path path = dir / "kataglyphis_shader_reader_fuzz.bin";

    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) { return; }
        if (!contents.empty()) {
            out.write(reinterpret_cast<const char *>(contents.data()), static_cast<std::streamsize>(contents.size()));
        }
        if (!out) { return; }
    }

    const std::vector<char> read_back = Kataglyphis::Shared::readBinaryFile(path.string());

    // Size first: a NUL truncation reads far clearer here than deep in the comparison.
    ASSERT_EQ(read_back.size(), contents.size())
      << "readCharSequence returned " << read_back.size() << " of " << contents.size()
      << " bytes - a shader read this way would be silently truncated";

    for (std::size_t i = 0; i < contents.size(); ++i) {
        ASSERT_EQ(static_cast<uint8_t>(read_back[i]), contents[i])
          << "byte " << i << " changed on read - binary content is being transformed";
    }

    std::filesystem::remove(path, ec);
}

FUZZ_TEST(ShaderFileReaderFuzz, ReadingAFileReturnsItsBytesExactly)
  .WithSeeds({ std::vector<uint8_t>{},
    // SPIR-V magic number, little endian - a real shader's first four bytes.
    std::vector<uint8_t>{ 0x03, 0x02, 0x23, 0x07 },
    std::vector<uint8_t>{ 0x00, 0x00, 0x00, 0x00 },
    std::vector<uint8_t>{ 'a', 0x00, 'b' },
    std::vector<uint8_t>{ 0x0d, 0x0a, 0x0d, 0x0a },// CRLF, mangled by text mode
    std::vector<uint8_t>{ 0x1a },// historic DOS EOF
    std::vector<uint8_t>{ 0xff, 0xfe, 0xef, 0xbb, 0xbf } });

}// namespace
