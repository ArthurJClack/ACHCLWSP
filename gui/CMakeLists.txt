# The desktop GUI every app links: Dear ImGui (vendored, MIT) on SDL3 (zlib)
# with SDL_Renderer, so it runs on a GPU or in software on machines without one.
#
#   wetwaresuite_gui_logic  window-free logic (search, forms, ...); always built, unit tested
#   wetwaresuite_gui        wetwaresuite::gui::run(); a stub that explains itself when SDL3 is missing
#                           or WETWARESUITE_GUI is off. Every app binary links it (apps/common).
# The wetwaresuite-gui launcher is built in apps/wetwaresuite next to the all-in-one.
#
# SDL3 comes from find_package(SDL3) when installed, else the pinned release
# below is downloaded once at configure time (never at run time). With no network
# point FETCHCONTENT_SOURCE_DIR_SDL3 at an unpacked SDL3 source tree, or the
# window is skipped and the stub is built.

if(MSVC)
  set(WETWARESUITE_GUI_WARNINGS /W4 /permissive- /utf-8)
else()
  set(WETWARESUITE_GUI_WARNINGS -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
endif()

add_library(wetwaresuite_gui_logic src/logic.cpp src/layout.cpp src/handoff.cpp src/palette.cpp)
target_include_directories(wetwaresuite_gui_logic PUBLIC include)
target_link_libraries(wetwaresuite_gui_logic PUBLIC wetwaresuite::core)

if(WETWARESUITE_BUILD_TESTS)
  add_executable(test_gui_handoff tests/test_handoff.cpp)
  target_link_libraries(test_gui_handoff PRIVATE wetwaresuite_gui_logic wetwaresuite_check)
  add_test(NAME gui_handoff COMMAND test_gui_handoff)
  add_executable(test_gui_palette tests/test_palette.cpp)
  target_link_libraries(test_gui_palette PRIVATE wetwaresuite_gui_logic wetwaresuite_check)
  add_test(NAME gui_palette COMMAND test_gui_palette)
  add_executable(test_gui_layout tests/test_layout.cpp)
  target_link_libraries(test_gui_layout PRIVATE wetwaresuite_gui_logic wetwaresuite_check)
  add_test(NAME gui_layout COMMAND test_gui_layout)
endif()
if(WETWARESUITE_BUILD_TESTS AND TARGET wetwaresuite_photo AND TARGET wetwaresuite_paint)
  add_executable(test_gui_logic tests/test_logic.cpp)
  target_link_libraries(test_gui_logic PRIVATE wetwaresuite_gui_logic wetwaresuite::photo wetwaresuite::paint wetwaresuite_check)
  add_test(NAME gui_logic COMMAND test_gui_logic)
endif()

set(_ww_sdl "")
if(WETWARESUITE_GUI)
# ---- SDL3 ----------------------------------------------------------------------
set(WETWARESUITE_SDL3_VERSION 3.2.24)
set(WETWARESUITE_SDL3_SHA256 81cc0fc17e5bf2c1754eeca9af9c47a76789ac5efdd165b3b91cbbe4b90bfb76)
set(WETWARESUITE_SDL3_URL
    https://github.com/libsdl-org/SDL/releases/download/release-${WETWARESUITE_SDL3_VERSION}/SDL3-${WETWARESUITE_SDL3_VERSION}.tar.gz)

find_package(SDL3 CONFIG QUIET COMPONENTS SDL3)
if(SDL3_FOUND AND TARGET SDL3::SDL3)
  set(_ww_sdl SDL3::SDL3)
  message(STATUS "gui: using installed SDL3 ${SDL3_VERSION}")
else()
  set(_ww_archive ${CMAKE_BINARY_DIR}/_deps/SDL3-${WETWARESUITE_SDL3_VERSION}.tar.gz)
  set(_ww_have_source OFF)
  if(FETCHCONTENT_SOURCE_DIR_SDL3)
    set(_ww_have_source ON)
  else()
    if(NOT EXISTS ${_ww_archive})
      message(STATUS "gui: downloading SDL3 ${WETWARESUITE_SDL3_VERSION}")
      # no EXPECTED_HASH here: a failed download would then stop the whole configure
      file(DOWNLOAD ${WETWARESUITE_SDL3_URL} ${_ww_archive}.part TLS_VERIFY ON STATUS _ww_status)
      list(GET _ww_status 0 _ww_code)
      list(GET _ww_status 1 _ww_reason)
      if(_ww_code EQUAL 0)
        file(SHA256 ${_ww_archive}.part _ww_hash)
        if(NOT _ww_hash STREQUAL WETWARESUITE_SDL3_SHA256)
          set(_ww_code 1)
          set(_ww_reason "wrong SHA-256 ${_ww_hash}")
        endif()
      endif()
      if(_ww_code EQUAL 0)
        file(RENAME ${_ww_archive}.part ${_ww_archive})
      else()
        file(REMOVE ${_ww_archive}.part)
        message(WARNING "gui: couldn't download SDL3 (${_ww_reason}); building without a window. "
                        "Set FETCHCONTENT_SOURCE_DIR_SDL3 to an SDL3 source folder, or -DWETWARESUITE_GUI=OFF.")
      endif()
    endif()
    if(EXISTS ${_ww_archive})
      set(_ww_have_source ON)
    endif()
  endif()

  if(_ww_have_source)
    # only what the window needs: video, events, renderer and file dialogs
    set(SDL_SHARED OFF)
    set(SDL_STATIC ON)
    set(SDL_TEST_LIBRARY OFF)
    set(SDL_TESTS OFF)
    set(SDL_EXAMPLES OFF)
    set(SDL_DISABLE_INSTALL ON)
    set(SDL_DISABLE_UNINSTALL ON)
    set(SDL_AUDIO OFF)
    set(SDL_CAMERA OFF)
    set(SDL_HAPTIC OFF)
    set(SDL_JOYSTICK OFF)
    set(SDL_HIDAPI OFF)
    set(SDL_SENSOR OFF)
    set(SDL_GPU OFF)
    set(SDL_VULKAN OFF)
    set(SDL_RENDER_VULKAN OFF)
    set(SDL_RENDER_GPU OFF)
    if(UNIX AND NOT APPLE AND NOT ANDROID)
      # headless build machines often have no X11/Wayland headers: build anyway, the
      # window then reports that it can't open and the command line keeps working
      find_package(X11 QUIET)
      find_package(PkgConfig QUIET)
      set(_ww_wayland OFF)
      if(PKG_CONFIG_FOUND)
        pkg_check_modules(_WW_WAYLAND QUIET wayland-client wayland-egl wayland-cursor egl xkbcommon)
        set(_ww_wayland ${_WW_WAYLAND_FOUND})
      endif()
      if(NOT _ww_wayland)
        set(SDL_WAYLAND OFF)
      endif()
      if(NOT X11_FOUND)
        set(SDL_X11 OFF)
      endif()
      if(NOT X11_FOUND AND NOT _ww_wayland)
        message(WARNING "gui: no X11 or Wayland development files found; the window can only run offscreen. "
                        "Install libx11-dev (or the Wayland dev packages) for a desktop window.")
        set(SDL_UNIX_CONSOLE_BUILD ON)
      endif()
    endif()
    # SDL is C and has its own warning settings: don't hand it ours
    set_property(DIRECTORY PROPERTY COMPILE_OPTIONS "")
    FetchContent_Declare(SDL3 URL ${_ww_archive} URL_HASH SHA256=${WETWARESUITE_SDL3_SHA256} DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(SDL3)
    if(TARGET SDL3::SDL3-static)
      set(_ww_sdl SDL3::SDL3-static)
    elseif(TARGET SDL3::SDL3)
      set(_ww_sdl SDL3::SDL3)
    endif()
  endif()
endif()

endif()  # WETWARESUITE_GUI

# ---- the window -------------------------------------------------------------------
if(_ww_sdl)
  set(_imgui ${PROJECT_SOURCE_DIR}/third_party/imgui)
  add_library(wetwaresuite_imgui STATIC
    ${_imgui}/imgui.cpp ${_imgui}/imgui_draw.cpp ${_imgui}/imgui_tables.cpp ${_imgui}/imgui_widgets.cpp
    ${_imgui}/imgui_stdlib.cpp ${_imgui}/backends/imgui_impl_sdl3.cpp ${_imgui}/backends/imgui_impl_sdlrenderer3.cpp)
  target_include_directories(wetwaresuite_imgui PUBLIC ${_imgui} ${_imgui}/backends)
  target_compile_definitions(wetwaresuite_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
  target_link_libraries(wetwaresuite_imgui PUBLIC ${_ww_sdl})
  if(MSVC)
    target_compile_options(wetwaresuite_imgui PRIVATE /utf-8)
  endif()

  set(_font_cpp ${CMAKE_CURRENT_BINARY_DIR}/ui_font.cpp)
  add_custom_command(OUTPUT ${_font_cpp}
    COMMAND ${CMAKE_COMMAND} -DINPUT=${_imgui}/fonts/Roboto-Medium.ttf -DOUTPUT=${_font_cpp} -DNAME=kUiFont
            -DNAMESPACE=wetwaresuite::gui -P ${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake
    DEPENDS ${_imgui}/fonts/Roboto-Medium.ttf ${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake
    COMMENT "Embedding the UI font")
  set(_icon_cpp ${CMAKE_CURRENT_BINARY_DIR}/app_icon.cpp)
  set(_icon_png ${PROJECT_SOURCE_DIR}/branding/png/icon-128.png)
  add_custom_command(OUTPUT ${_icon_cpp}
    COMMAND ${CMAKE_COMMAND} -DINPUT=${_icon_png} -DOUTPUT=${_icon_cpp} -DNAME=kAppIcon
            -DNAMESPACE=wetwaresuite::gui -P ${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake
    DEPENDS ${_icon_png} ${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake
    COMMENT "Embedding the window icon")

  add_library(wetwaresuite_gui src/shell.cpp src/panels.cpp src/chrome.cpp src/canvas.cpp src/retouch.cpp src/game_view.cpp src/handoff_panels.cpp src/colours.cpp src/updates.cpp src/transport.cpp src/video_play.cpp src/orbit.cpp src/handles.cpp src/brush3d.cpp src/dopesheet.cpp src/board_view.cpp src/glyph_view.cpp src/transform_view.cpp src/preview.cpp src/timeline_view.cpp src/deck_view.cpp src/layout_view.cpp src/sketch_view.cpp src/grid_view.cpp src/selection_view.cpp src/vector_view.cpp src/brush_tools.cpp src/layers_view.cpp src/sound_view.cpp src/launcher_view.cpp src/window.cpp ${_font_cpp} ${_icon_cpp}
    # the system tools' panels: calculator, code editor, files, terminal, monitor, clock
    src/tool_panels.cpp src/code_editor.cpp src/panel_calc.cpp src/panel_code.cpp src/panel_explorer.cpp
    src/panel_terminal.cpp src/panel_monitor.cpp src/panel_clock.cpp src/pdf_view.cpp
    src/panel_circuit.cpp src/maps_view.cpp)
  target_compile_options(wetwaresuite_gui PRIVATE ${WETWARESUITE_GUI_WARNINGS})
  target_link_libraries(wetwaresuite_gui PUBLIC wetwaresuite_gui_logic PRIVATE wetwaresuite_imgui wetwaresuite::raster)
  message(STATUS "gui: window enabled (${_ww_sdl})")
else()
  add_library(wetwaresuite_gui src/stub.cpp)
  target_compile_options(wetwaresuite_gui PRIVATE ${WETWARESUITE_GUI_WARNINGS})
  target_link_libraries(wetwaresuite_gui PUBLIC wetwaresuite_gui_logic)
  if(NOT WETWARESUITE_GUI)
    target_compile_definitions(wetwaresuite_gui PRIVATE WW_GUI_DISABLED=1)
    message(STATUS "gui: WETWARESUITE_GUI is off, building the stub (apps say there's no window)")
  else()
    message(STATUS "gui: no SDL3, building the stub (apps say there's no window)")
  endif()
endif()
add_library(wetwaresuite::gui ALIAS wetwaresuite_gui)
