# Only values that identify BeschleunigerBallett; the packaging wiring lives in ANTfrastructure's CPackCommon.
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
  "BeschleunigerBallett.desktop"
  APPIMAGE_ICON_NAME
  "BeschleunigerBallett"
  # glibc travels (LOADER below); the Vulkan loader and its drivers must be the machine's.
  DEBIAN_DEPENDS
  "libvulkan1")

# The user's name for a package: no build type or compiler in it (the hub's default names both).
set(CPACK_PACKAGE_FILE_NAME "${PROJECT_NAME}-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${KATAGLYPHIS_CPACK_ARCH}")
# Up to 1.7.0 the .deb was graphicsengine; it ships the same /usr/Resources files, so it must step aside.
set(CPACK_DEBIAN_PACKAGE_REPLACES "graphicsengine")
set(CPACK_DEBIAN_PACKAGE_CONFLICTS "graphicsengine")
set(CPACK_DEBIAN_PACKAGE_PROVIDES "graphicsengine")

# The compiler's libstdc++ and the image's glibc: GCC 16.2's and glibc 2.43 are newer than Debian 13's.
kataglyphis_install_linux_closure(
  LOADER
  PRIVATE_DIR
  beschleunigerballett
  TARGETS
  ${PROJECT_NAME}
  SYSTEM
  "libvulkan.so.1")

include(CPack)
