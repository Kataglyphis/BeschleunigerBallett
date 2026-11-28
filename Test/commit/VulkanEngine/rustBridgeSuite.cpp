// OxidANT's kataglyphis_cxx_bridge, which every build links: calls that cross into Rust and come back as values.

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

#include "RepoFiles.hpp"
#include "ScopedLogCapture.hpp"
#include "oxidant_bridge/diagnostics.h"

import kataglyphis.vulkan.gltf_loader;
import kataglyphis.vulkan.oxidant_diagnostics;

namespace {

using Kataglyphis::TestSupport::CapturedRecord;
using Kataglyphis::TestSupport::ScopedLogCapture;

// Three vertices and two triangles, the second naming vertex 7: OxidANT drops it with a warning.
constexpr const char *kOutOfRangeIndexGltf = R"GLTF({
  "asset": { "version": "2.0" },
  "meshes": [ { "primitives": [ { "attributes": { "POSITION": 0 }, "indices": 1 } ] } ],
  "nodes": [ { "mesh": 0 } ],
  "scenes": [ { "nodes": [ 0 ] } ],
  "accessors": [
    { "componentType": 5126, "count": 3, "type": "VEC3", "min": [0,0,0], "max": [1,1,0], "bufferView": 0 },
    { "componentType": 5123, "count": 6, "type": "SCALAR", "bufferView": 1 }
  ],
  "bufferViews": [
    { "buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962 },
    { "buffer": 0, "byteOffset": 36, "byteLength": 12, "target": 34963 }
  ],
  "buffers": [ { "byteLength": 48,
    "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIABwABAAIA" } ]
})GLTF";

std::filesystem::path writeOutOfRangeIndexGltf(const char *name)
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    std::ofstream out(path, std::ios::binary);
    out << kOutOfRangeIndexGltf;
    return path;
}

// [workspace.package]'s version in OxidANT's Cargo.toml, empty when the checkout is not beside the suite.
std::string workspaceVersion()
{
    const auto text = Kataglyphis::TestSupport::readFileText(
      Kataglyphis::TestSupport::repoRoot() / "third_party" / "OxidANT" / "Cargo.toml");
    if (!text) { return {}; }
    static const std::regex kVersion(R"(\[workspace\.package\][^\[]*?\nversion\s*=\s*"([^"]+)\")");
    std::smatch match;
    return std::regex_search(*text, match, kVersion) ? match[1].str() : std::string{};
}

bool loggedTheDroppedTriangle(const std::vector<CapturedRecord> &records)
{
    return std::any_of(records.begin(), records.end(), [](const CapturedRecord &record) {
        return record.level == spdlog::level::warn
               && record.text.find("kataglyphis_assets::gltf_loader") != std::string::npos
               && record.text.find("dropped 1 triangle") != std::string::npos;
    });
}

}// namespace

TEST(RustBridgeUnit, ReportsTheOxidantVersionItWasBuiltFrom)
{
    const std::string version = Kataglyphis::oxidantVersion();
    EXPECT_TRUE(std::regex_match(version, std::regex(R"(\d+\.\d+\.\d+([-+].*)?)")))
      << "OxidANT reported '" << version << "'";
    const std::string expected = workspaceVersion();
    if (!expected.empty()) { EXPECT_EQ(version, expected) << "not the version of the pinned OxidANT's Cargo.toml"; }
}

TEST(RustBridgeUnit, ALoaderWarningReachesSpdlogAndLeavesNothingBehind)
{
    const std::filesystem::path path = writeOutOfRangeIndexGltf("kat_bridge_log_oob.gltf");
    ScopedLogCapture capture;
    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(path.string()));
    std::filesystem::remove(path);

    EXPECT_TRUE(loggedTheDroppedTriangle(capture.records()))
      << "the loader's dropped-triangle warning did not reach spdlog as a warning naming its module";
    EXPECT_TRUE(kataglyphis::oxidant::diagnostics::take_log_records().empty())
      << "parseCpu left this thread's records for the next call to log";
}

// The records wait on the thread that logged them, so a parse worker logs its own and drains no other's.
TEST(RustBridgeUnit, AWorkerThreadLogsItsOwnLoaderWarnings)
{
    const std::filesystem::path path = writeOutOfRangeIndexGltf("kat_bridge_log_worker.gltf");
    ScopedLogCapture capture;
    bool parsed = false;
    std::thread worker([&] {
        Kataglyphis::GltfLoader loader;
        parsed = loader.parseCpu(path.string());
    });
    worker.join();
    std::filesystem::remove(path);

    ASSERT_TRUE(parsed);
    EXPECT_TRUE(loggedTheDroppedTriangle(capture.records())) << "the worker's warning never reached spdlog";
    EXPECT_TRUE(kataglyphis::oxidant::diagnostics::take_log_records().empty())
      << "this thread holds records another thread's call logged";
}
