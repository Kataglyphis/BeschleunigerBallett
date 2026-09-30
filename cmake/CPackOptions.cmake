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
  "Engine_logo")

include(CPack)
