// The goldens' glTF controls once went to fixed names in the shared temp directory and stayed there.

#include "GltfVariant.hpp"
#include "RepoFiles.hpp"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

using Kataglyphis::TestSupport::GltfVariant;

namespace {

long long current_process_id()
{
#ifdef _WIN32
    return static_cast<long long>(_getpid());
#else
    return static_cast<long long>(getpid());
#endif
}

}// namespace

// Two suites on one host must not truncate each other's copy, and a finished test leaves nothing behind.
TEST(GltfVariantUnit, EachProcessWritesItsOwnCopyAndRemovesIt)
{
    const std::filesystem::path fixture =
      Kataglyphis::TestSupport::repoRoot() / "Resources" / "Models" / "GltfTest" / "metallic_card.gltf";
    ASSERT_TRUE(std::filesystem::exists(fixture)) << "missing " << fixture.string();

    std::filesystem::path written;
    {
        const std::optional<GltfVariant> variant = GltfVariant::create(fixture,
          "variant_suite_card.gltf",
          [](nlohmann::json &gltf) { gltf["materials"][0]["pbrMetallicRoughness"]["metallicFactor"] = 0.0; });
        ASSERT_TRUE(variant.has_value());
        written = variant->path();

        EXPECT_NE(written.filename().string().find(std::to_string(current_process_id())), std::string::npos)
          << written.string() << " is not private to this process";
        std::ifstream in(written);
        ASSERT_TRUE(in.is_open()) << "the variant was not written: " << written.string();
        std::stringstream text;
        text << in.rdbuf();
        const nlohmann::json gltf = nlohmann::json::parse(text.str(), nullptr, false);
        ASSERT_FALSE(gltf.is_discarded());
        EXPECT_EQ(gltf["materials"][0]["pbrMetallicRoughness"]["metallicFactor"].get<double>(), 0.0)
          << "the edit did not reach the copy";
    }
    EXPECT_FALSE(std::filesystem::exists(written)) << written.string() << " outlived its test";

    const auto no_edit = [](nlohmann::json &) {};
    EXPECT_FALSE(GltfVariant::create(fixture.parent_path() / "no_such_card.gltf", "unused.gltf", no_edit).has_value())
      << "an unreadable fixture must give no variant";
}
