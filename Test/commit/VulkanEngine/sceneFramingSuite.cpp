// The owner rule for where the camera starts on a load, OxidANT's framing through the bridge, and the credit's source.

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <glm/geometric.hpp>
#include <glm/glm.hpp>
#include <optional>
#include <string>
#include <system_error>

import kataglyphis.vulkan.camera;
import kataglyphis.vulkan.gltf_loader;
import kataglyphis.vulkan.scene_framing;

namespace {

using Kataglyphis::CameraPose;
using Kataglyphis::FramingSource;
using Kataglyphis::SceneBounds;
using Kataglyphis::SceneCamera;
using Kataglyphis::SceneMetadata;

// Colosseum-gltf's bounds, an 8 km ground under a 190 m building, and its Kamera_Arena in the stands.
constexpr SceneBounds kColosseumBounds{ .valid = true,
    .min = glm::vec3(-4000.0F, -11.25F, -4000.0F),
    .max = glm::vec3(4000.0F, 58.6F, 4000.0F) };
// crytek-sponza's, about 40 m across.
constexpr SceneBounds kSponzaBounds{ .valid = true,
    .min = glm::vec3(-19.2F, -1.3F, -11.9F),
    .max = glm::vec3(18.0F, 15.4F, 11.2F) };

SceneCamera arenaCamera()
{
    return SceneCamera{ .name = "Kamera_Arena",
        .eye = glm::vec3(-58.0F, 34.0F, 36.0F),
        .forward = glm::normalize(glm::vec3(0.783F, -0.403F, -0.474F)),
        .yfovRadians = 0.7427F };
}

void expectSamePose(const CameraPose &actual, const CameraPose &expected)
{
    EXPECT_EQ(actual.eye, expected.eye);
    EXPECT_EQ(actual.yaw, expected.yaw);
    EXPECT_EQ(actual.pitch, expected.pitch);
    EXPECT_EQ(actual.fov, expected.fov);
    EXPECT_EQ(actual.nearPlane, expected.nearPlane);
    EXPECT_EQ(actual.farPlane, expected.farPlane);
    EXPECT_EQ(actual.movementSpeed, expected.movementSpeed);
    EXPECT_EQ(actual.shadowDistance, expected.shadowDistance);
}

// A scratch folder, removed with what the test wrote into it.
struct ScratchDir
{
    std::filesystem::path path;
    explicit ScratchDir(const char *name) : path(std::filesystem::temp_directory_path() / name)
    {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
        std::filesystem::create_directories(path);
    }
    ~ScratchDir()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    ScratchDir(const ScratchDir &) = delete;
    ScratchDir &operator=(const ScratchDir &) = delete;

    [[nodiscard]] std::string write(const char *file, const std::string &text) const
    {
        std::ofstream out(path / file, std::ios::binary);
        out << text;
        return (path / file).string();
    }
};

}// namespace

// The framing hands Camera a pose in its own convention, so the camera looks exactly where the glTF camera looks.
TEST(SceneFramingUnit, ASceneWithItsOwnCameraStartsAtItAndLooksItsWay)
{
    const Camera fixed_camera;
    const SceneMetadata metadata{ .cameras = { SceneCamera{ .name = "Kamera", .eye = glm::vec3(23.0F, 2.2F, 1.5F) },
                                    arenaCamera() },
        .activeCamera = 1,
        .credit = {},
        .bounds = kColosseumBounds };
    const Kataglyphis::SceneFraming framing = Kataglyphis::frameScene(metadata, fixed_camera.fixed_pose());
    ASSERT_TRUE(framing.framed);
    EXPECT_EQ(framing.source, FramingSource::ActiveCamera);

    Camera camera;
    camera.apply_pose(framing.pose);
    const glm::vec3 eye = camera.get_camera_position();
    EXPECT_NEAR(glm::distance(eye, arenaCamera().eye), 0.0F, 1e-3F);
    EXPECT_NEAR(glm::dot(camera.get_camera_direction(), arenaCamera().forward), 1.0F, 1e-5F);
    EXPECT_NEAR(camera.get_fov(), glm::degrees(0.7427F), 1e-3F);
    // The far plane reaches the scene's farthest corner, and the near plane keeps 32-bit depth precision.
    EXPECT_GT(camera.get_far_plane(), 8000.0F);
    EXPECT_LE(camera.get_far_plane() / camera.get_near_plane(), 50000.0F * 1.001F);
    EXPECT_GT(framing.pose.shadowDistance, 120.0F) << "twice the view distance to the arena";
    EXPECT_GT(framing.pose.movementSpeed, 10.0F);

    // Without an active camera the first one frames the scene.
    SceneMetadata first = metadata;
    first.activeCamera = -1;
    const Kataglyphis::SceneFraming at_first = Kataglyphis::frameScene(first, fixed_camera.fixed_pose());
    EXPECT_EQ(at_first.source, FramingSource::FirstCamera);
    EXPECT_NEAR(glm::distance(at_first.pose.eye, glm::vec3(23.0F, 2.2F, 1.5F)), 0.0F, 1e-3F);
}

