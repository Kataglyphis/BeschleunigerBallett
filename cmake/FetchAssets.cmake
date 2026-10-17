# Puts the AssetANT scenes Resources/assets.lock pins into Resources/Models; runs alone as cmake -P cmake/FetchAssets.cmake.
include_guard(GLOBAL)

if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
  cmake_minimum_required(VERSION 3.24)
endif()

cmake_path(
  GET
  CMAKE_CURRENT_LIST_DIR
  PARENT_PATH
  _KATAGLYPHIS_ASSETS_ROOT)

# What the default scenes, the tests and the packages read; KATAGLYPHIS_FETCH_ALL_ASSETS takes every locked archive.
set(KATAGLYPHIS_DEFAULT_ASSETS crytek-sponza Dinosaurs)

function(_kataglyphis_asset_cache_dir out_var)
  if(NOT
     "$ENV{KATAGLYPHIS_ASSET_CACHE}"
     STREQUAL
     "")
    set(_cache "$ENV{KATAGLYPHIS_ASSET_CACHE}")
  elseif(
    CMAKE_HOST_WIN32
    AND NOT
        "$ENV{LOCALAPPDATA}"
        STREQUAL
        "")
    set(_cache "$ENV{LOCALAPPDATA}/kataglyphis-assets")
  elseif(
    NOT
    "$ENV{XDG_CACHE_HOME}"
    STREQUAL
    "")
    set(_cache "$ENV{XDG_CACHE_HOME}/kataglyphis-assets")
  elseif(
    NOT
    "$ENV{HOME}"
    STREQUAL
    "")
    set(_cache "$ENV{HOME}/.cache/kataglyphis-assets")
  else()
    message(FATAL_ERROR "FetchAssets: no cache directory; set KATAGLYPHIS_ASSET_CACHE.")
  endif()
  get_filename_component(_cache "${_cache}" ABSOLUTE)
  set(${out_var}
      "${_cache}"
      PARENT_SCOPE)
endfunction()

function(_kataglyphis_asset_download url part)
  foreach(_attempt RANGE 1 3)
    file(REMOVE "${part}")
    file(
      DOWNLOAD "${url}" "${part}"
      TLS_VERIFY ON
      INACTIVITY_TIMEOUT 120
      STATUS _status)
    list(
      GET
      _status
      0
      _rc)
    list(
      GET
      _status
      1
      _msg)
    if(_rc EQUAL 0)
      return()
    endif()
    message(STATUS "FetchAssets: attempt ${_attempt} of 3 for ${url} failed: ${_rc} ${_msg}")
  endforeach()
  file(REMOVE "${part}")
  message(
    FATAL_ERROR
      "FetchAssets: downloading ${url} FAILED: ${_rc} ${_msg}\n"
      "Offline, put the archive in the cache as <sha256>-<file> (KATAGLYPHIS_ASSET_CACHE names the directory), "
      "or configure with -DKATAGLYPHIS_FETCH_ASSETS=OFF and go without the scenes and the tests that read them.")
endfunction()

function(
  _kataglyphis_fetch_asset
  name
  sha
  file
  url
  destination
  cache)
  set(_target "${destination}/${name}")
  set(_stamp "${_target}/.assetant-sha256")
  if(EXISTS "${_stamp}")
    file(READ "${_stamp}" _have)
    string(STRIP "${_have}" _have)
    # The count catches files gone under a kept stamp, as after checking out a commit that still tracked the scene.
    file(
      GLOB_RECURSE _files
      LIST_DIRECTORIES false
      "${_target}/*")
    list(REMOVE_ITEM _files "${_stamp}")
    list(LENGTH _files _count)
    if(_have STREQUAL "${sha} ${_count}")
      message(STATUS "FetchAssets: ${name} is up to date")
      return()
    endif()
    message(STATUS "FetchAssets: ${name} is stale or incomplete, extracting it again")
  elseif(EXISTS "${_target}")
    message(
      FATAL_ERROR "FetchAssets: ${_target} exists but was not fetched (no .assetant-sha256 in it).\n"
                  "Move it away or delete it, and configure again; this step replaces only what it extracted itself.")
  endif()

  set(_archive "${cache}/${sha}-${file}")
  if(EXISTS "${_archive}")
    file(SHA256 "${_archive}" _got)
    if(NOT
       _got
       STREQUAL
       sha)
      message(STATUS "FetchAssets: dropping the corrupt cached ${_archive}")
      file(REMOVE "${_archive}")
    endif()
  endif()
  if(NOT EXISTS "${_archive}")
    message(STATUS "FetchAssets: downloading ${url}")
    _kataglyphis_asset_download("${url}" "${_archive}.part")
    file(SHA256 "${_archive}.part" _got)
    if(NOT
       _got
       STREQUAL
       sha)
      file(REMOVE "${_archive}.part")
      message(
        FATAL_ERROR
          "FetchAssets: ${url} has sha256 ${_got}, but Resources/assets.lock pins ${sha}; the download was deleted.\n"
          "A published archive never changes: a new one is a new AssetANT release (<name>-v<N+1>) and a new lock line.")
    endif()
    file(RENAME "${_archive}.part" "${_archive}")
  endif()

  # The stamp goes in last, so an interrupted extraction is redone rather than trusted.
  file(REMOVE_RECURSE "${_target}")
  file(
    ARCHIVE_EXTRACT
    INPUT
    "${_archive}"
    DESTINATION
    "${destination}"
    TOUCH)
  if(NOT IS_DIRECTORY "${_target}")
    message(FATAL_ERROR "FetchAssets: ${_archive} does not unpack to ${name}/, as every AssetANT archive does.")
  endif()
  file(
    GLOB_RECURSE _files
    LIST_DIRECTORIES false
    "${_target}/*")
  list(LENGTH _files _count)
  file(WRITE "${_stamp}" "${sha} ${_count}\n")
  message(STATUS "FetchAssets: ${name} extracted into ${_target}")
