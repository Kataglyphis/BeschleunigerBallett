# Packaging metadata for THIS project. The wiring it feeds - generator
# selection per platform, the architecture-normalised package name, the NSIS,
# WiX, DEB and AppImage blocks - lives in ContainerHub's CPackCommon module,
# because AccelerANTgine had a copy-seeded duplicate of all of it and the two
# copies had already drifted.
#
# Everything below is a value that identifies GraphicsEngine and nothing else:
# its icons, its installer copy, its MSI upgrade code, the .desktop file it
# installs. Add project-specific CPACK_* after the call and before include(CPack)
# if this project ever needs one the module does not model.
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
  # STABLE FOREVER. Changing this breaks upgrades and uninstalls of every MSI
  # already installed in the field. AccelerANTgine used to carry this same
  # literal value; it now has its own, which is what makes the two products
  # distinct to the Windows Installer instead of aliases for each other.
  WIX_UPGRADE_GUID
  "A8B86F5E-5B3E-4C38-9D7F-4F4923F9E5C2"
  WIX_PRODUCT_ICON
  "${CMAKE_CURRENT_SOURCE_DIR}/images/faviconNew.ico"
  WIX_DEFAULT
  ON
  APPIMAGE_DEFAULT
  ON
  # Must match what CMakeLists.txt installs into share/applications, and the
  # icon name it installs under share/icons - the AppImage generator resolves
  # both by name, not by path.
  APPIMAGE_DESKTOP_FILE
  "GraphicsEngine.desktop"
  APPIMAGE_ICON_NAME
  "Engine_logo")

include(CPack)
