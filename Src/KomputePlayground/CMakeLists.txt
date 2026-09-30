cmake_minimum_required(VERSION 3.20)
project(kompute_array_mult)

set(CMAKE_CXX_STANDARD 14)

# Options
option(KOMPUTE_OPT_GIT_TAG "The tag of the repo to use for the example" v0.9.0)
option(KOMPUTE_OPT_FROM_SOURCE "Whether to build example from source or from git fetch repo" ON)

# Shaders: one vulkan_compile_shader call per shader
vulkan_compile_shader(
  INFILE
  shader/my_shader.comp
  OUTFILE
  shader/my_shader.hpp
  NAMESPACE
  "shader")

# Then add it to the library, so you can access it later in your code
add_library(shader INTERFACE "shader/my_shader.hpp")
target_include_directories(shader INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)

# Setting up main example code
add_executable(kompute_array_mult src/main.cpp)
target_link_libraries(kompute_array_mult PRIVATE shader kompute::kompute)