// Owner rule: a scene without a camera that the fixed pose sees keeps the pose, its planes and its speed.
TEST(SceneFramingUnit, ASceneTheFixedPoseSeesKeepsThePose)
{
    const Camera camera;
    for (const SceneBounds &bounds : { kSponzaBounds, SceneBounds{} }) {
        const SceneMetadata metadata{ .cameras = {}, .activeCamera = -1, .credit = {}, .bounds = bounds };
        const Kataglyphis::SceneFraming framing = Kataglyphis::frameScene(metadata, camera.fixed_pose());
        EXPECT_FALSE(framing.framed);
        EXPECT_EQ(framing.source, FramingSource::FixedPose);
        expectSamePose(framing.pose, camera.fixed_pose());
    }
}

// Owner rule: a scene the fixed pose would need a far plane past 50 000 near planes for is fitted to its bounds.
TEST(SceneFramingUnit, AnEightKilometreSceneIsFittedToItsBoundsFromTheFixedPosesDirection)
{
    const Camera camera;
    const CameraPose &fixed = camera.fixed_pose();
    const SceneMetadata metadata{ .cameras = {}, .activeCamera = -1, .credit = {}, .bounds = kColosseumBounds };
    const Kataglyphis::SceneFraming framing = Kataglyphis::frameScene(metadata, fixed);
    ASSERT_TRUE(framing.framed);
    EXPECT_EQ(framing.source, FramingSource::Bounds);
    Camera fitted;
    fitted.apply_pose(framing.pose);
    EXPECT_NEAR(glm::dot(fitted.get_camera_direction(), camera.get_camera_direction()), 1.0F, 1e-5F);
    EXPECT_EQ(framing.pose.fov, fixed.fov);
    const glm::vec3 centre = (kColosseumBounds.min + kColosseumBounds.max) * 0.5F;
    EXPECT_GT(glm::distance(framing.pose.eye, centre), 5000.0F) << "far enough back to see every corner";
    EXPECT_LE(framing.pose.farPlane / framing.pose.nearPlane, 50000.0F * 1.001F);
}

// A load moves the camera only when it is framed, or back to the fixed pose after a framed one.
TEST(SceneFramingUnit, TheStartPoseIsTheFramingOrTheFixedPoseAfterAFramedScene)
{
    const CameraPose fixed{ .eye = glm::vec3(0.0F, 2.0F, 0.0F) };
    const Kataglyphis::SceneFraming kept{ .framed = false, .source = FramingSource::FixedPose, .pose = fixed };
    CameraPose arena = fixed;
    arena.eye = arenaCamera().eye;
    const Kataglyphis::SceneFraming framed{ .framed = true, .source = FramingSource::ActiveCamera, .pose = arena };

    const std::optional<CameraPose> at_arena = Kataglyphis::startPose(framed, fixed, false);
    ASSERT_TRUE(at_arena.has_value());
    EXPECT_EQ(at_arena->eye, arena.eye);
    EXPECT_FALSE(Kataglyphis::startPose(kept, fixed, false).has_value()) << "the camera stays where the user flew";
    const std::optional<CameraPose> back = Kataglyphis::startPose(kept, fixed, true);
    ASSERT_TRUE(back.has_value()) << "a scene after a framed one gets the fixed pose and planes back";
    EXPECT_EQ(back->eye, fixed.eye);
}

