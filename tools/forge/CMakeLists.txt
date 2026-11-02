add_library(wetwaresuite_forge src/forge.cpp src/mesh_commands.cpp src/image_commands.cpp src/sprite_commands.cpp src/texture.cpp src/meshops.cpp src/bake.cpp
  src/sprite_model.cpp src/aseprite.cpp src/sprite_export.cpp src/anim_commands.cpp)
add_library(wetwaresuite::forge ALIAS wetwaresuite_forge)
target_include_directories(wetwaresuite_forge PUBLIC include PRIVATE ${PROJECT_SOURCE_DIR}/third_party/stb)
find_package(Threads REQUIRED)
target_link_libraries(wetwaresuite_forge PUBLIC wetwaresuite::mesh wetwaresuite::raster PRIVATE nlohmann_json::nlohmann_json Threads::Threads)

if(WETWARESUITE_BUILD_TESTS)
  add_executable(test_forge tests/test_forge.cpp)
  target_link_libraries(test_forge PRIVATE wetwaresuite::forge wetwaresuite::mcp nlohmann_json::nlohmann_json wetwaresuite_check)
  add_test(NAME forge COMMAND test_forge)
endif()
