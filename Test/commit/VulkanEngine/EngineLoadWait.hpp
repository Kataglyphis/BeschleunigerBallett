#pragma once

#include <gtest/gtest.h>

namespace Kataglyphis::TestSupport {

/// Pumps frames until the worker-parsed startup model is installed; the frame cap fails a hung parse.
/// Renderer is a template parameter so this header stays import-free: gcc rejects a module import before vulkan.hpp.
template<typename Renderer, typename RenderOneFrame>
void waitForModelLoad(Renderer *renderer, RenderOneFrame renderOneFrame)
{
    constexpr int MAX_LOAD_FRAMES = 20000;

    int frames = 0;
    while (renderer->isModelLoadPending() && frames < MAX_LOAD_FRAMES) {
        renderOneFrame();
        ++frames;
        if (renderer->hasDeviceLost()) { return; }
    }

    EXPECT_FALSE(renderer->isModelLoadPending()) << "Model was still loading after " << MAX_LOAD_FRAMES << " frames.";
}

}// namespace Kataglyphis::TestSupport
