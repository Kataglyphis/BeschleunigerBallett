// Callers memcpy image_size bytes, so a size that disagrees with width * height * 4 is a heap overflow.

// Must precede fuzztest.h, which friend-declares them unincluded; a force-include would break module BMI synthesis.
#include "absl/random/internal/distribution_caller.h"// IWYU pragma: keep
#include "absl/random/internal/mock_helpers.h"// IWYU pragma: keep

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

// This target links no engine, so it carries its own copy of the header-only implementation.
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include "fuzztest/fuzztest.h"

import kataglyphis.vulkan.texture_decode;

namespace {

void DecodingArbitraryImageBytesRespectsTheSizeContract(const std::vector<uint8_t> &contents)
{
    if (contents.size() > 1u << 18) { return; }

    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
    if (ec) { return; }
    const std::filesystem::path path = dir / "kataglyphis_texture_fuzz.img";

    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) { return; }
        if (!contents.empty()) {
            out.write(reinterpret_cast<const char *>(contents.data()), static_cast<std::streamsize>(contents.size()));
        }
        if (!out) { return; }
    }

    // Sentinels, not 0; callers may rely on these only on the success path.
    int width = -1;
    int height = -1;
    std::uint64_t image_size = 0;
    unsigned char *pixels = Kataglyphis::TextureDecode::decodeImageRGBA8(path.string(), &width, &height, &image_size);

    if (pixels != nullptr) {
        EXPECT_GT(width, 0) << "decode succeeded but reported a non-positive width";
        EXPECT_GT(height, 0) << "decode succeeded but reported a non-positive height";
        // Callers size the staging buffer from this; STBI_rgb_alpha forces 4 channels.
        EXPECT_EQ(image_size,
          static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) * 4U)
          << "reported byte count does not match the reported dimensions - a staging "
             "buffer sized from this would over- or under-run";
        stbi_image_free(pixels);
    }

    std::filesystem::remove(path, ec);
}

FUZZ_TEST(TextureLoadingFuzz, DecodingArbitraryImageBytesRespectsTheSizeContract)
  .WithSeeds({ std::vector<uint8_t>{},
    // PNG magic - enough to enter the PNG decoder and then fail.
    std::vector<uint8_t>{ 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a },
    // JPEG SOI.
    std::vector<uint8_t>{ 0xff, 0xd8, 0xff, 0xe0 },
    // BMP.
    std::vector<uint8_t>{ 'B', 'M' },
    // Absurd TGA dimensions are how a decoder is talked into reporting a size it did not allocate.
    std::vector<uint8_t>{ 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 32, 0 },
    std::vector<uint8_t>{ 'G', 'I', 'F', '8', '9', 'a' } });

}// namespace
