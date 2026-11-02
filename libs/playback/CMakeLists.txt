# Live audio playback (one output device per process) on miniaudio, vendored in
# third_party/miniaudio (public domain / MIT-0), and MIDI input on what each system ships. Device backends load at runtime,
# so nothing extra is needed to build; with no sound card it plays silently.
add_library(wetwaresuite_playback src/player.cpp src/commands.cpp src/miniaudio_impl.cpp src/midi_in.cpp)
add_library(wetwaresuite::playback ALIAS wetwaresuite_playback)
target_include_directories(wetwaresuite_playback PUBLIC include PRIVATE ${PROJECT_SOURCE_DIR}/third_party/miniaudio)
target_link_libraries(wetwaresuite_playback PUBLIC wetwaresuite::audio wetwaresuite::core)
find_package(Threads REQUIRED)
target_link_libraries(wetwaresuite_playback PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
if(WIN32)
  target_link_libraries(wetwaresuite_playback PRIVATE winmm)
endif()
if(APPLE)
  target_link_libraries(wetwaresuite_playback PRIVATE "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox" "-framework CoreMIDI")
elseif(UNIX)
  target_link_libraries(wetwaresuite_playback PRIVATE m)
endif()

if(WETWARESUITE_BUILD_TESTS)
  add_executable(test_playback tests/test_playback.cpp)
  target_link_libraries(test_playback PRIVATE wetwaresuite::playback wetwaresuite_check)
  add_test(NAME playback COMMAND test_playback)
  set_tests_properties(playback PROPERTIES ENVIRONMENT "WETWARESUITE_AUDIO=null")
endif()
