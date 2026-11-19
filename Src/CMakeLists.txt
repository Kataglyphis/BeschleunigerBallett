# The bridge crate alone: OxidANT's root package links GStreamer and the ORT loader, which the engine never calls.
corrosion_import_crate(
  MANIFEST_PATH
  ../third_party/OxidANT/Cargo.toml
  CRATE_TYPES
  staticlib
  CRATES
  kataglyphis_cxx_bridge
  LOCKED)

include(CorrosionToolchain)
kataglyphis_configure_corrosion_target(kataglyphis_cxx_bridge ADDRESS_SANITIZER ${myproject_ENABLE_SANITIZER_ADDRESS})
# OxidANT's release profile aborts on a panic, where guarded() must catch it: Debug keeps dev, which unwinds too.
set_property(TARGET kataglyphis_cxx_bridge PROPERTY INTERFACE_CORROSION_CARGO_PROFILE $<$<NOT:$<CONFIG:Debug>>:bridge>)

# One file per bridged area, each its own header (oxidant_bridge/<area>.h); BuildIntegrity holds the list to the crate.
corrosion_add_cxxbridge(
  oxidant_bridge
  CRATE
  kataglyphis_cxx_bridge
  FILES
  gltf.rs
  diagnostics.rs)

add_subdirectory(GraphicsEngineVulkan)
# Off by default: kompute's headers throw, so the playground cannot use the no-exceptions options.
if(KATAGLYPHIS_BUILD_KOMPUTE_PLAYGROUND)
  add_subdirectory(KomputePlayground)
endif()
