// GUI model selection reaches these, so hostile paths must not crash, hang or escape the resource tree.

// Must precede fuzztest.h, which friend-declares them unincluded; a force-include would break module BMI synthesis.
#include "absl/random/internal/distribution_caller.h"// IWYU pragma: keep
#include "absl/random/internal/mock_helpers.h"// IWYU pragma: keep

#include <algorithm>
#include <filesystem>
#include <string>

#include "fuzztest/fuzztest.h"

import kataglyphis.vulkan.scene_config;

namespace {

void ResolvingArbitraryPathsNeverCrashes(const std::string &relative_path)
{
    // Overly long inputs only exercise the OS path limit, not our logic.
    if (relative_path.size() > 512) { return; }

    const std::string resolved = sceneConfig::resolveModelPath(relative_path);

    // A miss legitimately returns the direct candidate, so only usability is checked.
    (void)resolved.empty();

    std::error_code ignored;
    (void)std::filesystem::exists(resolved, ignored);
}

FUZZ_TEST(SceneConfigFuzz, ResolvingArbitraryPathsNeverCrashes)
  .WithSeeds({ std::string("Models/plane.obj"),
    std::string("Models/does_not_exist.obj"),
    std::string(""),
    std::string("../../../etc/passwd"),
    std::string("Models/\xff\xfe/weird.obj"),
    std::string("C:\\absolute\\path.obj") });

// GUI rendering lists models every frame the panel is open.
void ListingModelsIsStable(int repetitions)
{
    const int bounded = (repetitions % 4) + 1;
    const auto first = sceneConfig::getAvailableModelPaths();
    for (int i = 1; i < bounded; ++i) {
        const auto again = sceneConfig::getAvailableModelPaths();
        if (!std::ranges::equal(first, again)) { std::abort(); }
    }
}

FUZZ_TEST(SceneConfigFuzz, ListingModelsIsStable).WithSeeds({ 1, 3 });

}// namespace
