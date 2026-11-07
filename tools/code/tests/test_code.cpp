// code: languages, highlighting, outlines, error parsing, editing commands with
// undo, search across files, running with installed toolchains, git, and MCP.
#include <chrono>
#include <filesystem>
#include <thread>

#include <nlohmann/json.hpp>

#include "check.hpp"
#include "wetwaresuite/code/code.hpp"
#include "wetwaresuite/core/process.hpp"
#include "wetwaresuite/mcp/server.hpp"
#include "wetwaresuite/sysutil/sysutil.hpp"

using namespace wetwaresuite;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

std::string field(const Result& r, const char* key) {
    const Value* v = r.field(key);
    return v ? toText(*v) : std::string();
}

bool hasSpan(const std::vector<code::Span>& spans, int line, int col, code::Token kind) {
    for (const auto& s : spans) {
        if (s.line == line && s.col == col && s.kind == kind) return true;
    }
    return false;
}

}  // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / ("ww-code-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);

    // ---- languages ----
    {
        CHECK(code::detectLanguage("main.cpp").id == "cpp");
        CHECK(code::detectLanguage("lib.RS").id == "rust");
        CHECK(code::detectLanguage("Makefile").id == "makefile");
        CHECK(code::detectLanguage("CMakeLists.txt").id == "cmake");
        CHECK(code::detectLanguage("script", "#!/usr/bin/env python3\nprint(1)").id == "python");
        CHECK(code::detectLanguage("run", "#!/bin/bash\n").id == "shell");
        CHECK(code::detectLanguage("notes.unknownext").id == "text");
        CHECK(code::languages().size() >= 35);
        CHECK(code::languageById("typescript") != nullptr);
    }

    // ---- highlighting ----
    {
        const auto& cpp = *code::languageById("cpp");
        auto spans = code::highlight("#include <x>\nint main() { return 42; } // hi\nconst char* s = \"a\\\"b\";", cpp);
        CHECK(hasSpan(spans, 0, 0, code::Token::Preproc));
        CHECK(hasSpan(spans, 1, 0, code::Token::Type));
        CHECK(hasSpan(spans, 1, 4, code::Token::Function));
        CHECK(hasSpan(spans, 1, 13, code::Token::Keyword));
        CHECK(hasSpan(spans, 1, 20, code::Token::Number));
        CHECK(hasSpan(spans, 1, 26, code::Token::Comment));
        CHECK(hasSpan(spans, 2, 16, code::Token::String));
        // block comments and strings across lines
        auto py = code::highlight("x = \"\"\"doc\nstill doc\"\"\"\ny = 1  # c", *code::languageById("python"));
        CHECK(hasSpan(py, 1, 0, code::Token::String));
        CHECK(hasSpan(py, 2, 7, code::Token::Comment));
        auto md = code::highlight("# Title\nsome `code` and [a link](x)", *code::languageById("markdown"));
        CHECK(hasSpan(md, 0, 0, code::Token::Heading));
        auto html = code::highlightHtml("fn main() {}", *code::languageById("rust"));
        CHECK(html.find("<span") != std::string::npos && html.find("fn") != std::string::npos);
        CHECK(code::highlightAnsi("let x = 1;", *code::languageById("javascript")).find("\x1b[") != std::string::npos);
        // CRLF text: the \r is not part of any span
        auto crlf = code::highlight("int a;\r\nint b;\r\n", cpp);
        CHECK(hasSpan(crlf, 1, 0, code::Token::Type));
    }

    // ---- outlines ----
    {
        auto syms = code::outline("namespace n {\nclass Widget {\n};\nint add(int a, int b) {\n  return a + b;\n}\n}", *code::languageById("cpp"));
        bool cls = false, fn = false;
        for (const auto& s : syms) {
            if (s.name == "Widget" && s.kind == "class" && s.line == 2) cls = true;
            if (s.name == "add" && s.kind == "function" && s.line == 4) fn = true;
        }
        CHECK(cls && fn);
        auto py = code::outline("class A:\n    def f(self):\n        pass\ndef g():\n    pass\n", *code::languageById("python"));
        CHECK(py.size() == 3 && py[1].name == "f" && py[1].depth == 1);
        auto md = code::outline("# One\ntext\n## Two\n", *code::languageById("markdown"));
        CHECK(md.size() == 2 && md[1].name == "Two" && md[1].depth == 1);
        auto rs = code::outline("pub struct S;\nimpl S {\n    pub fn new() -> Self { S }\n}\nfn main() {}\n", *code::languageById("rust"));
        CHECK(rs.size() >= 3);
    }

    // ---- problems in output ----
    {
        auto gcc = code::parseProblems("src/a.cpp:12:5: error: 'x' was not declared\nsrc/a.cpp:3:1: warning: unused\n");
        CHECK(gcc.size() == 2 && gcc[0].line == 12 && gcc[0].col == 5 && gcc[0].severity == "error" && gcc[1].severity == "warning");
        auto msvc = code::parseProblems("C:\\p\\main.cpp(7,10): error C2065: 'y': undeclared identifier\n");
        CHECK(msvc.size() == 1 && msvc[0].line == 7 && msvc[0].col == 10);
        auto rust = code::parseProblems("error[E0425]: cannot find value `z`\n --> src/main.rs:4:13\n");
        CHECK(rust.size() == 1 && rust[0].file.find("main.rs") != std::string::npos && rust[0].line == 4);
        auto py = code::parseProblems("Traceback (most recent call last):\n  File \"t.py\", line 3, in <module>\n    1/0\nZeroDivisionError: division by zero\n");
        CHECK(!py.empty() && py.back().line == 3 && py.back().message.find("ZeroDivision") != std::string::npos);
        auto args = code::splitArgs("a \"b c\" 'd e' C:\\dir\\x");
        CHECK(args.size() == 4 && args[1] == "b c" && args[2] == "d e" && args[3] == "C:\\dir\\x");  // backslashes stay (Windows paths)
    }

    Session s;
    code::registerCommands(s);

    // ---- editing with undo ----
    {
        const fs::path file = dir / "hello.py";
        sysutil::writeFile(file, "def greet(name):\r\n    return 'hi ' + name\r\n\r\nprint(greet('you'))\r\n");
        Result o = s.run("code.open", {{"path", sysutil::fromPath(file)}});
        CHECK(o.ok && field(o, "doc") == "code1" && field(o, "language") == "python" && field(o, "eol") == "crlf");
        CHECK(!s.run("code.open", {{"path", sysutil::fromPath(dir)}}).ok);  // folders are for code.project

        Result e = s.run("code.edit", {{"doc", std::string("code1")}, {"old", std::string("'hi '")}, {"new", std::string("'hello '")}});
        CHECK(e.ok);
        Result r = s.run("code.read", {{"doc", std::string("code1")}, {"start", std::int64_t{2}}, {"end", std::int64_t{2}}});
        CHECK(r.ok && r.message == "2\t    return 'hello ' + name\n");
        CHECK(!s.run("code.edit", {{"doc", std::string("code1")}, {"old", std::string("nothing like this")}, {"new", std::string("x")}}).ok);
        CHECK(s.run("code.insert", {{"doc", std::string("code1")}, {"line", std::int64_t{1}}, {"text", std::string("import sys\n")}}).ok);
        CHECK(s.run("code.replace_lines", {{"doc", std::string("code1")}, {"start", std::int64_t{4}}, {"end", std::int64_t{4}}}).ok);  // the blank line
        Result f = s.run("code.find", {{"doc", std::string("code1")}, {"find", std::string("GREET")}});
        CHECK(f.ok && field(f, "count") == "2");
        CHECK(s.run("code.find", {{"doc", std::string("code1")}, {"find", std::string("GREET")}, {"match_case", true}}).value == Value(std::int64_t{0}));
        Result rep = s.run("code.replace", {{"doc", std::string("code1")}, {"find", std::string("greet\\((\\w+)\\)")}, {"replace", std::string("welcome($1)")}, {"regex", true}});
        CHECK(rep.ok && field(rep, "count") == "1");
        Result all = s.run("code.read", {{"doc", std::string("code1")}, {"raw", true}});
        CHECK(field(all, "text") == "import sys\ndef welcome(name):\n    return 'hello ' + name\nprint(greet('you'))\n");
        // undo walks back through the edits
        CHECK(s.history().undo());
        CHECK(field(s.run("code.read", {{"doc", std::string("code1")}}), "text").find("def greet(name)") != std::string::npos);
        CHECK(s.history().redo());
        Result ol = s.run("code.outline", {{"doc", std::string("code1")}});
        CHECK(ol.ok && field(ol, "rows").find("welcome") != std::string::npos);
        Result g = s.run("code.goto", {{"doc", std::string("code1")}, {"line", std::int64_t{3}}, {"context", std::int64_t{0}}});
        CHECK(g.ok && field(g, "text") == "    return 'hello ' + name");
        Result hl = s.run("code.highlight", {{"doc", std::string("code1")}});
        CHECK(hl.ok && std::get<NumberList>(*hl.field("spans")).size() % 4 == 0 && !std::get<NumberList>(*hl.field("spans")).empty());
        // save keeps CRLF
        CHECK(s.run("code.save", {{"doc", std::string("code1")}}).ok);
        std::string saved = sysutil::readFile(file);
        CHECK(saved.find("def welcome(name):\r\n") != std::string::npos && saved.find("\n\n") == std::string::npos);
        Result files = s.run("code.files", {});
        CHECK(files.ok && field(files, "count") == "1" && field(files, "rows").find("modified") == std::string::npos);
        // close and undo it
        CHECK(s.run("code.close", {{"doc", std::string("code1")}}).ok);
        CHECK(!s.workspace().get("code1"));
        CHECK(s.history().undo());
        CHECK(s.workspace().get("code1"));
        // a new file, saved under a name, picks up its language
        Result n = s.run("code.new", {{"text", std::string("fn main() {}\n")}});
        CHECK(n.ok && field(n, "language") == "text");
        CHECK(s.run("code.save", {{"doc", field(n, "doc")}, {"path", sysutil::fromPath(dir / "proj" / "main.rs")}}).ok);
        CHECK(field(s.run("code.read", {{"doc", field(n, "doc")}}), "language") == "rust");
        CHECK(s.run("code.set_text", {{"doc", field(n, "doc")}, {"text", std::string("a\r\nb")}}).ok);
        CHECK(field(s.run("code.read", {{"doc", field(n, "doc")}}), "text") == "a\nb");
    }

    // ---- folders: tree, search, replace in files, count ----
    {
        const fs::path proj = dir / "proj";
        fs::create_directories(proj / "src");
        fs::create_directories(proj / "node_modules" / "dep");
        sysutil::writeFile(proj / "src" / "a.cpp", "int alpha = 1;\nint beta = alpha + 1;\n");
        sysutil::writeFile(proj / "src" / "b.h", "// alpha header\nextern int alpha;\n");
        sysutil::writeFile(proj / "node_modules" / "dep" / "x.js", "var alpha;\n");
        sysutil::writeFile(proj / "logo.bin", std::string("\0alpha\0", 7));
        Result pr = s.run("code.project", {{"folder", sysutil::fromPath(proj)}});
        CHECK(pr.ok && field(pr, "files") == "4");
        Result t = s.run("code.tree", {});
        CHECK(t.ok && t.message.find("src/") != std::string::npos && t.message.find("a.cpp") != std::string::npos);
        Result sr = s.run("code.search", {{"find", std::string("alpha")}, {"whole_word", true}});
        CHECK(sr.ok && field(sr, "count") == "4" && field(sr, "files") == "2");  // skips node_modules and the binary
        CHECK(field(s.run("code.search", {{"find", std::string("alpha")}, {"files", std::string("*.h")}}), "count") == "2");
        Result dry = s.run("code.replace_in_files", {{"find", std::string("alpha")}, {"replace", std::string("gamma")}, {"dry_run", true}});
        CHECK(dry.ok && field(dry, "count") == "4" && sysutil::readFile(proj / "src" / "a.cpp").find("alpha") != std::string::npos);
        Result rf = s.run("code.replace_in_files", {{"find", std::string("alpha")}, {"replace", std::string("gamma")}, {"files", std::string("*.cpp")}});
        CHECK(rf.ok && sysutil::readFile(proj / "src" / "a.cpp") == "int gamma = 1;\nint beta = gamma + 1;\n");
        CHECK(s.history().undo());
        CHECK(sysutil::readFile(proj / "src" / "a.cpp") == "int alpha = 1;\nint beta = alpha + 1;\n");
        Result c = s.run("code.count", {});
        CHECK(c.ok && c.message.find("C++") != std::string::npos && field(c, "lines") != "0");
    }

    // ---- running with what is installed ----
    {
        Result tc = s.run("code.toolchains", {});
        CHECK(tc.ok);
        Result langs = s.run("code.languages", {});
        CHECK(langs.ok && langs.message.find("rust") != std::string::npos);
        const bool havePython = findProgram("python3") || findProgram("python");
        if (havePython) {
            const fs::path py = dir / "ask.py";
            sysutil::writeFile(py, "import sys\nname = input()\nprint('hello', name)\nsys.exit(2 if name == 'bad' else 0)\n");
            Result r = s.run("code.run", {{"path", sysutil::fromPath(py)}, {"input", std::string("world\n")}});
            CHECK(r.ok && field(r, "output").find("hello world") != std::string::npos && field(r, "exit_code") == "0");
            Result bad = s.run("code.run", {{"path", sysutil::fromPath(py)}, {"input", std::string("bad\n")}});
            CHECK(!bad.ok && field(bad, "exit_code") == "2");
            // a traceback becomes a clickable problem
            sysutil::writeFile(dir / "boom.py", "x = 1\ny = x / 0\n");
            Result boom = s.run("code.run", {{"path", sysutil::fromPath(dir / "boom.py")}});
            CHECK(!boom.ok && field(boom, "problems").find("boom.py\t2\t") != std::string::npos);
            // in the background: start, type into it, read the output
            Result bg = s.run("code.run", {{"path", sysutil::fromPath(py)}, {"wait", false}});
            CHECK(bg.ok && field(bg, "running") == "true");
            const std::string job = field(bg, "job");
            CHECK(s.run("code.job_input", {{"job", job}, {"text", std::string("there")}}).ok);
            Result out;
            for (int i = 0; i < 200; ++i) {
                out = s.run("code.job_output", {{"job", job}});
                if (field(out, "running") == "false") break;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            CHECK(field(out, "output").find("hello there") != std::string::npos);
            // stopping a long one
            sysutil::writeFile(dir / "slow.py", "import time\ntime.sleep(30)\n");
            Result slow = s.run("code.run", {{"path", sysutil::fromPath(dir / "slow.py")}, {"wait", false}});
            CHECK(s.run("code.job_stop", {{"job", field(slow, "job")}}).ok);
            CHECK(field(s.run("code.job_output", {{"job", field(slow, "job")}}), "running") == "false");
            CHECK(field(s.run("code.jobs", {}), "rows").find("stopped") != std::string::npos);
        }
        const bool haveCxx = findProgram("g++") || findProgram("clang++") || findProgram("cl");
        if (haveCxx) {
            sysutil::writeFile(dir / "add.cpp", "#include <cstdio>\nint main(int argc, char**) { std::printf(\"args %d\\n\", argc - 1); }\n");
            Result r = s.run("code.run", {{"path", sysutil::fromPath(dir / "add.cpp")}, {"args", std::string("a b")}, {"timeout", 120.0}});
            CHECK(r.ok && field(r, "output").find("args 2") != std::string::npos);
            // a compiled program runs as it is
            fs::path exe = fs::temp_directory_path() / "wetwaresuite-run" / "add";
#ifdef _WIN32
            exe += ".exe";
#endif
            if (fs::exists(exe)) {
                Result direct = s.run("code.run", {{"path", sysutil::fromPath(exe)}, {"args", std::string("x")}});
                CHECK(direct.ok && field(direct, "output").find("args 1") != std::string::npos);
            }
            sysutil::writeFile(dir / "broken.cpp", "int main() { return undefined_thing; }\n");
            Result b = s.run("code.run", {{"path", sysutil::fromPath(dir / "broken.cpp")}, {"timeout", 120.0}});
            CHECK(!b.ok && field(b, "errors") != "0" && field(b, "problems").find("broken.cpp\t1\t") != std::string::npos);
        }
        if (findProgram("make")) {
            const fs::path mk = dir / "mk";
            fs::create_directories(mk);
            sysutil::writeFile(mk / "Makefile", "all:\n\t@echo built it\ntest:\n\t@echo tested\n");
            Result b = s.run("code.build", {{"folder", sysutil::fromPath(mk)}});
            CHECK(b.ok && field(b, "output").find("built it") != std::string::npos);
            CHECK(field(s.run("code.build", {{"folder", sysutil::fromPath(mk)}, {"action", std::string("test")}}), "output").find("tested") != std::string::npos);
        }
        CHECK(!s.run("code.build", {{"folder", sysutil::fromPath(dir / "proj" / "src")}}).ok);  // no build file
        Result er = s.run("code.errors", {{"text", std::string("x.c:1:2: error: bad\n")}});
        CHECK(er.ok && field(er, "count") == "1");
    }

    // ---- git ----
    if (findProgram("git")) {
        const fs::path repo = dir / "repo";
        fs::create_directories(repo);
        const std::string folder = sysutil::fromPath(repo);
#ifdef _WIN32
        _putenv_s("GIT_AUTHOR_NAME", "Test");
        _putenv_s("GIT_AUTHOR_EMAIL", "test@example.com");
        _putenv_s("GIT_COMMITTER_NAME", "Test");
        _putenv_s("GIT_COMMITTER_EMAIL", "test@example.com");
#else
        setenv("GIT_AUTHOR_NAME", "Test", 1);
        setenv("GIT_AUTHOR_EMAIL", "test@example.com", 1);
        setenv("GIT_COMMITTER_NAME", "Test", 1);
        setenv("GIT_COMMITTER_EMAIL", "test@example.com", 1);
#endif
        CHECK(!s.run("code.git_status", {{"folder", folder}}).ok);  // not a repository yet
        CHECK(s.run("code.git_init", {{"folder", folder}}).ok);
        sysutil::writeFile(repo / "a.txt", "one\n");
        Result st = s.run("code.git_status", {{"folder", folder}});
        CHECK(st.ok && st.message.find("new (not staged): a.txt") != std::string::npos);
        CHECK(field(st, "branch").find("No commits") == std::string::npos && st.message.find("(no commits yet)") != std::string::npos);
        CHECK(field(s.run("code.git_log", {{"folder", folder}}), "count") == "0");
        CHECK(!s.run("code.git_commit", {{"folder", folder}, {"message", std::string("first")}}).ok);  // nothing staged
        CHECK(s.run("code.git_stage", {{"folder", folder}, {"paths", std::string("a.txt")}}).ok);
        Result c = s.run("code.git_commit", {{"folder", folder}, {"message", std::string("first")}});
        CHECK(c.ok && !field(c, "commit").empty());
        sysutil::writeFile(repo / "a.txt", "one\ntwo\n");
        Result d = s.run("code.git_diff", {{"folder", folder}});
        CHECK(d.ok && field(d, "added") == "1" && d.message.find("+two") != std::string::npos);
        CHECK(s.run("code.git_commit", {{"folder", folder}, {"message", std::string("second")}, {"all", true}}).ok);
        Result lg = s.run("code.git_log", {{"folder", folder}});
        CHECK(lg.ok && field(lg, "count") == "2" && lg.message.find("second") != std::string::npos);
        CHECK(s.run("code.git_switch", {{"folder", folder}, {"branch", std::string("feature")}, {"create", true}}).ok);
        Result br = s.run("code.git_branches", {{"folder", folder}});
        CHECK(br.ok && field(br, "current") == "feature" && field(br, "count") == "2");
        sysutil::writeFile(repo / "b.txt", "b\n");
        CHECK(s.run("code.git_stage", {{"folder", folder}}).ok);
        CHECK(s.run("code.git_unstage", {{"folder", folder}, {"paths", std::string("b.txt")}}).ok);
        CHECK(field(s.run("code.git_status", {{"folder", folder}}), "staged") == "0");

        // remotes, push, fetch, pull and merge against a bare repository on disk (no network)
        CHECK(s.run("code.git_commit", {{"folder", folder}, {"message", std::string("b")}, {"all", true}}).ok);
        const fs::path bare = dir / "server.git";
        CHECK(runProgram({"git", "init", "-q", "--bare", sysutil::fromPath(bare)}).exitCode == 0);
        CHECK(field(s.run("code.git_remotes", {{"folder", folder}}), "count") == "0");
        Result rm = s.run("code.git_remotes", {{"folder", folder}, {"add", std::string("origin")}, {"url", sysutil::fromPath(bare)}});
        CHECK(rm.ok && field(rm, "count") == "1" && field(rm, "rows").rfind("origin\t", 0) == 0);
        Result push = s.run("code.git_push", {{"folder", folder}});
        CHECK(push.ok && push.message.find("now its upstream") != std::string::npos);
        CHECK(s.run("code.git_push", {{"folder", folder}}).message.find("already up to date") != std::string::npos);
        // a second copy pushes a change; the first pulls it
        const fs::path other = dir / "other";
        CHECK(runProgram({"git", "clone", "-q", "-b", "feature", sysutil::fromPath(bare), sysutil::fromPath(other)}).exitCode == 0);
        const std::string otherFolder = sysutil::fromPath(other);
        sysutil::writeFile(other / "c.txt", "from the other copy\n");
        CHECK(s.run("code.git_commit", {{"folder", otherFolder}, {"message", std::string("c")}, {"all", true}}).ok);
        CHECK(s.run("code.git_push", {{"folder", otherFolder}}).ok);
        Result fetched = s.run("code.git_fetch", {{"folder", folder}});
        CHECK(fetched.ok && field(fetched, "behind") == "1" && field(fetched, "ahead") == "0");
        Result pulled = s.run("code.git_pull", {{"folder", folder}});
        CHECK(pulled.ok && field(pulled, "commits") == "1" && fs::exists(repo / "c.txt"));
        CHECK(s.run("code.git_pull", {{"folder", folder}}).message == "already up to date");
        // both change the same line: the push is refused, the pull stops on the conflict
        sysutil::writeFile(other / "a.txt", "theirs\n");
        CHECK(s.run("code.git_commit", {{"folder", otherFolder}, {"message", std::string("theirs")}, {"all", true}}).ok);
        CHECK(s.run("code.git_push", {{"folder", otherFolder}}).ok);
        sysutil::writeFile(repo / "a.txt", "ours\n");
        CHECK(s.run("code.git_commit", {{"folder", folder}, {"message", std::string("ours")}, {"all", true}}).ok);
        Result refused = s.run("code.git_push", {{"folder", folder}});
        CHECK(!refused.ok && refused.message.find("git_pull first") != std::string::npos);
        Result conflict = s.run("code.git_pull", {{"folder", folder}});
        CHECK(conflict.ok && field(conflict, "conflicts") == "1" && field(conflict, "rows") == "a.txt\n");
        CHECK(s.run("code.git_merge", {{"folder", folder}, {"abort", true}}).ok);
        {
            // git puts the file back itself here, so on windows (core.autocrlf) it comes back with \r\n
            std::string back = sysutil::readFile(repo / "a.txt");
            std::erase(back, '\r');
            CHECK(back == "ours\n");
        }
        // merge a local branch in (fast-forward), then resolve a real conflict by hand
        CHECK(s.run("code.git_switch", {{"folder", folder}, {"branch", std::string("side")}, {"create", true}}).ok);
        sysutil::writeFile(repo / "d.txt", "d\n");
        CHECK(s.run("code.git_commit", {{"folder", folder}, {"message", std::string("d")}, {"all", true}}).ok);
        CHECK(s.run("code.git_switch", {{"folder", folder}, {"branch", std::string("feature")}}).ok);
        Result ff = s.run("code.git_merge", {{"folder", folder}, {"branch", std::string("side")}});
        CHECK(ff.ok && ff.message.find("fast-forwarded") != std::string::npos && fs::exists(repo / "d.txt"));
        Result m2 = s.run("code.git_merge", {{"folder", folder}, {"branch", std::string("origin/feature")}});
        CHECK(m2.ok && field(m2, "conflicts") == "1");
        sysutil::writeFile(repo / "a.txt", "both\n");
        CHECK(s.run("code.git_commit", {{"folder", folder}, {"message", std::string("merged")}, {"all", true}}).ok);
        CHECK(s.run("code.git_push", {{"folder", folder}}).ok);
        CHECK(!s.run("code.git_push", {{"folder", folder}, {"remote", std::string("nowhere")}}).ok);
    }

    // ---- language servers (a fake one built with the tests) ----
    {
        const std::string server = std::string("\"") + FAKE_LSP + "\"";
#ifdef _WIN32
        _putenv_s("WETWARESUITE_LSP_PYTHON", server.c_str());
#else
        setenv("WETWARESUITE_LSP_PYTHON", server.c_str(), 1);
#endif
        const fs::path lspDir = dir / "lsp";
        fs::create_directories(lspDir);
        sysutil::writeFile(lspDir / "main.py", "def foo():\n    pass\nfoo()  # TODO\npri");
        Result o = s.run("code.open", {{"path", sysutil::fromPath(lspDir / "main.py")}});
        CHECK(o.ok);
        const std::string doc = field(o, "doc");
        Result dg = s.run("code.diagnostics", {{"doc", doc}});
        CHECK(dg.ok && field(dg, "count") == "1" && field(dg, "warnings") == "1" && field(dg, "fresh") == "true");
        CHECK(field(dg, "rows") == "3\t10\twarning\tsomething left to do\n");
        Result cm = s.run("code.complete", {{"doc", doc}, {"line", std::int64_t{4}}, {"col", std::int64_t{4}}});
        CHECK(cm.ok && field(cm, "count") == "2" && field(cm, "from_col") == "1");
        CHECK(field(cm, "rows").rfind("printf\tfunction\tC style\tprintf\n", 0) == 0);  // sorted by the server's sortText; zebra doesn't start with "pri"
        Result hv = s.run("code.hover", {{"doc", doc}, {"line", std::int64_t{3}}, {"col", std::int64_t{2}}});
        CHECK(hv.ok && hv.message == "hover at 2:1");
        Result df = s.run("code.definition", {{"doc", doc}, {"line", std::int64_t{3}}, {"col", std::int64_t{1}}});
        CHECK(df.ok && field(df, "line") == "1" && field(df, "col") == "5" && field(df, "path").find("main.py") != std::string::npos);
        Result rf = s.run("code.references", {{"doc", doc}, {"line", std::int64_t{1}}, {"col", std::int64_t{5}}});
        CHECK(rf.ok && field(rf, "count") == "2" && field(rf, "rows").find("\t3\t1\tfoo()  # TODO\n") != std::string::npos);
        // a change goes to the server, and the problems follow it
        CHECK(s.run("code.set_text", {{"doc", doc}, {"text", std::string("# TODO one\n# TODO two\n")}}).ok);
        Result dg2 = s.run("code.diagnostics", {{"doc", doc}});
        CHECK(dg2.ok && field(dg2, "count") == "2" && field(dg2, "fresh") == "true");
        Result st = s.run("code.lsp", {});
        CHECK(st.ok && field(st, "rows").find("python\t") != std::string::npos && field(st, "rows").find("\trunning\n") != std::string::npos);
        CHECK(s.run("code.lsp", {{"action", std::string("stop")}}).message == "stopped 1 language server(s)");
        // it starts again when needed
        CHECK(s.run("code.hover", {{"doc", doc}, {"line", std::int64_t{1}}, {"col", std::int64_t{1}}}).ok);
        // unsaved files can't be read by a server
        Result n = s.run("code.new", {{"language", std::string("python")}, {"text", std::string("x = 1\n")}});
        Result un = s.run("code.diagnostics", {{"doc", field(n, "doc")}});
        CHECK(!un.ok && un.message.find("save the file first") != std::string::npos);
        s.run("code.lsp", {{"action", std::string("stop")}});
    }

    // ---- MCP ----
    {
        mcp::Server server(s, {"wetwaresuite", "test", "code"});
        json list = json::parse(*server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})"))["result"]["tools"];
        int seen = 0;
        for (const auto& t : list) {
            if (std::string(t["name"]).rfind("code_", 0) == 0) ++seen;
        }
        CHECK(seen == 51);  // incl. merge, remotes, fetch, pull, push and the language server ones
        json call = {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/call"},
                     {"params", {{"name", "code_new"}, {"arguments", {{"language", "go"}, {"text", "package main\nfunc main() {}\n"}}}}}};
        json res = json::parse(*server.handle(call.dump()))["result"];
        CHECK(res["isError"] == false && res["structuredContent"]["language"] == "go");
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
    return check::summary("code");
}
