# The C++ bridge's crate graph for the target Corrosion builds; the hub stager fails on an unreviewed source.
get_filename_component(_myproject_cargo_license_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
# FindRust keeps Rust_CARGO and Rust_CARGO_TARGET in third_party/'s scope; its *_CACHED copies are global.
if(NOT Rust_CARGO_CACHED OR NOT Rust_CARGO_TARGET_CACHED)
  message(
    FATAL_ERROR
      "CargoLicenses.cmake needs Corrosion's Rust_CARGO_CACHED and Rust_CARGO_TARGET_CACHED; include it after Corrosion."
  )
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(_myproject_oxidant "${_myproject_cargo_license_root}/third_party/OxidANT")
set(_myproject_cargo_licenses "${PROJECT_BINARY_DIR}/cargo-licenses")
add_custom_command(
  OUTPUT "${_myproject_cargo_licenses}/manifest.json"
  COMMAND "${CMAKE_COMMAND}" -E rm -rf "${_myproject_cargo_licenses}"
  COMMAND "${Rust_CARGO_CACHED}" fetch --locked --manifest-path "${_myproject_oxidant}/Cargo.toml" --target
          "${Rust_CARGO_TARGET_CACHED}"
  COMMAND
    "${Python3_EXECUTABLE}"
    "${_myproject_cargo_license_root}/third_party/ANTfrastructure/linux/scripts/06-packaging/stage-cargo-licenses.py"
    --workspace "${_myproject_oxidant}" --root-package kataglyphis_cxx_bridge --target "${Rust_CARGO_TARGET_CACHED}"
    --policy "${_myproject_oxidant}/packaging/cargo-license-policy.json" --output-dir "${_myproject_cargo_licenses}"
  DEPENDS "${_myproject_oxidant}/Cargo.lock" "${_myproject_oxidant}/packaging/cargo-license-policy.json"
  VERBATIM)
add_custom_target(myproject_cargo_licenses ALL DEPENDS "${_myproject_cargo_licenses}/manifest.json")
install(DIRECTORY "${_myproject_cargo_licenses}/" DESTINATION "share/licenses/${PROJECT_NAME}/cargo")
