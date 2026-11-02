add_library(wetwaresuite_cut
  src/model.cpp src/media.cpp src/stream.cpp src/effects.cpp src/text.cpp src/compose.cpp src/mix.cpp src/export.cpp
  src/helpers.cpp src/cmd_document.cpp src/cmd_add.cpp src/cmd_edit.cpp src/cmd_trim.cpp src/cmd_sequence.cpp
  src/cmd_effects.cpp src/cmd_titles.cpp src/cmd_audio.cpp src/cmd_export.cpp src/cmd_files.cpp src/strips.cpp src/proxy.cpp src/cmd_proxy.cpp)
add_library(wetwaresuite::cut ALIAS wetwaresuite_cut)
target_include_directories(wetwaresuite_cut PUBLIC include)
target_link_libraries(wetwaresuite_cut PUBLIC wetwaresuite::core wetwaresuite::raster wetwaresuite::audio
  PRIVATE wetwaresuite::playback wetwaresuite::pdfdoc nlohmann_json::nlohmann_json)

if(WETWARESUITE_BUILD_TESTS)
  add_executable(test_cut tests/test_cut.cpp tests/test_render.cpp tests/test_edits.cpp)
  target_link_libraries(test_cut PRIVATE wetwaresuite::cut wetwaresuite::mcp wetwaresuite_check wetwaresuite::raster wetwaresuite::audio)
  add_test(NAME cut COMMAND test_cut)
  set_tests_properties(cut PROPERTIES ENVIRONMENT "WETWARESUITE_AUDIO=null;WETWARESUITE_HW_ENCODER=none;WETWARESUITE_PROXY_DIR=${CMAKE_CURRENT_BINARY_DIR}/test-proxies")
endif()
