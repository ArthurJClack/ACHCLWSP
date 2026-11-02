cmake_minimum_required(VERSION 3.21)
project(wetwaresuite VERSION 0.2.0 LANGUAGES CXX)
# what every program reports (--version, MCP) and the updater compares with GitHub
# releases; the downloads workflow sets it to the release's version
set(WETWARESUITE_VERSION "${PROJECT_VERSION}" CACHE STRING "version the programs report")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(WETWARESUITE_BUILD_TESTS "Build unit tests" ON)
# the desktop window (Dear ImGui + SDL3) every app opens when double-clicked;
# off, or without SDL3, the apps get a stub that says there's no window
option(WETWARESUITE_GUI "Build the desktop GUI (gui/)" ON)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8)
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
endif()

include(FetchContent)
FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(nlohmann_json)

if(WETWARESUITE_BUILD_TESTS)
  enable_testing()
endif()

# the logo and app icon live in branding/ (see branding/README.md).
# wetwaresuite_add_icon(target [app]) gives a Windows .exe the icon (an app's own,
# branding/apps/<app>.ico, when named); elsewhere it does nothing
if(WIN32)
  enable_language(RC)
  set(WW_ICON ${PROJECT_SOURCE_DIR}/branding/wetwaresuite.ico)
  configure_file(${PROJECT_SOURCE_DIR}/branding/wetwaresuite.rc.in ${PROJECT_BINARY_DIR}/wetwaresuite.rc @ONLY)
endif()
function(wetwaresuite_add_icon target)
  if(WIN32)
    if(ARGC GREATER 1 AND EXISTS ${PROJECT_SOURCE_DIR}/branding/apps/${ARGV1}.ico)
      set(WW_ICON ${PROJECT_SOURCE_DIR}/branding/apps/${ARGV1}.ico)
      configure_file(${PROJECT_SOURCE_DIR}/branding/wetwaresuite.rc.in ${PROJECT_BINARY_DIR}/icons/${ARGV1}.rc @ONLY)
      target_sources(${target} PRIVATE ${PROJECT_BINARY_DIR}/icons/${ARGV1}.rc)
    else()
      target_sources(${target} PRIVATE ${PROJECT_BINARY_DIR}/wetwaresuite.rc)
    endif()
  endif()
endfunction()

add_subdirectory(core)
add_subdirectory(mcp)
add_subdirectory(libs/raster)
add_subdirectory(libs/mesh)
add_subdirectory(libs/audio)
add_subdirectory(libs/playback)
add_subdirectory(libs/pdf)
add_subdirectory(libs/pdfdoc)
add_subdirectory(libs/sysutil)

# ---- apps: what gets built ----
# Each app is its own binary, wetwaresuite-<app>, carrying a few tools (the
# table is apps/common/apps.cpp; hub/src/catalog.rs mirrors it). Build only
# some with -DWETWARESUITE_APPS="image;model" or by turning WETWARESUITE_APP_<NAME>
# off; WETWARESUITE_BUILD_SUITE is the all-in-one `wetwaresuite` binary.
set(WETWARESUITE_ALL_APPS image draw model game cad electronics sound video office maps system files)
set(WETWARESUITE_APP_TOOLS_image photo paint)
set(WETWARESUITE_APP_TOOLS_draw vector type board)
set(WETWARESUITE_APP_TOOLS_model model sculpt render animate)
set(WETWARESUITE_APP_TOOLS_game scene forge levels)
set(WETWARESUITE_APP_TOOLS_cad cad blueprint slicer fit)
set(WETWARESUITE_APP_TOOLS_electronics circuit flash)
set(WETWARESUITE_APP_TOOLS_sound audio beats)
set(WETWARESUITE_APP_TOOLS_video cut capture)
set(WETWARESUITE_APP_TOOLS_office office pdf layout notes)
set(WETWARESUITE_APP_TOOLS_maps maps)
set(WETWARESUITE_APP_TOOLS_system calc code explorer terminal monitor clock)
set(WETWARESUITE_APP_TOOLS_files toolkit convert library automate)

option(WETWARESUITE_BUILD_SUITE "Build the all-in-one wetwaresuite binary (every tool)" ON)
set(WETWARESUITE_APPS "" CACHE STRING "Apps to build, e.g. image;model (empty = every app whose WETWARESUITE_APP_<NAME> option is ON)")
string(REPLACE "," ";" _ww_apps_requested "${WETWARESUITE_APPS}")
set(WETWARESUITE_ENABLED_APPS "")
foreach(app IN LISTS WETWARESUITE_ALL_APPS)
  string(TOUPPER "${app}" _up)
  option(WETWARESUITE_APP_${_up} "Build wetwaresuite-${app}" ON)
  if(_ww_apps_requested)
    if("${app}" IN_LIST _ww_apps_requested)
      list(APPEND WETWARESUITE_ENABLED_APPS ${app})
    endif()
  elseif(WETWARESUITE_APP_${_up})
    list(APPEND WETWARESUITE_ENABLED_APPS ${app})
  endif()
endforeach()
foreach(app IN LISTS _ww_apps_requested)
  if(NOT "${app}" IN_LIST WETWARESUITE_ALL_APPS)
    message(FATAL_ERROR "WETWARESUITE_APPS: no app called '${app}' (apps: ${WETWARESUITE_ALL_APPS})")
  endif()
endforeach()

# the tools those apps need, plus the tools those tools link against
set(_ww_tools "")
if(WETWARESUITE_BUILD_SUITE)
  foreach(app IN LISTS WETWARESUITE_ALL_APPS)
    list(APPEND _ww_tools ${WETWARESUITE_APP_TOOLS_${app}})
  endforeach()
else()
  foreach(app IN LISTS WETWARESUITE_ENABLED_APPS)
    list(APPEND _ww_tools ${WETWARESUITE_APP_TOOLS_${app}})
  endforeach()
endif()
set(_ww_tool_deps_board vector)
set(_ww_tool_deps_layout vector)
set(_ww_tool_deps_type vector)
set(_ww_tool_deps_library vector toolkit)
set(_ww_tool_deps_automate photo vector convert)  # its tests drive them
set(_ww_tool_deps_explorer toolkit)
set(_ww_tool_deps_paint photo)                    # its tests open through photo
set(_ww_tool_deps_fit sculpt)                     # smoothing, simplifying and picking
set(_ww_tool_deps_cad blueprint)                  # drawings from the model
set(_ww_tool_deps_model sculpt render animate)    # decimate modifier, renders, GIFs
foreach(t IN LISTS _ww_tools)
  list(APPEND _ww_tools ${_ww_tool_deps_${t}})
endforeach()
list(REMOVE_DUPLICATES _ww_tools)
foreach(t toolkit vector blueprint photo paint convert forge slicer cad layout audio cut capture library automate
          office pdf levels sculpt beats animate render model circuit flash board notes maps type fit calc code explorer terminal monitor clock)
  if("${t}" IN_LIST _ww_tools)
    add_subdirectory(tools/${t})
  endif()
endforeach()
message(STATUS "wetwaresuite apps: ${WETWARESUITE_ENABLED_APPS}; all-in-one: ${WETWARESUITE_BUILD_SUITE}")

# the window every app opens when started without arguments; with
# WETWARESUITE_GUI off (or no SDL3) a stub that says why there's no window
add_subdirectory(gui)
add_subdirectory(apps/common)
add_subdirectory(apps/app)
if(WETWARESUITE_BUILD_SUITE)
  add_subdirectory(apps/wetwaresuite)
endif()
