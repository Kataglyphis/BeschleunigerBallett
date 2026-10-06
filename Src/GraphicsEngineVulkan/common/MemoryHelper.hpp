#pragma once
#include <cstdint>

namespace Kataglyphis {
// aligned piece of memory appropiately and when necessary return bigger piece
constexpr uint32_t align_up(uint32_t memory, uint32_t alignment) { return (memory + alignment - 1) & ~(alignment - 1); }

// vkGetRayTracingShaderGroupHandlesKHR packs handles at handleSize, ignoring the alignment requirement.
constexpr uint32_t sbt_handle_source_offset(uint32_t groupIndex, uint32_t handleSize)
{ return groupIndex * handleSize; }

// SBT records start at shaderGroupHandleAlignment, which can exceed the packed handle size.
constexpr uint32_t sbt_record_offset(uint32_t recordIndex, uint32_t handleSizeAligned)
{ return recordIndex * handleSizeAligned; }
}// namespace Kataglyphis