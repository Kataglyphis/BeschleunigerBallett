#pragma once

#include <vulkan/vulkan.hpp>

namespace Kataglyphis {

// Access mask implied by a layout, for either side of a transition.
constexpr vk::AccessFlags accessFlagsForImageLayout(vk::ImageLayout layout)
{
    switch (layout) {
    case vk::ImageLayout::ePreinitialized:
        return vk::AccessFlagBits::eHostWrite;
    case vk::ImageLayout::eTransferDstOptimal:
        return vk::AccessFlagBits::eTransferWrite;
    case vk::ImageLayout::eTransferSrcOptimal:
        return vk::AccessFlagBits::eTransferRead;
    case vk::ImageLayout::eColorAttachmentOptimal:
        return vk::AccessFlagBits::eColorAttachmentWrite;
    case vk::ImageLayout::eDepthStencilAttachmentOptimal:
        return vk::AccessFlagBits::eDepthStencilAttachmentWrite;
    case vk::ImageLayout::eShaderReadOnlyOptimal:
        return vk::AccessFlagBits::eShaderRead;
    case vk::ImageLayout::eGeneral:
        return vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite
               | vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eTransferRead
               | vk::AccessFlagBits::eTransferWrite;
    default:
        return vk::AccessFlags();
    }
}

// eAllCommands rather than fragment stages lets a transition be recorded on a non-graphics queue.
constexpr vk::PipelineStageFlags pipelineStageForLayout(vk::ImageLayout oldImageLayout)
{
    switch (oldImageLayout) {
    case vk::ImageLayout::eTransferDstOptimal:
    case vk::ImageLayout::eTransferSrcOptimal:
        return vk::PipelineStageFlagBits::eTransfer;
    case vk::ImageLayout::eColorAttachmentOptimal:
        return vk::PipelineStageFlagBits::eColorAttachmentOutput;
    case vk::ImageLayout::eDepthStencilAttachmentOptimal:
        return vk::PipelineStageFlagBits::eAllCommands;// We do this to allow queue
    case vk::ImageLayout::eShaderReadOnlyOptimal:
        return vk::PipelineStageFlagBits::eAllCommands;// We do this to allow queue
    case vk::ImageLayout::ePreinitialized:
        return vk::PipelineStageFlagBits::eHost;
    case vk::ImageLayout::eUndefined:
        return vk::PipelineStageFlagBits::eTopOfPipe;
    case vk::ImageLayout::eGeneral:
        return vk::PipelineStageFlagBits::eAllCommands;
    default:
        return vk::PipelineStageFlagBits::eBottomOfPipe;
    }
}

}// namespace Kataglyphis
