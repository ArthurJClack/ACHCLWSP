find_package(Threads REQUIRED)

add_library(wetwaresuite_library src/catalog.cpp src/scan.cpp src/query.cpp src/thumbs.cpp src/commands.cpp src/xmp.cpp)
add_library(wetwaresuite::library ALIAS wetwaresuite_library)
target_include_directories(wetwaresuite_library PUBLIC include)
target_link_libraries(wetwaresuite_library PUBLIC wetwaresuite::core wetwaresuite::raster
  PRIVATE wetwaresuite::mesh wetwaresuite::vector wetwaresuite_toolkit nlohmann_json::nlohmann_json Threads::Threads)

if(WETWARESUITE_BUILD_TESTS)
  add_executable(test_library tests/test_library.cpp)
  target_link_libraries(test_library PRIVATE wetwaresuite::library wetwaresuite::mesh wetwaresuite::vector wetwaresuite::mcp nlohmann_json::nlohmann_json wetwaresuite_check)
  add_test(NAME library COMMAND test_library)
endif()
