#include <gtest/gtest.h>
#include <glm/geometric.hpp>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE

#include <array>
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "RepoFiles.hpp"

import kataglyphis.vulkan.camera;
import kataglyphis.vulkan.scene_config;
import kataglyphis.vulkan.model_file_kind;

namespace {

constexpr float kEpsilon = 1e-4F;

void expect_vec3_near(const glm::vec3 &actual, const glm::vec3 &expected, float epsilon = kEpsilon)
{
    EXPECT_NEAR(actual.x, expected.x, epsilon);
    EXPECT_NEAR(actual.y, expected.y, epsilon);
    EXPECT_NEAR(actual.z, expected.z, epsilon);
}

} // namespace

TEST(CameraUnit, DefaultStateLooksDownNegativeZ)
{
    Camera camera;

    // Default position and pitch are framing choices, so only the orientation contract is asserted.
    const glm::vec3 front = camera.get_camera_direction();
    const glm::vec3 right = camera.get_right_axis();
    const glm::vec3 up = camera.get_up_axis();

    EXPECT_NEAR(glm::length(front), 1.0F, kEpsilon);
    EXPECT_LT(front.z, -0.9F) << "yaw -90 must look predominantly down -Z";
    EXPECT_NEAR(front.x, 0.0F, kEpsilon);

    // Right stays the world +X for yaw -90 whatever the pitch is.
    expect_vec3_near(right, glm::vec3(1.0F, 0.0F, 0.0F));

    // The basis must stay orthonormal.
    EXPECT_NEAR(glm::dot(front, right), 0.0F, kEpsilon);
    EXPECT_NEAR(glm::dot(front, up), 0.0F, kEpsilon);
    EXPECT_NEAR(glm::dot(right, up), 0.0F, kEpsilon);
    EXPECT_NEAR(glm::length(up), 1.0F, kEpsilon);
    EXPECT_GT(up.y, 0.0F) << "up must not be flipped";

    EXPECT_NEAR(camera.get_yaw(), -90.0F, kEpsilon);
    EXPECT_GT(camera.get_far_plane(), camera.get_near_plane());
    EXPECT_GT(camera.get_fov(), 0.0F);
}

TEST(CameraUnit, AxesStayOrthonormalAfterMouseInput)
{
    Camera camera;
    camera.mouse_control(123.0F, -37.0F);

    const glm::vec3 front = camera.get_camera_direction();
    const glm::vec3 right = camera.get_right_axis();
    const glm::vec3 up = camera.get_up_axis();

    EXPECT_NEAR(glm::length(front), 1.0F, kEpsilon);
    EXPECT_NEAR(glm::length(right), 1.0F, kEpsilon);
    EXPECT_NEAR(glm::length(up), 1.0F, kEpsilon);
    EXPECT_NEAR(glm::dot(front, right), 0.0F, kEpsilon);
    EXPECT_NEAR(glm::dot(front, up), 0.0F, kEpsilon);
    EXPECT_NEAR(glm::dot(right, up), 0.0F, kEpsilon);
}

TEST(CameraUnit, MouseYawUsesTurnSpeed)
{
    Camera camera;
    // turn_speed is 0.25 -> 40 px of x movement adds 10 degrees of yaw
    camera.mouse_control(40.0F, 0.0F);
    EXPECT_NEAR(camera.get_yaw(), -80.0F, kEpsilon);
}

TEST(CameraUnit, PitchIsClampedAt89Degrees)
{
    Camera camera;
    camera.mouse_control(0.0F, 100000.0F);

    // pitch clamps at 89 degrees -> front.y == sin(89 deg)
    const float expected_y = std::sin(glm::radians(89.0F));
    EXPECT_NEAR(camera.get_camera_direction().y, expected_y, kEpsilon);

    camera.mouse_control(0.0F, -200000.0F);
    EXPECT_NEAR(camera.get_camera_direction().y, -expected_y, kEpsilon);
}

TEST(CameraUnit, KeyControlMovesAlongFront)
{
    Camera camera;
    std::array<bool, GLFW_KEY_LAST + 1> keys{};

    // Relative to the camera's own start and front, so retuning the default framing cannot break this.
    const glm::vec3 start = camera.get_camera_position();
    const glm::vec3 front = camera.get_camera_direction();
    constexpr float kStep = 10.0F * 0.5F;// movement_speed * dt

    keys[GLFW_KEY_W] = true;
    camera.key_control(keys, 0.5F);
    expect_vec3_near(camera.get_camera_position(), start + front * kStep);

    keys[GLFW_KEY_W] = false;
    keys[GLFW_KEY_S] = true;
    camera.key_control(keys, 0.5F);
    expect_vec3_near(camera.get_camera_position(), start);
}

