# Release only for the arm64 cross lane: FuzzTest unit mode, ASan nowhere, so abseil and the targets still agree.
set(_kataglyphis_fuzz_unit_mode OFF)
if(CMAKE_BUILD_TYPE STREQUAL "Release" AND KATAGLYPHIS_RELEASE_FUZZ_TESTS)
  set(_kataglyphis_fuzz_unit_mode ON)
elseif(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
  message(STATUS "Skipping fuzz tests: build type is '${CMAKE_BUILD_TYPE}', requires Debug.")
  return()
endif()

if(NOT CMAKE_CXX_COMPILER_ID MATCHES ".*Clang")
  message(STATUS "Skipping fuzz tests: requires Clang or clang-cl.")
  return()
endif()

if(NOT(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR (WIN32 AND MSVC AND (KATAGLYPHIS_ENABLE_FUZZTEST_FUZZING_MODE OR _kataglyphis_fuzz_unit_mode))))
  message(STATUS "Skipping fuzz tests: supported on Linux Clang and Windows clang-cl fuzzing or unit mode.")
  return()
endif()

# Abseil's raw_hash_set layout follows ASan, so ASan on the fuzz targets alone is an ODR violation that SEGVs.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT myproject_ENABLE_SANITIZER_ADDRESS)
  message(
    STATUS
      "Skipping fuzz tests: they need myproject_ENABLE_SANITIZER_ADDRESS=ON so "
      "abseil and the fuzz targets agree on container layout. Use the "
      "linux-debug-asan-clang preset.")
  return()
endif()

message(STATUS "Clang detected: enabling fuzz test targets.")

include(${CMAKE_SOURCE_DIR}/Test/cmake/CommonTestBuild.cmake)

function(kataglyphis_add_fuzz_test fuzz_target source_file)
  add_executable(${fuzz_target} ${source_file})

  # Without module scanning clang reports: module 'kataglyphis.vulkan.*' not found.
  set_target_properties(${fuzz_target} PROPERTIES CXX_SCAN_FOR_MODULES ${MYPROJECT_CXX_SCAN_FOR_MODULES})

  # Unit mode instruments nothing: coverage needs FuzzTest's fuzzing runtime, and ADDRESS_SANITIZER flips abseil's layout.
  if(_kataglyphis_fuzz_unit_mode)
    target_compile_definitions(${fuzz_target} PRIVATE FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION)
    link_fuzztest(${fuzz_target})
    return()
  endif()

  if(MSVC)
    target_link_libraries(${fuzz_target} PRIVATE kataglyphis_fuzztest_windows_asan)

    target_compile_options(
      ${fuzz_target}
      PRIVATE /Z7
              -UNDEBUG
              -fsanitize-coverage=inline-8bit-counters
              -fsanitize-coverage=trace-cmp)
  else()
    # No -fsanitize=address here: it must come project-wide from the preset, or abseil disagrees.
    target_compile_options(
      ${fuzz_target}
      PRIVATE -g
              -UNDEBUG
              -fsanitize-coverage=inline-8bit-counters
              -fsanitize-coverage=trace-cmp)
  endif()

  target_compile_definitions(
    ${fuzz_target}
    PRIVATE FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
            ADDRESS_SANITIZER)



  if(FUZZTEST_COMPATIBILITY_MODE STREQUAL "libfuzzer")
    target_compile_options(${fuzz_target} PRIVATE -fsanitize=fuzzer-no-link)
    target_compile_definitions(${fuzz_target} PRIVATE FUZZTEST_COMPATIBILITY_MODE)
  endif()

  link_fuzztest(${fuzz_target})

  # Not ctest-discovered: discovery runs the binary, which SEGVs in a build without project-wide ASan.
endfunction()

kataglyphis_add_fuzz_test(first_fuzz_test dummy.cpp)
kataglyphis_add_fuzz_test(example_fuzz_test example_fuzz_test.cpp)
# Real engine surface: OBJ parsing (mirrors ObjLoader::loadVertices).
kataglyphis_add_fuzz_test(obj_parsing_fuzz_test obj_parsing_fuzz_test.cpp)
target_link_libraries(obj_parsing_fuzz_test PRIVATE tinyobjloader)

# cgltf is what the GUI model picker exposes to untrusted files; like every target here it links no renderer.
kataglyphis_add_fuzz_test(gltf_parsing_fuzz_test gltf_parsing_fuzz_test.cpp)
target_link_libraries(gltf_parsing_fuzz_test PRIVATE cgltf)

# Never link VulkanEngineCore: its PUBLIC -fsanitize=address would build this TU against uninstrumented abseil.
kataglyphis_add_fuzz_test(scene_config_fuzz_test scene_config_fuzz_test.cpp)
target_sources(
  scene_config_fuzz_test
  PRIVATE ${CMAKE_SOURCE_DIR}/Src/GraphicsEngineVulkan/scene/SceneConfig.cpp
  PUBLIC FILE_SET
         scene_config_module
         TYPE
         CXX_MODULES
         BASE_DIRS
         ${CMAKE_SOURCE_DIR}/Src/GraphicsEngineVulkan
         ${CMAKE_SOURCE_DIR}/Src/shared
         FILES
         ${CMAKE_SOURCE_DIR}/Src/GraphicsEngineVulkan/scene/SceneConfig.ixx
         ${CMAKE_SOURCE_DIR}/Src/GraphicsEngineVulkan/scene/ModelFileKind.ixx
         ${CMAKE_SOURCE_DIR}/Src/shared/util/ResourcePaths.ixx)
target_link_libraries(scene_config_fuzz_test PRIVATE glm)
# Same values kataglyphis_apply_runtime_compile_definitions gives the engine, so paths and clip conventions match.
target_compile_definitions(
  scene_config_fuzz_test
  PRIVATE GLM_FORCE_DEPTH_ZERO_TO_ONE
          GLM_FORCE_RADIANS
          RELATIVE_RESOURCE_PATH="/../Resources/")

# Every shader load reads through this module; it is compiled straight in, with no renderer linked.
kataglyphis_add_fuzz_test(shader_file_reader_fuzz_test shader_file_reader_fuzz_test.cpp)
target_sources(
  shader_file_reader_fuzz_test
  PUBLIC FILE_SET
         file_reader_module
         TYPE
         CXX_MODULES
         BASE_DIRS
         ${CMAKE_SOURCE_DIR}/Src/shared
         FILES
         ${CMAKE_SOURCE_DIR}/Src/shared/util/FileReader.ixx)

# The decoded byte count sizes a memcpy'd staging buffer; STB_IMAGE_IMPLEMENTATION lives in the fuzz source.
kataglyphis_add_fuzz_test(texture_loading_fuzz_test texture_loading_fuzz_test.cpp)
target_sources(
  texture_loading_fuzz_test
  PUBLIC FILE_SET
         texture_decode_module
         TYPE
         CXX_MODULES
         BASE_DIRS
         ${CMAKE_SOURCE_DIR}/Src/GraphicsEngineVulkan
         FILES
         ${CMAKE_SOURCE_DIR}/Src/GraphicsEngineVulkan/scene/TextureDecode.ixx)
target_link_libraries(texture_loading_fuzz_test PRIVATE stb)
