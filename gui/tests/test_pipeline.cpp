// The pipeline builder's model: blocks to JSON and back, drag to reorder. No window.
#include "check.hpp"
#include "wetwaresuite/gui/pipeline.hpp"

using namespace wetwaresuite::gui;

int main() {
    PipeModel m;
    m.name = "web copies";
    m.forEach = true;
    m.glob = "photos/*.png";
    m.blocks.push_back({"convert.file", {{"input", "$file"}, {"output", "out/$stem.jpg"}, {"quality", ""}}, "png", false, false});
    m.blocks.push_back({"library.tag", {{"paths", "out/$stem.jpg"}, {"tags", "web"}}, "", true, true});
    m.blocks.push_back({"library.index", {{"root", "out"}}, "", false, false});
    const std::string json = pipelineJson(m);
    CHECK(json.find("\"for_each\": \"photos/*.png\"") != std::string::npos);
    CHECK(json.find("\"quality\"") == std::string::npos);  // empty settings mean the default
    CHECK(json.find("\"ext\": \"png\"") != std::string::npos);
    CHECK(json.find("builder_off") != std::string::npos);  // the switched-off block is kept, not run
    CHECK(json.find("\"on_error\": \"continue\"") != std::string::npos);

    PipeModel back;
    std::string err;
    CHECK(parsePipelineJson(json, back, err));
    CHECK(back.name == "web copies" && back.forEach && back.glob == "photos/*.png");
    CHECK(back.blocks.size() == 3);
    CHECK(back.blocks[1].command == "library.tag" && back.blocks[1].off && back.blocks[1].keepGoing);  // back in its place
    CHECK(back.blocks[0].ext == "png" && back.blocks[0].args.size() == 2);
    CHECK(pipelineJson(back) == json);

    // drag and drop
    CHECK(moveBlock(back, 2, 0) == 0);
    CHECK(back.blocks[0].command == "library.index" && back.blocks[1].command == "convert.file");
    CHECK(moveBlock(back, 0, 99) == 2 && back.blocks[2].command == "library.index");

    // pipelines the builder can't show are refused with a reason, not mangled
    CHECK(!parsePipelineJson("{\"steps\": [{\"run\": \"photo.new width=5\"}]}", back, err) && !err.empty());
    CHECK(!parsePipelineJson("steps:\n  - mkdir: out\n", back, err));
    CHECK(parsePipelineJson("{\"steps\": [{\"command\": \"convert.file\", \"args\": {\"width\": 64}}]}", back, err));
    CHECK(!back.forEach && back.blocks[0].args[0].second == "64");
    return check::summary("gui_pipeline");
}