TEST(CameraUnit, ViewMatrixMatchesLookAt)
{
    Camera camera;
    camera.set_camera_position(glm::vec3(3.0F, 4.0F, 5.0F));

    const glm::mat4 view = camera.calculate_viewmatrix();

    // The camera position must map to the origin in view space.
    const glm::vec4 origin = view * glm::vec4(3.0F, 4.0F, 5.0F, 1.0F);
    EXPECT_NEAR(origin.x, 0.0F, kEpsilon);
    EXPECT_NEAR(origin.y, 0.0F, kEpsilon);
    EXPECT_NEAR(origin.z, 0.0F, kEpsilon);

    const glm::mat4 expected =
      glm::lookAt(glm::vec3(3.0F, 4.0F, 5.0F), glm::vec3(3.0F, 4.0F, 5.0F) + camera.get_camera_direction(),
        glm::vec3(0.0F, 1.0F, 0.0F));
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) { EXPECT_NEAR(view[col][row], expected[col][row], kEpsilon); }
    }
}

TEST(CameraUnit, SettersAreReflectedInGetters)
{
    Camera camera;
    camera.set_near_plane(0.5F);
    camera.set_far_plane(123.0F);
    camera.set_fov(60.0F);

    EXPECT_NEAR(camera.get_near_plane(), 0.5F, kEpsilon);
    EXPECT_NEAR(camera.get_far_plane(), 123.0F, kEpsilon);
    EXPECT_NEAR(camera.get_fov(), 60.0F, kEpsilon);
}

TEST(SceneConfigUnit, DefaultModelFileExists)
{
    const std::string model_file = sceneConfig::getModelFile();
    EXPECT_FALSE(model_file.empty());
    EXPECT_TRUE(std::filesystem::exists(model_file)) << "Resolved model file does not exist: " << model_file;
}

TEST(SceneConfigUnit, ResolveModelPathFindsBundledModel)
{
    const std::string resolved = sceneConfig::resolveModelPath("Models/VikingRoom/viking_room.obj");
    EXPECT_TRUE(std::filesystem::exists(resolved)) << "Could not resolve bundled model: " << resolved;
}

TEST(SceneConfigUnit, ResolveModelPathIsIdempotent)
{
    const std::string resolved = sceneConfig::resolveModelPath("Models/GltfTest/cube.glb");
    EXPECT_TRUE(std::filesystem::exists(resolved)) << "Could not resolve bundled model: " << resolved;

    const std::string resolved_again = sceneConfig::resolveModelPath(resolved);
    EXPECT_EQ(resolved, resolved_again)
      << "Resolving an already-resolved path must return it unchanged - reloadModel() relies on this.";
}

TEST(SceneConfigUnit, AvailableModelListingsAreConsistent)
{
    const auto paths = sceneConfig::getAvailableModelPaths();
    const auto names = sceneConfig::getAvailableModelDisplayNames();

    ASSERT_EQ(paths.size(), names.size());
    ASSERT_FALSE(paths.empty()) << "Model scan found no loadable files under Resources/Models";

    for (const std::string &path : paths) {
        // The loadable set is OBJ plus glTF.
        auto lower = path;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        EXPECT_TRUE(lower.ends_with(".obj") || lower.ends_with(".gltf") || lower.ends_with(".glb"))
          << "Non-loadable entry in model list: " << path;
        EXPECT_TRUE(std::filesystem::exists(sceneConfig::resolveModelPath(path)))
          << "Listed model cannot be resolved: " << path;
    }
    for (const std::string &name : names) { EXPECT_FALSE(name.empty()); }
}

TEST(SceneConfigUnit, ModelExtensionDispatchIsCaseInsensitive)
{
    EXPECT_TRUE(Kataglyphis::isSupportedModelPath("a.OBJ"));
    EXPECT_TRUE(Kataglyphis::isSupportedModelPath("a.GlTF"));
    EXPECT_TRUE(Kataglyphis::isSupportedModelPath("a.GLB"));
    EXPECT_FALSE(Kataglyphis::isSupportedModelPath("a.png"));
    EXPECT_FALSE(Kataglyphis::isSupportedModelPath("a"));
    EXPECT_FALSE(Kataglyphis::isSupportedModelPath(""));

    EXPECT_TRUE(Kataglyphis::isGltfModelPath("a.gltf"));
    EXPECT_TRUE(Kataglyphis::isGltfModelPath("a.GLTF"));
    EXPECT_TRUE(Kataglyphis::isGltfModelPath("a.glb"));
    EXPECT_TRUE(Kataglyphis::isGltfModelPath("a.GLB"));
    EXPECT_FALSE(Kataglyphis::isGltfModelPath("a.obj"));
}

TEST(SceneConfigUnit, ModelExtensionDispatchUsesTheExtensionNotTheWholePath)
{
    EXPECT_FALSE(Kataglyphis::isGltfModelPath("C:/assets.glb/model.obj"));
}

