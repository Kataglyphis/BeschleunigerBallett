#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "RepoFiles.hpp"

import kataglyphis.vulkan.scene;

using Kataglyphis::Scene;

TEST(SceneAsyncLoad, ReloadCancelsAPendingParse)
{
    Scene scene;

    scene.beginModelLoadAsync();
    ASSERT_TRUE(scene.isModelLoadPending());

    scene.cancelPendingModelLoad();
    EXPECT_FALSE(scene.isModelLoadPending());

    // Idempotent, like every other cleanUp() in this codebase.
    scene.cancelPendingModelLoad();
    EXPECT_FALSE(scene.isModelLoadPending());
}

namespace {
namespace fs = std::filesystem;

using Kataglyphis::TestSupport::readFileText;
using Kataglyphis::TestSupport::repoRoot;
}// namespace

// An uncancelled startup parse still lands via pollModelLoad, leaving both models in the scene.
TEST(BuildIntegrity, ReloadModelCancelsAPendingAsyncParse)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path scene_path = repo_root / "Src" / "GraphicsEngineVulkan" / "scene" / "Scene.cpp";
    const auto contentsOpt = readFileText(scene_path);
    ASSERT_TRUE(contentsOpt.has_value()) << "missing " << scene_path.string();
    const std::string &contents = *contentsOpt;

    const std::size_t signature_pos = contents.find("Scene::reloadModel(");
    ASSERT_NE(signature_pos, std::string::npos) << "Scene::reloadModel is no longer defined in " << scene_path.string();

    const std::size_t body_open = contents.find('{', signature_pos);
    ASSERT_NE(body_open, std::string::npos) << "could not locate the opening brace of Scene::reloadModel";

    int brace_depth = 0;
    std::size_t body_close = std::string::npos;
    for (std::size_t i = body_open; i < contents.size(); ++i) {
        if (contents[i] == '{') {
            ++brace_depth;
        } else if (contents[i] == '}') {
            --brace_depth;
            if (brace_depth == 0) {
                body_close = i;
                break;
            }
        }
    }
    ASSERT_NE(body_close, std::string::npos) << "could not brace-match the closing '}' of Scene::reloadModel";

    const std::string body = contents.substr(body_open, body_close - body_open + 1);
    EXPECT_TRUE(body.find("cancelPendingModelLoad") != std::string::npos)
      << "Scene::reloadModel no longer calls cancelPendingModelLoad() - a reload issued while "
         "beginModelLoadAsync() is still parsing would leave the worker's result to land later via "
         "pollModelLoad, leaving the scene holding both the reloaded model and the startup one";
}
