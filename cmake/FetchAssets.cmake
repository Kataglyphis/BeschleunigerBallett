# Puts AssetANT's locked scenes into Resources/Models and skyboxes into Resources/Textures/Skybox; runs alone as cmake -P.
include_guard(GLOBAL)

if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
  cmake_minimum_required(VERSION 3.24)
endif()

cmake_path(
  GET
  CMAKE_CURRENT_LIST_DIR
  PARENT_PATH
  _KATAGLYPHIS_ASSETS_ROOT)

# By path, because cmake -P has no module path.
include("${_KATAGLYPHIS_ASSETS_ROOT}/third_party/ANTfrastructure/cmake/KataglyphisFetchData.cmake")

# What the default scenes, the tests and the packages read; KATAGLYPHIS_FETCH_ALL_ASSETS takes every locked archive.
set(KATAGLYPHIS_DEFAULT_ASSETS crytek-sponza Dinosaurs)

function(kataglyphis_fetch_assets)
  set(_only "")
  if(NOT KATAGLYPHIS_FETCH_ALL_ASSETS)
    set(_only ONLY ${KATAGLYPHIS_DEFAULT_ASSETS})
  endif()
  kataglyphis_fetch_data(
    LOCK
    "${_KATAGLYPHIS_ASSETS_ROOT}/Resources/assets.lock"
    DEST
    "${_KATAGLYPHIS_ASSETS_ROOT}/Resources/Models"
    EXTRACT
    ${_only})
  # Its own lock: KATAGLYPHIS_FETCH_ALL_ASSETS must not extract a skybox into Resources/Models.
  kataglyphis_fetch_data(
    LOCK
    "${_KATAGLYPHIS_ASSETS_ROOT}/Resources/skyboxes.lock"
    DEST
    "${_KATAGLYPHIS_ASSETS_ROOT}/Resources/Textures/Skybox"
    EXTRACT)
endfunction()

if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
  kataglyphis_fetch_assets()
endif()
