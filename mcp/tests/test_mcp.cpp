#include <nlohmann/json.hpp>

#include "wetwaresuite/mcp/server.hpp"
#include "check.hpp"

using namespace wetwaresuite;
using nlohmann::json;

namespace {

class SetAction : public Action {
public:
    SetAction(std::string& target, std::string value) : target_(target), value_(std::move(value)) {}
    Result apply() override {
        previous_ = target_;
        target_ = value_;
        return Result::success("set to " + value_);
    }
    void revert() override { target_ = previous_; }

private:
    std::string& target_;
    std::string value_;
    std::string previous_;
};

json call(mcp::Server& server, const json& request) {
    auto response = server.handle(request.dump());
    return response ? json::parse(*response) : json();
}

}  // namespace

int main() {
    std::string title = "untitled";
    Session session;
    session.registry().add(
        {"doc.set_title", "Set title", "renames the document", {{"title", ParamType::String, "new title", true}}, true},
        [&title](const Params& p) { return std::make_unique<SetAction>(title, std::get<std::string>(p.at("title"))); });

    mcp::Server server(session, "test", "0.0.0");

    json init = call(server, {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
                              {"params", {{"protocolVersion", "2025-06-18"}}}});
    CHECK(init["result"]["protocolVersion"] == "2025-06-18");
    CHECK(init["result"]["serverInfo"]["name"] == "test");

    CHECK(!server.handle(R"({"jsonrpc":"2.0","method":"notifications/initialized"})").has_value());

    json list = call(server, {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}});
    const auto& tools = list["result"]["tools"];
    CHECK(tools.size() == 5);
    CHECK(tools[0]["name"] == "doc_set_title");
    CHECK(tools[0]["inputSchema"]["required"][0] == "title");

    json set = call(server, {{"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/call"},
                             {"params", {{"name", "doc_set_title"}, {"arguments", {{"title", "plan"}}}}}});
    CHECK(set["result"]["isError"] == false);
    CHECK(title == "plan");

    json undo = call(server, {{"jsonrpc", "2.0"}, {"id", 4}, {"method", "tools/call"}, {"params", {{"name", "undo"}}}});
    CHECK(undo["result"]["isError"] == false);
    CHECK(title == "untitled");

    json bad = call(server, {{"jsonrpc", "2.0"}, {"id", 5}, {"method", "tools/call"},
                             {"params", {{"name", "doc_set_title"}, {"arguments", {{"title", true}}}}}});
    CHECK(bad["result"]["isError"] == true);

    json unknown = call(server, {{"jsonrpc", "2.0"}, {"id", 6}, {"method", "nope"}});
    CHECK(unknown["error"]["code"] == -32601);

    json parse = json::parse(*server.handle("{not json"));
    CHECK(parse["error"]["code"] == -32700);

    // the fast tools/list text is byte for byte what nlohmann builds, edge cases included
    {
        Session odd;
        CommandInfo info{"odd.case", "Quote \" back\\slash \t tab", std::string("ctrl \x01\x1f del \x7f caf\xC3\xA9 \xE2\x9C\x93 / end"),
                         {{"zeta", ParamType::Number, "a number", false, {}, 0.1},
                          {"alpha", ParamType::NumberList, "list\nline", true, {}, NumberList{1, 2.5, -3e-7}},
                          {"mid", ParamType::String, "choices", false, {"a\"b", "\xC3\xA9t\xC3\xA9"}, std::string("a\"b")},
                          {"flag", ParamType::Bool, "", false, {}, true},
                          {"count", ParamType::Int, "count", true, {}, std::int64_t{-42}},
                          {"count", ParamType::Int, "repeated name", false},
                          {"big", ParamType::Number, "big", false, {}, 1e300}},
                         false, true};
        odd.registry().add(info, [](const Params&) { return std::make_unique<FunctionAction>([] { return Result::success("ok"); }); });
        odd.registry().add({"odd.empty", "", "", {}, true}, [](const Params&) { return std::make_unique<FunctionAction>([] { return Result::success("ok"); }); });
        CHECK(mcp::toolsListJson(odd.registry()) == mcp::toolsListJsonReference(odd.registry()));
        CHECK(mcp::toolsListJson(session.registry()) == mcp::toolsListJsonReference(session.registry()));
        mcp::Server oddServer(odd, "odd", "1");
        const json expected = {{"jsonrpc", "2.0"}, {"id", "seven"}, {"result", {{"tools", json::parse(mcp::toolsListJsonReference(odd.registry()))}}}};
        const std::string req = json{{"jsonrpc", "2.0"}, {"id", "seven"}, {"method", "tools/list"}}.dump();
        CHECK(*oddServer.handle(req) == expected.dump());
        CHECK(*oddServer.handle(req) == expected.dump());  // cached
        // a command added later shows up (the cache follows the registry)
        odd.registry().add({"odd.later", "Later", "added after the first list", {}, true},
                           [](const Params&) { return std::make_unique<FunctionAction>([] { return Result::success("later"); }); });
        CHECK(json::parse(*oddServer.handle(req))["result"]["tools"].size() == 7);
        json later = call(oddServer, {{"jsonrpc", "2.0"}, {"id", 8}, {"method", "tools/call"}, {"params", {{"name", "odd_later"}}}});
        CHECK(later["result"]["content"][0]["text"] == "later");
        json missing = call(oddServer, {{"jsonrpc", "2.0"}, {"id", 9}, {"method", "tools/call"}, {"params", {{"name", "odd_nope"}}}});
        CHECK(missing["error"]["code"] == -32602);
    }

    // doc= may be left out (the session fills in the one used last), so the schema doesn't demand it
    {
        Session docs;
        docs.registry().add({"note.read", "Read", "reads a note", {{"doc", ParamType::String, "which note", true}}, false},
                            [](const Params&) { return std::make_unique<FunctionAction>([] { return Result::success("read"); }); });
        mcp::Server docServer(docs, "docs", "0.0.0");
        json l = call(docServer, {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}});
        json schema;
        for (const auto& t : l["result"]["tools"]) {
            if (t["name"] == "note_read") schema = t["inputSchema"];
        }
        CHECK(schema["required"].empty());
        CHECK(schema["properties"]["doc"]["description"].get<std::string>().find("used last") != std::string::npos);
    }

    // a big registry lists find_tools / describe_tools / run_tool instead of every command
    {
        Session big;
        std::string last;
        auto add = [&](const std::string& id, const std::string& title, const std::string& description) {
            big.registry().add({id, title, description, {{"size", ParamType::Int, "pixels", true}}, true},
                               [&last, id](const Params& p) {
                                   const auto size = std::get<std::int64_t>(p.at("size"));
                                   return std::make_unique<FunctionAction>([&last, id, size] {
                                       last = id + " " + std::to_string(size);
                                       return Result::success("ran " + id);
                                   });
                               });
        };
        add("photo.resize", "Resize", "Change an image's size in pixels. Keeps the aspect ratio.");
        add("photo.crop", "Crop", "Cut an image down to a rectangle.");
        add("cad.fillet", "Fillet", "Round the edges of a part.");
        for (int i = 0; i < 160; ++i) add("filler.cmd" + std::to_string(i), "Filler", "padding so the registry is big");

        mcp::Server autoServer(big, {"big", "0", "", mcp::ToolMode::Auto});
        CHECK(autoServer.catalog());
        json init = call(autoServer, {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}, {"params", json::object()}});
        const std::string instructions = init["result"]["instructions"];
        CHECK(instructions.find("find_tools") != std::string::npos);
        CHECK(instructions.find("photo (2)") != std::string::npos);
        json l = call(autoServer, {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}});
        CHECK(l["result"]["tools"].size() == 7);
        CHECK(l["result"]["tools"][0]["name"] == "find_tools");

        auto tool = [&](const std::string& name, const json& args) {
            return call(autoServer, {{"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/call"},
                                     {"params", {{"name", name}, {"arguments", args}}}});
        };
        json found = tool("find_tools", {{"query", "resize images"}});
        CHECK(found["result"]["isError"] == false);
        CHECK(found["result"]["structuredContent"]["tools"][0]["name"] == "photo_resize");
        // the first sentence is enough to choose
        CHECK(found["result"]["structuredContent"]["tools"][0]["description"] == "Change an image's size in pixels.");
        json family = tool("find_tools", {{"query", "photo"}});
        CHECK(family["result"]["structuredContent"]["total"] == 2);
        json limited = tool("find_tools", {{"query", "filler"}, {"limit", 5}});
        CHECK(limited["result"]["structuredContent"]["tools"].size() == 5);
        CHECK(tool("find_tools", {{"query", "zebra"}})["result"]["isError"] == true);

        json described = tool("describe_tools", {{"names", "photo_crop, cad_fillet,nope"}});
        CHECK(described["result"]["structuredContent"]["tools"].size() == 2);
        CHECK(described["result"]["structuredContent"]["tools"][1]["inputSchema"]["required"][0] == "size");
        CHECK(described["result"]["content"][0]["text"].get<std::string>().find("not found: nope") != std::string::npos);

        json ran = tool("run_tool", {{"name", "cad_fillet"}, {"arguments", {{"size", 3}}}});
        CHECK(ran["result"]["isError"] == false);
        CHECK(last == "cad.fillet 3");
        CHECK(tool("run_tool", {{"name", "nope"}})["result"]["isError"] == true);
        CHECK(tool("run_tool", {{"name", "run_tool"}})["result"]["isError"] == true);
        // commands still answer to their own names
        CHECK(tool("photo_resize", {{"size", 64}})["result"]["isError"] == false);
        CHECK(last == "photo.resize 64");
        CHECK(tool("undo", json::object())["result"]["isError"] == false);

        mcp::Server allServer(big, {"big", "0", "", mcp::ToolMode::All});
        CHECK(!allServer.catalog());
        CHECK(call(allServer, {{"jsonrpc", "2.0"}, {"id", 4}, {"method", "tools/list"}})["result"]["tools"].size() == 167);

        // small registries keep listing everything under Auto; --catalog forces it
        mcp::Server smallAuto(session, {"s", "0", "", mcp::ToolMode::Auto});
        CHECK(!smallAuto.catalog());
        mcp::Server smallCatalog(session, {"s", "0", "", mcp::ToolMode::Catalog});
        CHECK(smallCatalog.catalog());

        std::vector<std::string> args = {"--http", "--catalog", "--port", "9"};
        CHECK(mcp::takeToolModeArgs(args) == mcp::ToolMode::Catalog);
        CHECK(args.size() == 3);
        std::vector<std::string> none = {"--http"};
        CHECK(mcp::takeToolModeArgs(none) == mcp::ToolMode::Auto);
    }

    return check::summary("mcp");
}
