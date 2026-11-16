#pragma once

#include <vulkan/vulkan.hpp>

namespace Kataglyphis {

// Access mask implied by a layout, for either side of a transition.
constexpr vk::AccessFlags2 accessFlagsForImageLayout(vk::ImageLayout layout)
{
    switch (layout) {
    case vk::ImageLayout::ePreinitialized:
        return vk::AccessFlagBits2::eHostWrite;
    case vk::ImageLayout::eTransferDstOptimal:
        return vk::AccessFlagBits2::eTransferWrite;
    case vk::ImageLayout::eTransferSrcOptimal:
        return vk::AccessFlagBits2::eTransferRead;
    case vk::ImageLayout::eColorAttachmentOptimal:
        return vk::AccessFlagBits2::eColorAttachmentWrite;
    case vk::ImageLayout::eDepthStencilAttachmentOptimal:
        return vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    case vk::ImageLayout::eShaderReadOnlyOptimal:
        return vk::AccessFlagBits2::eShaderRead;
    case vk::ImageLayout::eGeneral:
        return vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite
               | vk::AccessFlagBits2::eColorAttachmentWrite | vk::AccessFlagBits2::eTransferRead
               | vk::AccessFlagBits2::eTransferWrite;
    default:
        return vk::AccessFlags2();
    }
}

// eAllCommands rather than fragment stages lets a transition be recorded on a non-graphics queue.
constexpr vk::PipelineStageFlags2 pipelineStageForLayout(vk::ImageLayout oldImageLayout)
{
    switch (oldImageLayout) {
    case vk::ImageLayout::eTransferDstOptimal:
    case vk::ImageLayout::eTransferSrcOptimal:
        return vk::PipelineStageFlagBits2::eTransfer;
    case vk::ImageLayout::eColorAttachmentOptimal:
        return vk::PipelineStageFlagBits2::eColorAttachmentOutput;
    case vk::ImageLayout::eDepthStencilAttachmentOptimal:
        return vk::PipelineStageFlagBits2::eAllCommands;// We do this to allow queue
    case vk::ImageLayout::eShaderReadOnlyOptimal:
        return vk::PipelineStageFlagBits2::eAllCommands;// We do this to allow queue
    case vk::ImageLayout::ePreinitialized:
        return vk::PipelineStageFlagBits2::eHost;
    case vk::ImageLayout::eUndefined:
        return vk::PipelineStageFlagBits2::eTopOfPipe;
    case vk::ImageLayout::eGeneral:
        return vk::PipelineStageFlagBits2::eAllCommands;
    default:
        return vk::PipelineStageFlagBits2::eBottomOfPipe;
    }
}

}// namespace Kataglyphis
