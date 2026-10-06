# Not linked to myproject_options on purpose: kompute's headers throw, so this target keeps exceptions on.

find_program(
  KOMPUTE_PLAYGROUND_GLSLANG_VALIDATOR
  NAMES glslangValidator
  HINTS "$ENV{VULKAN_SDK}/bin" REQUIRED)

# Generated into the build tree, never the source tree, so the SPIR-V cannot go stale beside its GLSL.
set(_kompute_playground_shader "${CMAKE_CURRENT_SOURCE_DIR}/shader/my_shader.comp")
set(_kompute_playground_shader_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/shader")
set(_kompute_playground_shader_header "${_kompute_playground_shader_dir}/my_shader.hpp")
add_custom_command(
  OUTPUT "${_kompute_playground_shader_header}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${_kompute_playground_shader_dir}"
  COMMAND "${KOMPUTE_PLAYGROUND_GLSLANG_VALIDATOR}" -V --vn MY_SHADER_COMP_SPV -o "${_kompute_playground_shader_header}"
          "${_kompute_playground_shader}"
  DEPENDS "${_kompute_playground_shader}"
  COMMENT "Compiling the Kompute playground shader to SPIR-V"
  VERBATIM)

add_executable(kompute_array_mult src/main.cpp "${_kompute_playground_shader_header}")
target_include_directories(kompute_array_mult PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(kompute_array_mult PRIVATE kompute::kompute)
