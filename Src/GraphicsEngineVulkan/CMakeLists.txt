# update current positions
set(PROJECT_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/)
set(PROJECT_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/)

set(_KATAGLYPHIS_SOURCE_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/
    ${CMAKE_CURRENT_SOURCE_DIR}/common/
    ${CMAKE_CURRENT_SOURCE_DIR}/renderer/
    ${CMAKE_CURRENT_SOURCE_DIR}/renderer/pushConstants/
    ${CMAKE_CURRENT_SOURCE_DIR}/scene/)

include(KataglyphisCMakeHelpers)
include(cmake/filters/SetProjectFilters.cmake)
include(cmake/SetSourceGroups.cmake)

file(GLOB_RECURSE VULKANRENDERER_SOURCES "*.cpp")

# Separate Main.cpp from library sources
set(VULKANRENDERER_MAIN_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/Main.cpp")
list(REMOVE_ITEM VULKANRENDERER_SOURCES "${VULKANRENDERER_MAIN_SOURCE}")

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 15)
  set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/renderer/VulkanRenderer.cpp PROPERTIES COMPILE_OPTIONS
                                                                                                 "-fno-lto")
endif()

file(GLOB_RECURSE VULKANRENDERER_HEADERS "*.hpp")

kataglyphis_collect_module_interfaces(VULKAN_MODULE_INTERFACE_FILES "${CMAKE_CURRENT_SOURCE_DIR}")
kataglyphis_collect_module_interfaces(SHARED_MODULE_INTERFACE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/../shared")

# Generate config module file (shared between library and executable)
set(VULKAN_RENDERER_CONFIG_MODULE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(VULKAN_RENDERER_CONFIG_MODULE_FILE "${VULKAN_RENDERER_CONFIG_MODULE_DIR}/VulkanRendererConfig.ixx")
file(MAKE_DIRECTORY "${VULKAN_RENDERER_CONFIG_MODULE_DIR}")
configure_file(VulkanRendererConfig.ixx.in "${VULKAN_RENDERER_CONFIG_MODULE_FILE}" @ONLY)
set_source_files_properties("${VULKAN_RENDERER_CONFIG_MODULE_FILE}" PROPERTIES GENERATED TRUE)
list(APPEND VULKAN_MODULE_INTERFACE_FILES "${VULKAN_RENDERER_CONFIG_MODULE_FILE}")

# VulkanEngineCore: STATIC, which propagates C++20 modules better than OBJECT
add_library(VulkanEngineCore STATIC)

kataglyphis_set_module_scan(VulkanEngineCore)

# PUBLIC: every translation unit doing projection math needs Vulkan's clip conventions.
target_compile_definitions(VulkanEngineCore PUBLIC GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_FORCE_RADIANS)

if(RUST_FEATURES)
  target_compile_definitions(VulkanEngineCore PUBLIC USE_RUST=1)
else()
  target_compile_definitions(VulkanEngineCore PUBLIC USE_RUST=0)
endif()

kataglyphis_apply_runtime_compile_definitions(
  VulkanEngineCore
  RESOURCE_PATH_NON_MSVC
  "/../Resources/"
  RESOURCE_PATH_MSVC
  "/../../Resources/"
  INCLUDE_PATH_NON_MSVC
  "/../Src/GraphicsEngineVulkan/"
  INCLUDE_PATH_MSVC
  "/../../Src/GraphicsEngineVulkan/"
  IMGUI_FONTS_PATH_NON_MSVC
  "/../third_party/IMGUI/misc/fonts/"
  IMGUI_FONTS_PATH_MSVC
  "/../../third_party/IMGUI/misc/fonts/")

target_sources(
  VulkanEngineCore
  PRIVATE ${VULKANRENDERER_SOURCES}
  PUBLIC FILE_SET
         HEADERS
         BASE_DIRS
         ${PROJECT_INCLUDE_DIR}
         FILES
         ${VULKANRENDERER_HEADERS})

target_sources(
  VulkanEngineCore
  PUBLIC FILE_SET
         CXX_MODULES
         TYPE
         CXX_MODULES
         BASE_DIRS
         ${CMAKE_CURRENT_SOURCE_DIR}/..
         ${VULKAN_RENDERER_CONFIG_MODULE_DIR}
         FILES
         ${VULKAN_MODULE_INTERFACE_FILES}
         ${SHARED_MODULE_INTERFACE_FILES})

target_include_directories(VulkanEngineCore PUBLIC ${Vulkan_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_compile_definitions(VulkanEngineCore PUBLIC VULKAN_HPP_NO_EXCEPTIONS VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1)

target_link_libraries(
  VulkanEngineCore
  PUBLIC ${CMAKE_DL_LIBS}
         Threads::Threads
         Vulkan::Vulkan
         glfw
         imgui
         stb
         glm
         tinyobjloader
         cgltf
         vma
         ktx
         # enable compiler warnings
         myproject_warnings
         # enable sanitizers
         myproject_options
         # spdlog/nlohmann_json/GSL need to be PUBLIC because Main.cpp and test files use them
         GSL
         spdlog::spdlog
         nlohmann_json::nlohmann_json
         tomlplusplus_modules)

if(RUST_FEATURES)
  # Ensure CXX bridge headers are generated before module scanning
  add_dependencies(VulkanEngineCore oxidant_bridge)
  target_link_libraries(VulkanEngineCore PUBLIC oxidant oxidant_bridge)
endif()

# Linux Debug ASan, except under TSan: -fsanitize=address and -fsanitize=thread are mutually exclusive.
set(_kat_dbg_linux "$<AND:$<CONFIG:Debug>,$<PLATFORM_ID:Linux>>")
set(_kat_dbg_linux_asan
    "$<AND:$<CONFIG:Debug>,$<PLATFORM_ID:Linux>,$<NOT:$<BOOL:${myproject_ENABLE_SANITIZER_THREAD}>>>")
target_compile_options(VulkanEngineCore PUBLIC $<${_kat_dbg_linux_asan}:-fsanitize=address>
                                               $<${_kat_dbg_linux}:-fno-omit-frame-pointer>)
target_link_options(VulkanEngineCore PUBLIC $<${_kat_dbg_linux_asan}:-fsanitize=address>)

# GraphicsEngine executable
add_executable(${PROJECT_NAME})

kataglyphis_set_module_scan(${PROJECT_NAME})

target_sources(
  ${PROJECT_NAME}
  PRIVATE ${VULKANRENDERER_MAIN_SOURCE}
          # IMGUI object library - excluded from static analysis
          $<TARGET_OBJECTS:IMGUI>)

# Since CMake 3.28 the PUBLIC CXX_MODULES file set makes the modules importable here.
target_link_libraries(${PROJECT_NAME} PRIVATE VulkanEngineCore absl::flags_parse)