TEST(SceneConfigUnit, ModelMatrixIsUniformPositiveScale)
{
    const glm::mat4 model = sceneConfig::getModelMatrix();

    EXPECT_GT(model[0][0], 0.0F);
    EXPECT_NEAR(model[0][0], model[1][1], kEpsilon);
    EXPECT_NEAR(model[1][1], model[2][2], kEpsilon);
    EXPECT_NEAR(model[3][3], 1.0F, kEpsilon);

    // Off-diagonal entries of the upper-left 3x3 must be zero (pure scale).
    for (int col = 0; col < 3; ++col) {
        for (int row = 0; row < 3; ++row) {
            if (col != row) { EXPECT_NEAR(model[col][row], 0.0F, kEpsilon); }
        }
    }
}

TEST(SceneConfigUnit, DefaultModelRelativePathIsAmongTheModelsGetModelFileCanResolve)
{
    // If these drift, the GUI's default selection points at a path getModelFile() never loads.
    const std::string_view relative_path = sceneConfig::defaultModelRelativePath();
    EXPECT_FALSE(relative_path.empty());
    EXPECT_TRUE(std::filesystem::exists(sceneConfig::resolveModelPath(std::string(relative_path))))
      << "defaultModelRelativePath() does not resolve to a bundled model: " << relative_path;
}

TEST(ModelPickerUnit, DefaultSelectedModelIndexPrefersTheStartupModelAndFallsBackToTheFirst)
{
    const std::vector<std::string> models_with_preferred{
        "Models/Dinosaurs/dinosaurs.obj", "Models/VikingRoom/viking_room.obj"};
    EXPECT_EQ(sceneConfig::defaultSelectedModelIndex(models_with_preferred, "Models/Dinosaurs/dinosaurs.obj"), 0);
    EXPECT_EQ(sceneConfig::defaultSelectedModelIndex(models_with_preferred, "Models/VikingRoom/viking_room.obj"), 1);

    // Without the preferred path, fall back to 0: -1 leaves the model transform controls dead.
    const std::vector<std::string> models_without_preferred{"Models/VikingRoom/viking_room.obj"};
    const int fallback_index =
      sceneConfig::defaultSelectedModelIndex(models_without_preferred, "Models/Dinosaurs/dinosaurs.obj");
    EXPECT_EQ(fallback_index, 0) << "must fall back to 0, not -1 - -1 disables the transform controls";

    const std::vector<std::string> empty_models;
    EXPECT_EQ(sceneConfig::defaultSelectedModelIndex(empty_models, "Models/Dinosaurs/dinosaurs.obj"), -1);

    // Backslashed Windows spelling must still match the forward-slashed preferred path.
    const std::vector<std::string> models_with_backslash{"Models\\Dinosaurs\\dinosaurs.obj"};
    EXPECT_EQ(
      sceneConfig::defaultSelectedModelIndex(models_with_backslash, "Models/Dinosaurs/dinosaurs.obj"), 0);
}

TEST(SceneConfigUnit, ScanningARegularFileReportsAnIncompleteWalk)
{
    const std::filesystem::path repo_root = Kataglyphis::TestSupport::repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "Could not locate repo root from the test working directory";

    const auto result = sceneConfig::scanModelsUnder(repo_root / "AGENTS.md");
    EXPECT_FALSE(result.complete);
    EXPECT_TRUE(result.paths.empty());
    EXPECT_TRUE(result.displayNames.empty());
}

TEST(SceneConfigUnit, ScanningAMissingDirectoryReportsAnIncompleteWalk)
{
    const std::filesystem::path repo_root = Kataglyphis::TestSupport::repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "Could not locate repo root from the test working directory";

    const auto result = sceneConfig::scanModelsUnder(repo_root / "NoSuchDirectory");
    EXPECT_FALSE(result.complete);
    EXPECT_TRUE(result.paths.empty());
    EXPECT_TRUE(result.displayNames.empty());
}

TEST(SceneConfigUnit, ScanningTheRepoResourcesFindsTheDefaultDebugModel)
{
    const std::filesystem::path repo_root = Kataglyphis::TestSupport::repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "Could not locate repo root from the test working directory";

    if (!std::filesystem::exists(repo_root / "Resources" / "Models")) {
        GTEST_SKIP() << "Resources/Models is absent";
    }

    const auto result = sceneConfig::scanModelsUnder(repo_root / "Resources");
    EXPECT_TRUE(result.complete);
    ASSERT_EQ(result.paths.size(), result.displayNames.size());
    EXPECT_GE(sceneConfig::defaultSelectedModelIndex(result.paths, sceneConfig::defaultModelRelativePath()), 0);
}

TEST(CameraSceneConfigUnit, ModelMatrixIsIdentityInEveryConfiguration)
{
    // Identity in every build configuration, asserted directly so neither can regress silently.
    const glm::mat4 model = sceneConfig::getModelMatrix();
    const glm::mat4 identity(1.0F);

    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) { EXPECT_FLOAT_EQ(model[col][row], identity[col][row]); }
    }
}
