# ANTfrastructure's GTestDiscovery (on CMAKE_MODULE_PATH) registers the tests and includes GoogleTest itself.
include(GTestDiscovery)

function(kataglyphis_disable_test_warnings test_target)
  if(MSVC)
    target_compile_options(${test_target} PRIVATE /w)
  else()
    target_compile_options(${test_target} PRIVATE -w)
  endif()
endfunction()

function(kataglyphis_configure_common_test_target test_target)
  set_target_properties(${test_target} PROPERTIES CXX_SCAN_FOR_MODULES ${MYPROJECT_CXX_SCAN_FOR_MODULES})
  kataglyphis_disable_test_warnings(${test_target})
endfunction()

function(kataglyphis_add_imgui_test_sources test_target)
  target_sources(
    ${test_target}
    PRIVATE
      # IMGUI object library - excluded from static analysis
      $<TARGET_OBJECTS:IMGUI>)
endfunction()

function(kataglyphis_enable_windows_vulkan_delay_load test_target)
  if(WIN32 AND MSVC)
    target_link_options(${test_target} PRIVATE /DELAYLOAD:vulkan-1.dll)
    target_link_libraries(${test_target} PRIVATE delayimp)
  endif()
endfunction()

# WORKING_DIRECTORY is opt-in upstream; the suites read resources relative to the repo root, like the executable.
function(kataglyphis_configure_gtest_discovery test_target)
  kataglyphis_register_gtest_target(${test_target} WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endfunction()
