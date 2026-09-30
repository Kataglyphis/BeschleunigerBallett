// Mirrors GltfLoader's extractImageBytes on purpose: if the loader's guard regresses, the divergence is the signal.

// Must precede fuzztest.h, which friend-declares them unincluded; a force-include would break module BMI synthesis.
#include "absl/random/internal/distribution_caller.h"// IWYU pragma: keep
#include "absl/random/internal/mock_helpers.h"// IWYU pragma: keep

#include <cstddef>
#include <cstdlib>
#include <string>
#include <vector>

#include "fuzztest/fuzztest.h"

// This target links no engine, so it carries its own copy of the header-only implementation.
#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

namespace {

// The loader's two guards: the buffer view must fit its buffer, and base64 length must conform.
void ExtractImageBytesSafely(const cgltf_image *image)
{
    if (image == nullptr) { return; }

    if (image->buffer_view != nullptr && image->buffer_view->buffer != nullptr
        && image->buffer_view->buffer->data != nullptr) {
        const cgltf_size offset = image->buffer_view->offset;
        const cgltf_size size = image->buffer_view->size;
        const cgltf_size buffer_size = image->buffer_view->buffer->size;
        if (offset > buffer_size || size > buffer_size - offset) { return; }
        const auto *base = static_cast<const unsigned char *>(image->buffer_view->buffer->data);
        const std::vector<unsigned char> bytes(base + offset, base + offset + size);
        (void)bytes;
        return;
    }

    if (image->uri != nullptr) {
        const std::string uri = image->uri;
        const std::string marker = "base64,";
        const std::string::size_type pos = uri.find(marker);
        if (pos == std::string::npos) { return; }
        const cgltf_size b64len = uri.size() - pos - marker.size();
        if (b64len < 4 || (b64len % 4) != 0) { return; }
    }
}

void ParsingArbitraryGltfNeverCrashes(const std::vector<uint8_t> &bytes)
{
    if (bytes.empty()) { return; }

    cgltf_options options{};
    cgltf_data *data = nullptr;
    if (cgltf_parse(&options, bytes.data(), bytes.size(), &data) != cgltf_result_success) {
        return;// parse rejection is fine; crashing is not
    }

    // The engine never walks a document that fails cgltf_validate.
    if (cgltf_validate(data) != cgltf_result_success) {
        cgltf_free(data);
        return;
    }

    // In-memory data resolves only embedded buffers; a failed load is fine, but unloaded views must not be walked.
    const bool buffers_loaded =
      cgltf_load_buffers(&options, data, nullptr) == cgltf_result_success;

    for (cgltf_size i = 0; buffers_loaded && i < data->images_count; ++i) {
        ExtractImageBytesSafely(&data->images[i]);
    }

    cgltf_free(data);
}

}// namespace

// The smallest valid glTF gives the fuzzer a parseable starting point.
FUZZ_TEST(GltfParsing, ParsingArbitraryGltfNeverCrashes)
  .WithSeeds({ std::vector<uint8_t>{} });
