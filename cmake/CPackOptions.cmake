# Only values that identify GraphicsEngine; the packaging wiring lives in ANTfrastructure's CPackCommon.
include(CPackCommon)

kataglyphis_cpack_common(
  VENDOR
  "${AUTHOR}"
  PACKAGE_ICON
  "${CMAKE_CURRENT_SOURCE_DIR}/images/Engine_logo.png"
  NSIS_WELCOME_TITLE
  "Get ready for epic graphics."
  NSIS_FINISH_TITLE
  "Now you are ready to render :)"
  NSIS_HEADER_IMAGE
  "${CMAKE_CURRENT_SOURCE_DIR}/images/Engine_logo.bmp"
  NSIS_MUI_ICON
  "${CMAKE_CURRENT_SOURCE_DIR}/images/faviconNew.ico"
  # STABLE FOREVER: changing it breaks upgrades and uninstalls of every MSI already installed.
  WIX_UPGRADE_GUID
  "A8B86F5E-5B3E-4C38-9D7F-4F4923F9E5C2"
  WIX_PRODUCT_ICON
  "${CMAKE_CURRENT_SOURCE_DIR}/images/faviconNew.ico"
  WIX_DEFAULT
  ON
  APPIMAGE_DEFAULT
  ON
  # AppImage resolves the .desktop file and icon by name, so both must match what CMakeLists.txt installs.
  APPIMAGE_DESKTOP_FILE
  "GraphicsEngine.desktop"
  APPIMAGE_ICON_NAME
  "Engine_logo"
  # The packages bundle neither glibc nor the Vulkan loader: they need the image's base, Ubuntu 26.04, or newer.
  DEBIAN_DEPENDS
  "libc6 (>= 2.43), libvulkan1")

# The compiler's libstdc++: GCC 16.2's GLIBCXX_3.4.36 is newer than any distro's.
kataglyphis_install_linux_closure(
  PRIVATE_DIR
  graphicsengine
  TARGETS
  ${PROJECT_NAME}
  SYSTEM
  "libvulkan.so.1")

include(CPack)
