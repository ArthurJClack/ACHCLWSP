# the pdf tool (pdf.* commands); libs/pdf is the PDF library it is built on
add_library(wetwaresuite_pdf_tool src/doc.cpp src/save.cpp src/commands.cpp src/cmd_edit.cpp src/cmd_pages.cpp src/cmd_sign.cpp src/cmd_ocr.cpp)
add_library(wetwaresuite::pdf_tool ALIAS wetwaresuite_pdf_tool)
target_include_directories(wetwaresuite_pdf_tool PUBLIC include)
target_link_libraries(wetwaresuite_pdf_tool PUBLIC wetwaresuite::core PRIVATE wetwaresuite::pdf wetwaresuite::pdfdoc wetwaresuite::raster)

if(WETWARESUITE_BUILD_TESTS)
  add_executable(test_pdf_tool tests/test_pdf_tool.cpp)
  target_link_libraries(test_pdf_tool PRIVATE wetwaresuite::pdf_tool wetwaresuite::pdf wetwaresuite::raster wetwaresuite::mcp nlohmann_json::nlohmann_json wetwaresuite_check)
  add_test(NAME pdf_tool COMMAND test_pdf_tool)
endif()
