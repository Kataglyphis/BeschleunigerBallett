# Original license texts of every third-party component the engine links or ships; see docs/source/LICENSES-README.md.
include(FetchContent)
get_filename_component(_myproject_license_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

set(_myproject_third_party_license_dir "share/licenses/${PROJECT_NAME}/third_party")

function(_myproject_install_third_party_license name)
  foreach(_file IN LISTS ARGN)
    if(NOT EXISTS "${_file}")
      message(FATAL_ERROR "Third-party license text for ${name} is missing: ${_file}")
    endif()
    file(SIZE "${_file}" _size)
    if(_size EQUAL 0)
      message(FATAL_ERROR "Third-party license text for ${name} is empty: ${_file}")
    endif()
  endforeach()
  install(FILES ${ARGN} DESTINATION "${_myproject_third_party_license_dir}/${name}")
endfunction()

FetchContent_GetProperties(GSL SOURCE_DIR _myproject_gsl_source)
FetchContent_GetProperties(abseil-cpp SOURCE_DIR _myproject_abseil_source)

# One mapping, <component>/<file>=<source>, that the DEB and MSIX gates read back too.
file(STRINGS "${_myproject_license_root}/licenses/third-party-licenses.txt" _myproject_license_map)
foreach(_entry IN LISTS _myproject_license_map)
  string(
    REGEX MATCH
          "^([^/=]+)/([^/=]+)=(.+)$"
          _matched
          "${_entry}")
  get_filename_component(_source_name "${CMAKE_MATCH_3}" NAME)
  if(NOT _matched
     OR NOT
        _source_name
        STREQUAL
        CMAKE_MATCH_2)
    message(FATAL_ERROR "Malformed third-party license mapping: ${_entry}")
  endif()
  _myproject_install_third_party_license(${CMAKE_MATCH_1} "${_myproject_license_root}/${CMAKE_MATCH_3}")
endforeach()
_myproject_install_third_party_license(gsl "${_myproject_gsl_source}/LICENSE")
_myproject_install_third_party_license(abseil "${_myproject_abseil_source}/LICENSE")
