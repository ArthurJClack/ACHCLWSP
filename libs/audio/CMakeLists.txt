add_library(wetwaresuite_audio
  src/buffer.cpp src/io.cpp src/decode_dr.cpp src/decode_vorbis.cpp src/internal.cpp src/dsp.cpp src/dynamics.cpp
  src/spectral.cpp src/effects.cpp src/analysis.cpp src/draw.cpp src/project.cpp src/document.cpp
  src/rack.cpp src/repair.cpp src/clap_host.cpp)
add_library(wetwaresuite::audio ALIAS wetwaresuite_audio)
target_include_directories(wetwaresuite_audio PUBLIC include
  PRIVATE ${PROJECT_SOURCE_DIR}/third_party/dr_libs ${PROJECT_SOURCE_DIR}/third_party/stb_vorbis ${PROJECT_SOURCE_DIR}/third_party/clap/include)
# CLAP plugins load at run time (dlopen / LoadLibrary): only the MIT-licensed headers are vendored
target_link_libraries(wetwaresuite_audio PUBLIC wetwaresuite::core wetwaresuite::raster PRIVATE nlohmann_json::nlohmann_json ${CMAKE_DL_LIBS})

if(WETWARESUITE_BUILD_TESTS)
  add_executable(test_audio_lib tests/test_audio_lib.cpp)
  target_link_libraries(test_audio_lib PRIVATE wetwaresuite::audio wetwaresuite_check)
  add_test(NAME audio_lib COMMAND test_audio_lib)
  add_executable(test_audio_rack tests/test_rack.cpp)
  target_link_libraries(test_audio_rack PRIVATE wetwaresuite::audio wetwaresuite_check)
  add_test(NAME audio_rack COMMAND test_audio_rack)
  # a tiny CLAP gain plugin to host in the tests
  add_library(test_clap_gain MODULE tests/clap_gain.cpp)
  target_include_directories(test_clap_gain PRIVATE ${PROJECT_SOURCE_DIR}/third_party/clap/include)
  set_target_properties(test_clap_gain PROPERTIES PREFIX "" SUFFIX ".clap" OUTPUT_NAME "test-gain")
  add_executable(test_clap tests/test_clap.cpp)
  target_link_libraries(test_clap PRIVATE wetwaresuite::audio wetwaresuite_check)
  target_compile_definitions(test_clap PRIVATE TEST_CLAP_PLUGIN="$<TARGET_FILE:test_clap_gain>")
  add_dependencies(test_clap test_clap_gain)
  add_test(NAME audio_clap COMMAND test_clap)
endif()
