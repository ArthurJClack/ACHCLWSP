add_library(wetwaresuite_model
  src/poly.cpp src/primitives.cpp src/select.cpp src/extrude.cpp src/bevel.cpp src/cut.cpp src/topo.cpp
  src/subsurf.cpp src/uv.cpp src/scene.cpp src/modifiers.cpp src/io.cpp
  src/cmd_common.cpp src/cmd_doc.cpp src/cmd_object.cpp src/cmd_object2.cpp src/cmd_select.cpp src/cmd_edit.cpp src/cmd_edit2.cpp
  src/cmd_modifier.cpp src/cmd_render.cpp src/rig.cpp src/cmd_rig.cpp)
add_library(wetwaresuite::model ALIAS wetwaresuite_model)
target_include_directories(wetwaresuite_model PUBLIC include)
target_link_libraries(wetwaresuite_model PUBLIC wetwaresuite::core wetwaresuite::mesh wetwaresuite::raster
  PRIVATE wetwaresuite::sculpt wetwaresuite::render wetwaresuite::animate nlohmann_json::nlohmann_json)

if(WETWARESUITE_BUILD_TESTS)
  add_executable(test_model_ops tests/test_ops.cpp)
  target_link_libraries(test_model_ops PRIVATE wetwaresuite::model wetwaresuite_check)
  add_test(NAME model_ops COMMAND test_model_ops)
  add_executable(test_model tests/test_model.cpp)
  target_link_libraries(test_model PRIVATE wetwaresuite::model wetwaresuite::mcp nlohmann_json::nlohmann_json wetwaresuite_check)
  add_test(NAME model COMMAND test_model)
  add_executable(test_model_rig tests/test_rig.cpp)
  target_link_libraries(test_model_rig PRIVATE wetwaresuite::model wetwaresuite::render wetwaresuite_check)
  add_test(NAME model_rig COMMAND test_model_rig)
endif()