// The glTF's cameras, its active one, its copyright and its box reach the loader's metadata.
TEST(SceneFramingUnit, AGltfsCamerasCreditAndBoundsReachTheLoader)
{
    const ScratchDir dir("kat_scene_framing_gltf");
    const std::string path = dir.write("camera.gltf", R"GLTF({
      "asset": { "version": "2.0", "copyright": "Triangle by Somebody, CC BY 4.0" },
      "scene": 0,
      "scenes": [ { "nodes": [ 0, 1 ], "extras": { "active_camera": "Arena" } } ],
      "nodes": [ { "mesh": 0 }, { "name": "Arena", "camera": 0, "translation": [0, 1, 5] } ],
      "cameras": [ { "type": "perspective", "perspective": { "yfov": 0.6, "znear": 0.1 } } ],
      "meshes": [ { "primitives": [ { "attributes": { "POSITION": 0 } } ] } ],
      "accessors": [ { "componentType": 5126, "count": 3, "type": "VEC3",
                       "min": [0,0,0], "max": [1,1,0], "bufferView": 0 } ],
      "bufferViews": [ { "buffer": 0, "byteLength": 36 } ],
      "buffers": [ { "byteLength": 36,
        "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA" } ]
    })GLTF");

    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(path));
    const SceneMetadata &metadata = loader.getMetadata();
    ASSERT_EQ(metadata.cameras.size(), 1U);
    EXPECT_EQ(metadata.cameras[0].name, "Arena");
    EXPECT_EQ(metadata.cameras[0].eye, glm::vec3(0.0F, 1.0F, 5.0F));
    EXPECT_EQ(metadata.cameras[0].forward, glm::vec3(0.0F, 0.0F, -1.0F));
    EXPECT_FLOAT_EQ(metadata.cameras[0].yfovRadians, 0.6F);
    EXPECT_EQ(metadata.activeCamera, 0);
    EXPECT_EQ(metadata.credit, "Triangle by Somebody, CC BY 4.0");
    EXPECT_TRUE(metadata.bounds.valid);
    EXPECT_EQ(metadata.bounds.min, glm::vec3(0.0F));
    EXPECT_EQ(metadata.bounds.max, glm::vec3(1.0F, 1.0F, 0.0F));

    // The uploader takes the parse's metadata with the rest, and the worker's loader reads as unparsed after.
    Kataglyphis::GltfLoader uploader;
    uploader.adoptParsed(std::move(loader));
    EXPECT_EQ(uploader.getMetadata().credit, "Triangle by Somebody, CC BY 4.0");
    EXPECT_TRUE(loader.getMetadata().cameras.empty());
}

// An OBJ has no copyright field: AssetANT's ATTRIBUTION.txt beside it credits it, as it does crytek-sponza.
TEST(SceneFramingUnit, AnObjIsCreditedByTheAttributionBesideIt)
{
    const ScratchDir dir("kat_scene_framing_obj");
    const std::string path = dir.write("triangle.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    {
        Kataglyphis::GltfLoader loader;
        ASSERT_TRUE(loader.parseCpu(path));
        EXPECT_TRUE(loader.getMetadata().credit.empty()) << "no attribution, no credit";
    }
    (void)dir.write(
      "ATTRIBUTION.txt", "triangle: one triangle\n\nLicense:     CC BY 3.0\nAttribution: (c) 2010 Somebody\n");
    Kataglyphis::GltfLoader loader;
    ASSERT_TRUE(loader.parseCpu(path));
    EXPECT_EQ(loader.getMetadata().credit, "(c) 2010 Somebody, CC BY 3.0");
    EXPECT_TRUE(loader.getMetadata().cameras.empty());
    EXPECT_EQ(loader.getMetadata().activeCamera, -1);
}
