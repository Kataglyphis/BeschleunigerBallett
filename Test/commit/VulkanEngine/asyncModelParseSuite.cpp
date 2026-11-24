// The engine's only threaded class, on the one platform without ThreadSanitizer; device-free like the parse suites.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include "RepoFiles.hpp"

import kataglyphis.vulkan.async_model_parse;
import kataglyphis.vulkan.gltf_loader;
import kataglyphis.vulkan.scene_config;

namespace {

std::string test_model()
{
    // The purpose-built shadow rig: small, committed, and known-good.
    return sceneConfig::resolveModelPath("Models/ShadowTest/shadow_rig.obj");
}

std::string test_gltf() { return Kataglyphis::TestSupport::oxidantGltfFixture("cube.glb"); }

// Polls like a frame loop instead of joining: callers drive this by polling, not blocking.
void spinUntilFinished(Kataglyphis::AsyncModelParse &parse)
{
    for (int spin = 0; spin < 10000 && !parse.isFinished(); ++spin) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

}// namespace

TEST(AsyncModelParseUnit, IdleInstancesAreSafeToQueryAndTake)
{
    Kataglyphis::AsyncModelParse parse;
    EXPECT_FALSE(parse.isRunning());
    EXPECT_FALSE(parse.isFinished());
    EXPECT_EQ(parse.takeResult(), nullptr);
    parse.waitForCompletion();// must not deadlock on a never-started parse
}

TEST(AsyncModelParseUnit, ParsesOffThreadAndHandsBackTheResult)
{
    if (!std::filesystem::exists(test_model())) { GTEST_SKIP() << "test model not present"; }

    Kataglyphis::GltfLoader reference;
    ASSERT_TRUE(reference.parseCpu(test_model()));

    Kataglyphis::AsyncModelParse parse;
    parse.start(test_model());
    spinUntilFinished(parse);
    ASSERT_TRUE(parse.isFinished()) << "the parse never completed";
    ASSERT_TRUE(parse.wasSuccessful());

    const std::unique_ptr<Kataglyphis::GltfLoader> result = parse.takeResult();
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->getVertices().size(), reference.getVertices().size());
    EXPECT_EQ(result->getIndices().size(), reference.getIndices().size());
}

TEST(AsyncModelParseUnit, ParsesAGlbOffThreadThroughTheSameLoader)
{
    if (!std::filesystem::exists(test_gltf())) { GTEST_SKIP() << "test glb not present"; }

    Kataglyphis::GltfLoader reference;
    ASSERT_TRUE(reference.parseCpu(test_gltf()));

    Kataglyphis::AsyncModelParse parse;
    parse.start(test_gltf());
    spinUntilFinished(parse);
    ASSERT_TRUE(parse.isFinished()) << "the parse never completed";
    ASSERT_TRUE(parse.wasSuccessful());

    const std::unique_ptr<Kataglyphis::GltfLoader> result = parse.takeResult();
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->getVertices().size(), reference.getVertices().size());
    EXPECT_EQ(result->getIndices().size(), reference.getIndices().size());
}

// The worker parses with the options it started with: MikkTSpace splits the quad's two mirrored corners.
TEST(AsyncModelParseUnit, TheWorkerParsesWithTheLoadOptionsItStartedWith)
{
    const std::string quad = Kataglyphis::TestSupport::oxidantGltfFixture("mirrored_uv_quad.gltf");
    ASSERT_TRUE(std::filesystem::exists(quad)) << quad;

    Kataglyphis::AsyncModelParse parse;
    parse.start(quad, Kataglyphis::GltfLoadOptions{ .mikktspaceTangents = true });
    spinUntilFinished(parse);
    ASSERT_TRUE(parse.wasSuccessful());
    const std::unique_ptr<Kataglyphis::GltfLoader> result = parse.takeResult();
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->getVertices().size(), 6U) << "the worker dropped the MikkTSpace option";
}

TEST(AsyncModelParseUnit, AFailedParseReportsFailureRatherThanEmptyGeometry)
{
    Kataglyphis::AsyncModelParse parse;
    parse.start("no/such/model.obj");
    parse.waitForCompletion();

    EXPECT_TRUE(parse.isFinished()) << "the worker must always publish, even on failure";
    EXPECT_FALSE(parse.wasSuccessful());
    // Failed must differ from "loaded nothing", or a bad path silently empties the scene.
    EXPECT_EQ(parse.takeResult(), nullptr);
}

TEST(AsyncModelParseUnit, StartingASecondParseSupersedesTheFirst)
{
    if (!std::filesystem::exists(test_model())) { GTEST_SKIP() << "test model not present"; }

    // Newest wins when the GUI picks again mid-load; two workers must never write into one loader.
    Kataglyphis::AsyncModelParse parse;
    parse.start(test_model());
    parse.start(test_model());
    parse.waitForCompletion();

    ASSERT_TRUE(parse.wasSuccessful());
    EXPECT_FALSE(parse.isRunning()) << "no worker should be left joinable after the second start";
    const auto result = parse.takeResult();
    ASSERT_NE(result, nullptr);
    EXPECT_GT(result->getVertices().size(), 0U);
}

TEST(AsyncModelParseUnit, TakingAResultLeavesIsFinishedTrue)
{
    // Pinned: taking a result leaves isFinished() true, which Scene::pollModelLoad survives only via its own flag.
    if (!std::filesystem::exists(test_model())) { GTEST_SKIP() << "test model not present"; }

    Kataglyphis::AsyncModelParse parse;
    parse.start(test_model());
    parse.waitForCompletion();
    ASSERT_TRUE(parse.isFinished());

    const auto result = parse.takeResult();
    ASSERT_NE(result, nullptr);
    EXPECT_TRUE(parse.isFinished()) << "takeResult must not reset finished - see Scene::pollModelLoad's own gate";
    EXPECT_TRUE(parse.wasSuccessful());
}

TEST(AsyncModelParseUnit, DestructionJoinsRatherThanDetaches)
{
    if (!std::filesystem::exists(test_model())) { GTEST_SKIP() << "test model not present"; }

    // A detached worker writing into a destroyed loader is a silent use-after-free; ASan catches it here.
    {
        Kataglyphis::AsyncModelParse parse;
        parse.start(test_model());
        // Deliberately no wait: the destructor must handle it.
    }
    SUCCEED() << "destructor returned without a dangling worker";
}