endfunction()

function(kataglyphis_fetch_assets)
  set(_lock "${_KATAGLYPHIS_ASSETS_ROOT}/Resources/assets.lock")
  set(_destination "${_KATAGLYPHIS_ASSETS_ROOT}/Resources/Models")
  if(NOT EXISTS "${_lock}")
    message(FATAL_ERROR "FetchAssets: no lock file ${_lock}")
  endif()
  if(NOT CMAKE_SCRIPT_MODE_FILE)
    set_property(
      DIRECTORY
      APPEND
      PROPERTY CMAKE_CONFIGURE_DEPENDS "${_lock}")
  endif()

  set(_names "")
  file(STRINGS "${_lock}" _lines ENCODING UTF-8)
  foreach(_line IN LISTS _lines)
    string(STRIP "${_line}" _line)
    if(_line STREQUAL "" OR _line MATCHES "^#")
      continue()
    endif()
    if(NOT
       _line
       MATCHES
       "^([0-9a-f]+)[ \t]+(([A-Za-z0-9][A-Za-z0-9._-]*)\\.tar\\.xz)[ \t]+([^ \t]+)$")
      message(FATAL_ERROR "FetchAssets: ${_lock}: not '<sha256> <name>.tar.xz <url>': ${_line}")
    endif()
    set(_sha "${CMAKE_MATCH_1}")
    set(_name "${CMAKE_MATCH_3}")
    string(LENGTH "${_sha}" _sha_length)
    if(NOT
       _sha_length
       EQUAL
       64
       OR _name IN_LIST _names)
      message(FATAL_ERROR "FetchAssets: ${_lock}: a short sha256 or a second line for ${_name}: ${_line}")
    endif()
    list(APPEND _names "${_name}")
    set(_sha_${_name} "${_sha}")
    set(_file_${_name} "${CMAKE_MATCH_2}")
    set(_url_${_name} "${CMAKE_MATCH_4}")
  endforeach()

  if(KATAGLYPHIS_FETCH_ALL_ASSETS)
    set(_wanted ${_names})
  else()
    set(_wanted ${KATAGLYPHIS_DEFAULT_ASSETS})
  endif()
  foreach(_name IN LISTS _wanted)
    if(NOT
       _name
       IN_LIST
       _names)
      message(FATAL_ERROR "FetchAssets: ${_name} is not in ${_lock}")
    endif()
  endforeach()

  _kataglyphis_asset_cache_dir(_cache)
  file(MAKE_DIRECTORY "${_cache}")
  # One fetch per cache at a time: two configures would otherwise extract over each other.
  file(
    LOCK "${_cache}/.lock"
    GUARD FUNCTION
    TIMEOUT 1800
    RESULT_VARIABLE _lock_result)
  if(NOT
     _lock_result
     EQUAL
     0)
    message(FATAL_ERROR "FetchAssets: could not lock ${_cache}/.lock: ${_lock_result}")
  endif()
  foreach(_name IN LISTS _wanted)
    _kataglyphis_fetch_asset(
      "${_name}"
      "${_sha_${_name}}"
      "${_file_${_name}}"
      "${_url_${_name}}"
      "${_destination}"
      "${_cache}")
  endforeach()
endfunction()

if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
  kataglyphis_fetch_assets()
endif()
