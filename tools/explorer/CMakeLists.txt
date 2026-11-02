add_library(wetwaresuite_explorer
  src/hash.cpp
  src/archive.cpp
  src/files.cpp
  src/cmd_browse.cpp
  src/cmd_change.cpp
  src/cmd_archive.cpp
  src/system_trash.cpp)
add_library(wetwaresuite::explorer ALIAS wetwaresuite_explorer)
target_include_directories(wetwaresuite_explorer PUBLIC include)
target_link_libraries(wetwaresuite_explorer PUBLIC wetwaresuite::core
  PRIVATE wetwaresuite::sysutil wetwaresuite_toolkit wetwaresuite::raster wetwaresuite::pdf)
if(WIN32)
  target_link_libraries(wetwaresuite_explorer PRIVATE shell32)
endif()

if(WETWARESUITE_BUILD_TESTS)
  add_executable(test_explorer tests/test_explorer.cpp)
  target_link_libraries(test_explorer PRIVATE wetwaresuite::explorer wetwaresuite::sysutil wetwaresuite::raster wetwaresuite::mcp nlohmann_json::nlohmann_json wetwaresuite_check)
  add_test(NAME explorer COMMAND test_explorer)
endif()
