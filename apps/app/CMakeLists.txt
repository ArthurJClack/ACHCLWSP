# One binary per app: wetwaresuite-image, wetwaresuite-model, ... all from the
# same main.cpp. Each is compiled with WW_TOOL_<NAME> for its own tools and links
# only those tools' libraries, so it stays small. They land together in
# <build>/bin so they find each other (app.installed, app.send).
function(wetwaresuite_tool_target tool out)
  if(tool STREQUAL "toolkit")
    set(${out} wetwaresuite_toolkit PARENT_SCOPE)
  elseif(tool STREQUAL "audio")
    set(${out} wetwaresuite::audio_tool PARENT_SCOPE)
  elseif(tool STREQUAL "pdf")
    set(${out} wetwaresuite::pdf_tool PARENT_SCOPE)
  elseif(tool STREQUAL "scene")
    set(${out} wetwaresuite::levels PARENT_SCOPE)
  else()
    set(${out} wetwaresuite::${tool} PARENT_SCOPE)
  endif()
endfunction()

foreach(app IN LISTS WETWARESUITE_ENABLED_APPS)
  set(target wetwaresuite-${app})
  add_executable(${target} main.cpp)
  wetwaresuite_add_icon(${target} ${app})
  set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
  target_compile_definitions(${target} PRIVATE WW_APP_ID="${app}")
  target_link_libraries(${target} PRIVATE wetwaresuite::apps)
  foreach(tool IN LISTS WETWARESUITE_APP_TOOLS_${app})
    string(TOUPPER "${tool}" up)
    wetwaresuite_tool_target(${tool} lib)
    target_compile_definitions(${target} PRIVATE WW_TOOL_${up}=1)
    target_link_libraries(${target} PRIVATE ${lib})
  endforeach()
  install(TARGETS ${target} RUNTIME DESTINATION bin)

  if(WETWARESUITE_BUILD_TESTS)
    string(REPLACE ";" "," tools "${WETWARESUITE_APP_TOOLS_${app}}")
    add_test(NAME app_${app}_lists_only_its_tools
             COMMAND ${CMAKE_COMMAND} -DBIN=$<TARGET_FILE:${target}> -DTOOLS=${tools}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/check_app.cmake)
  endif()
endforeach()
