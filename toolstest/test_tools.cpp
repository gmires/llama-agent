#include "../src/tools.h"
#include "../src/permissions.h"

#include <iostream>
#include <cassert>
#include <string>
#include <fstream>
#include <cstdio>

static int tests = 0, passed = 0;

#define CHECK(cond, msg) do { \
    tests++; \
    if (!(cond)) { \
        std::cerr << "  FAIL [" << __LINE__ << "] " << msg << ": " << #cond << "\n"; \
    } else { \
        passed++; \
    } \
} while(0)

static void test_parse_tool_call()
{
    std::cout << "[Parser JSON tool call]\n";

    ToolRegistry reg;

    // Formato base: {"tool": "bash", "args": {"command": "ls"}}
    std::string name;
    std::map<std::string, std::string> args;

    CHECK(reg.parse_tool_call("{\"tool\": \"bash\", \"args\": {\"command\": \"ls\"}}", name, args),
          "parse base json");
    CHECK(name == "bash", "tool name");
    CHECK(args["command"] == "ls", "arg command");

    // Formato con backtick: {"tool": "write", "args": {"path": "test.txt", "content": "hello"}}
    name.clear(); args.clear();
    CHECK(reg.parse_tool_call("```json\n{\"tool\": \"write\", \"args\": {\"path\": \"x.txt\", \"content\": \"hello\"}}\n```", name, args),
          "parse json in code block");
    CHECK(name == "write", "tool name write");
    CHECK(args["path"] == "x.txt", "arg path");

    // Escape sequences: \n → newline
    name.clear(); args.clear();
    CHECK(reg.parse_tool_call("{\"tool\": \"write\", \"args\": {\"path\": \"y.txt\", \"content\": \"line1\\nline2\"}}", name, args),
          "parse with \\n escape");
    CHECK(args["content"] == "line1\nline2", "unescape \\n");
    CHECK(args["content"].size() == 11, "unescape correct length");

    // Escape sequences: \t
    name.clear(); args.clear();
    CHECK(reg.parse_tool_call("{\"tool\": \"write\", \"args\": {\"content\": \"col1\\tcol2\"}}", name, args),
          "parse with \\t escape");
    CHECK(args["content"] == "col1\tcol2", "unescape \\t");

    // Escape sequences: \\ and \"
    name.clear(); args.clear();
    CHECK(reg.parse_tool_call("{\"tool\": \"write\", \"args\": {\"content\": \"path\\\\to\\\\file\"}}", name, args),
          "parse with \\\\ escape");
    CHECK(args["content"] == "path\\to\\file", "unescape \\\\");

    // Funzione con "function" e "arguments" (formato OpenAI)
    name.clear(); args.clear();
    CHECK(reg.parse_tool_call("{\"function\": \"bash\", \"arguments\": {\"command\": \"echo hi\"}}", name, args),
          "parse function/arguments format");
    CHECK(name == "bash", "function name");
    CHECK(args["command"] == "echo hi", "function args");

    // Nessuna tool call (testo normale)
    name.clear(); args.clear();
    CHECK(!reg.parse_tool_call("Ciao, come posso aiutarti?", name, args),
          "no tool call in normal text");

    // Tool call con testo circostante
    name.clear(); args.clear();
    CHECK(reg.parse_tool_call("Ecco cosa faccio:\n{\"tool\": \"glob\", \"args\": {\"pattern\": \"*.cpp\"}}\nFatto.", name, args),
          "tool call with surrounding text");
    CHECK(name == "glob", "glob name");
    CHECK(args["pattern"] == "*.cpp", "glob pattern");

    // --- Bug 1: JS content con {} (es. { x: 10, y: 10 }) ---
    name.clear(); args.clear();
    CHECK(reg.parse_tool_call(
        "{\"tool\": \"write\", \"args\": {\"path\": \"script.js\", \"content\": \"let snake = [{ x: 10, y: 10 }];\"}}",
        name, args),
        "js with braces");
    CHECK(name == "write", "js braces tool name");
    CHECK(args["path"] == "script.js", "js braces path");
    CHECK(args["content"] == "let snake = [{ x: 10, y: 10 }];", "js braces content");
    CHECK(args.size() == 2, "js braces exactly 2 args");

    // --- Bug 2: JS/HTML content con " (es. getElementById("canvas")) ---    
    name.clear(); args.clear();
    CHECK(reg.parse_tool_call(
        "{\"tool\": \"write\", \"args\": {\"path\": \"app.js\", \"content\": \"document.getElementById(\\\"canvas\\\");\"}}",
        name, args),
        "js with escaped quotes");
    CHECK(args["content"] == "document.getElementById(\"canvas\");", "js quotes unescaped");

    // --- Bug 3: Content con "key": value patterns (falsi match regex) ---
    name.clear(); args.clear();
    CHECK(reg.parse_tool_call(
        "{\"tool\": \"write\", \"args\": {\"path\": \"data.json\", \"content\": \"{\\\"name\\\": \\\"test\\\"}\"}}",
        name, args),
        "json content with inner quotes/colons");
    CHECK(args["content"] == "{\"name\": \"test\"}", "json content preserved");
    CHECK(args["path"] == "data.json", "path still correct after inner json");
    CHECK(args.size() == 2, "exactly 2 args with inner json");

    // --- Scenario realistico: write con JS snake game (full content) ---
    name.clear(); args.clear();
    CHECK(reg.parse_tool_call(
        "```json\n{\"tool\": \"write\", \"args\": {\"path\": \"snake.js\", \"content\": \"const canvas = document.getElementById('gameCanvas');\\nconst ctx = canvas.getContext('2d');\\nlet snake = [{ x: 10, y: 10 }, { x: 9, y: 10 }];\"}}\n```",
        name, args),
        "realistic js snake write call");
    CHECK(name == "write", "snake js tool");
    CHECK(args["path"] == "snake.js", "snake js path");
    CHECK(args["content"].find("const canvas") != std::string::npos, "snake has const canvas");
    CHECK(args["content"].find("{ x: 10, y: 10 }") != std::string::npos, "snake has object literals");

    // --- Malformed: JSON senza colonne (come output dal modello) ---
    // Non deve parseare come tool call valida
    name.clear(); args.clear();
    CHECK(!reg.parse_tool_call(
        "{\"ool\" \"rite\" \"rgs\" {\"ath\" \"cript.js\"}}",
        name, args),
        "malformed json without colons is rejected");

    std::cout << "  " << passed << "/" << tests << " passed\n";
}

static void test_write_tool()
{
    std::cout << "[Tool: write]\n";

    ToolRegistry reg;
    std::string test_file = "/tmp/test_agent_write.txt";
    std::string content = "#!/usr/bin/env python3\n\ndef main():\n    print(\"hello\")\n";

    auto res = reg.execute("write", {{"path", test_file}, {"content", content}});
    CHECK(res.success, "write success");

    // Verifica contenuto
    std::ifstream f(test_file);
    std::string read_content((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
    CHECK(read_content == content, "write content matches");
    CHECK(read_content.find("def main():") != std::string::npos, "write has def main");
    CHECK(read_content.find("\\n") == std::string::npos, "no literal \\n");
    CHECK(read_content.find("#include") == std::string::npos, "no C include");

    std::remove(test_file.c_str());

    std::cout << "  OK\n";
}

static void test_bash_tool()
{
    std::cout << "[Tool: bash]\n";

    ToolRegistry reg;
    auto res = reg.execute("bash", {{"command", "echo hello agent"}});
    CHECK(res.success, "bash echo success");
    CHECK(res.output.find("hello agent") != std::string::npos, "bash output matches");

    std::cout << "  OK\n";
}

static void test_glob_tool()
{
    std::cout << "[Tool: glob]\n";

    ToolRegistry reg;
    auto res = reg.execute("glob", {{"pattern", "*.cpp"}});
    CHECK(res.success, "glob success");
    CHECK(!res.output.empty(), "glob found files");

    std::cout << "  OK\n";
}

static void test_read_tool()
{
    std::cout << "[Tool: read]\n";

    ToolRegistry reg;
    auto res = reg.execute("read", {{"path", "../../CMakeLists.txt"}});
    CHECK(res.success, "read CMakeLists.txt");
    CHECK(res.output.find("cmake_minimum_required") != std::string::npos, "read content");

    res = reg.execute("read", {{"path", "/nonexistent_file_xyz"}});
    CHECK(!res.success, "read nonexistent file fails");

    std::cout << "  OK\n";
}

static void test_grep_tool()
{
    std::cout << "[Tool: grep]\n";

    ToolRegistry reg;
    auto res = reg.execute("grep", {{"pattern", "int main"}, {"path", "../../src"}});
    CHECK(res.success, "grep success");
    if (res.success) {
        bool found = res.output.find("main.cpp") != std::string::npos;
        if (!found) {
            std::cerr << "  grep output: " << res.output.substr(0, 200) << "\n";
        }
        CHECK(found, "grep found file");
    }

    std::cout << "  OK\n";
}

static void test_ls_tool()
{
    std::cout << "[Tool: ls]\n";

    ToolRegistry reg;
    // ls della directory corrente (toolstest/)
    auto res = reg.execute("ls", {{"path", "."}});
    CHECK(res.success, "ls success");
    CHECK(res.output.find("test_tools") != std::string::npos, "ls found test_tools ref");
    CHECK(!res.output.empty(), "ls has output");

    // ls di path inesistente
    res = reg.execute("ls", {{"path", "/nonexistent_xyz"}});
    CHECK(!res.success, "ls nonexistent fails");

    std::cout << "  OK\n";
}

static void test_rm_tool()
{
    std::cout << "[Tool: rm]\n";

    ToolRegistry reg;
    std::string f = "/tmp/test_agent_rm.txt";

    // Crea file temporaneo
    { std::ofstream of(f); of << "delete me"; }

    auto res = reg.execute("rm", {{"path", f}});
    CHECK(res.success, "rm success");
    CHECK(!std::ifstream(f).is_open(), "rm file actually deleted");

    // rm di file inesistente
    res = reg.execute("rm", {{"path", "/tmp/nonexistent_xyz"}});
    CHECK(!res.success, "rm nonexistent fails");

    std::cout << "  OK\n";
}

static void test_mv_tool()
{
    std::cout << "[Tool: mv]\n";

    ToolRegistry reg;
    std::string from = "/tmp/test_agent_mv_src.txt";
    std::string to   = "/tmp/test_agent_mv_dst.txt";

    // Cleanup
    std::remove(from.c_str());
    std::remove(to.c_str());

    // Crea file sorgente
    { std::ofstream of(from); of << "rename me"; }

    auto res = reg.execute("mv", {{"from", from}, {"to", to}});
    CHECK(res.success, "mv success");
    CHECK(!std::ifstream(from).is_open(), "mv source gone");
    CHECK(std::ifstream(to).is_open(), "mv dest exists");

    std::remove(to.c_str());

    // mv di file inesistente
    res = reg.execute("mv", {{"from", "/tmp/nonexistent_xyz"}, {"to", "/tmp/dst"}});
    CHECK(!res.success, "mv nonexistent fails");

    std::cout << "  OK\n";
}

static void test_edit_tool()
{
    std::cout << "[Tool: edit]\n";

    ToolRegistry reg;
    std::string f = "/tmp/test_agent_edit.txt";
    std::string orig = "Hello World\nThis is a test\nGoodbye\n";

    { std::ofstream of(f); of << orig; }

    // Sostituzione unica
    auto res = reg.execute("edit", {{"path", f}, {"old_string", "World"}, {"new_string", "C++"}});
    CHECK(res.success, "edit single replace");
    {
        std::ifstream in(f);
        std::string c((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(c.find("Hello C++") != std::string::npos, "edit content changed");
        CHECK(c.find("World") == std::string::npos, "edit old string gone");
        CHECK(c.find("This is a test") != std::string::npos, "edit rest untouched");
    }

    // Sostituzione con match non unico
    res = reg.execute("edit", {{"path", f}, {"old_string", "is"}, {"new_string", "IS"}});
    CHECK(!res.success, "edit non-unique fails");

    // Sostituzione non trovata
    res = reg.execute("edit", {{"path", f}, {"old_string", "xyz_not_found"}, {"new_string", ""}});
    CHECK(!res.success, "edit not found fails");

    std::remove(f.c_str());
    std::cout << "  OK\n";
}

static void test_find_tool()
{
    std::cout << "[Tool: find]\n";

    ToolRegistry reg;
    // Cerca dal toolstest/ (directory padre del build/)
    auto res = reg.execute("find", {{"pattern", "*.cpp"}, {"path", ".."}});
    CHECK(res.success, "find success");
    CHECK(res.output.find("test_tools.cpp") != std::string::npos, "find found test_tools.cpp");

    // find con brace expansion
    res = reg.execute("find", {{"pattern", "*.{cpp,h}"}, {"path", "../.."}});
    CHECK(res.success, "find brace expansion success");

    // find con type=file verso la root progetto
    res = reg.execute("find", {{"pattern", "CMakeLists.txt"}, {"type", "file"}, {"path", "../.."}});
    CHECK(res.success, "find type=file success");

    std::cout << "  OK\n";
}

static void test_fetch_tool()
{
    std::cout << "[Tool: fetch]\n";

    ToolRegistry reg;
    // Test con URL che esiste e ritorna qualcosa
    auto res = reg.execute("fetch", {{"url", "https://example.com"}, {"timeout", "10"}});
    CHECK(res.success, "fetch example.com");
    if (res.success) {
        CHECK(res.output.find("Example Domain") != std::string::npos, "fetch has content");
    }

    // Test con URL inesistente
    res = reg.execute("fetch", {{"url", "https://invalid.domain.xyz.nonexistent/test"}, {"timeout", "5"}});
    // Fetch di un dominio inesistente può dare success=false o output vuoto
    // Accettiamo entrambi

    std::cout << "  OK\n";
}

static void test_web_search_tool()
{
    std::cout << "[Tool: web_search]\n";

    ToolRegistry reg;
    auto res = reg.execute("web_search", {{"query", "C++ std::filesystem example"}, {"num", "3"}});
    // Questo test richiede rete — se fallisce per timeout/rete non è un bug
    if (res.success) {
        CHECK(!res.output.empty(), "web_search has results");
        // Verifica formato output (numerato)
        CHECK(res.output.find("1.") != std::string::npos, "web_search numbered results");
    } else {
        std::cerr << "  (rete non disponibile, test saltato)\n";
    }

    std::cout << "  OK\n";
}

static void test_hooks()
{
    std::cout << "[Tool Hooks]\n";

    ToolRegistry reg;
    bool before_called = false;
    bool after_called = false;

    // Test before hook: blocca rm assoluti
    reg.set_before_hook([&](const std::string & name,
                             const std::map<std::string, std::string> &) -> ToolResult {
        before_called = true;
        return {};
    });
    reg.set_after_hook([&](const std::string & name,
                            const std::map<std::string, std::string> &,
                            const ToolResult & r) -> ToolResult {
        after_called = true;
        return r;
    });

    auto res = reg.execute("bash", {{"command", "echo test"}});
    CHECK(before_called, "before hook called");
    CHECK(after_called, "after hook called");
    CHECK(res.success, "hook does not block execution");

    // Test before hook: blocca esplicitamente
    reg.set_before_hook([&](const std::string & name,
                             const std::map<std::string, std::string> &) -> ToolResult {
        return {false, "", "bloccato dal test", true};
    });
    res = reg.execute("bash", {{"command", "echo blocked"}});
    CHECK(!res.success, "before hook blocks");
    CHECK(res.is_error, "before hook sets is_error");

    // Test dopo hook: modifica risultato
    reg.set_before_hook({}); // rimuovi blocco precedente
    reg.set_after_hook([](const std::string & name, const std::map<std::string, std::string> &,
                           const ToolResult & r) -> ToolResult {
        auto m = r;
        m.details["hook_added"] = "yes";
        m.details["tool"] = name;
        return m;
    });
    res = reg.execute("bash", {{"command", "echo hook_test"}});
    CHECK(res.details["hook_added"] == "yes", "after hook added detail");
    CHECK(res.details["tool"] == "bash", "after hook adds tool name");

    std::cout << "  OK\n";
}

static void test_git_tools()
{
    std::cout << "[Tool: git_*]\n";
    ToolRegistry reg;

    auto res = reg.execute("git_status", {});
    CHECK(res.success, "git_status runs");
    // git_status potrebbe fallire se non siamo in un repo git — accettiamo entrambi

    res = reg.execute("git_branch", {});
    CHECK(res.success, "git_branch runs");

    res = reg.execute("git_log", {{"n", "3"}});
    CHECK(res.success, "git_log runs");

    res = reg.execute("git_diff", {{"stat", "true"}});
    CHECK(res.success, "git_diff runs");

    std::cout << "  OK\n";
}

static void test_task_tools()
{
    std::cout << "[Tool: task_*]\n";
    ToolRegistry reg;

    // Pulizia
    std::remove(".cache/tasks.json");

    auto res = reg.execute("task_create", {{"title", "Fix scrolling bug"}, {"description", "PageUp not working"}});
    CHECK(res.success, "task_create success");
    CHECK(res.output.find("#1") != std::string::npos, "task_create got id 1");
    CHECK(res.output.find("Fix scrolling") != std::string::npos, "task_create title correct");

    res = reg.execute("task_create", {{"title", "Add git tools"}});
    CHECK(res.success, "task_create 2 success");

    res = reg.execute("task_list", {});
    CHECK(res.success, "task_list success");
    CHECK(res.output.find("#1") != std::string::npos, "task_list shows task 1");
    CHECK(res.output.find("#2") != std::string::npos, "task_list shows task 2");

    res = reg.execute("task_update", {{"id", "1"}, {"status", "in_progress"}});
    CHECK(res.success, "task_update to in_progress");

    res = reg.execute("task_update", {{"id", "1"}, {"status", "done"}});
    CHECK(res.success, "task_update to done");

    // Test con #1 (formato display del task_create)
    res = reg.execute("task_update", {{"id", "#2"}, {"status", "in_progress"}});
    CHECK(res.success, "task_update with # prefix");

    res = reg.execute("task_list", {});
    CHECK(res.output.find("[x]") != std::string::npos, "task_list shows done marker");

    res = reg.execute("task_update", {{"id", "99"}, {"status", "done"}});
    CHECK(!res.success, "task_update nonexistent fails");

    res = reg.execute("task_update", {{"id", "1"}, {"status", "invalid"}});
    CHECK(!res.success, "task_update invalid status fails");

    std::remove(".cache/tasks.json");
    std::cout << "  OK\n";
}

static void test_permissions()
{
    std::cout << "[PermissionManager]\n";

    PermissionManager pm;
    // read è ALLOW per default
    CHECK(pm.check("read", "test.txt") == PermissionAction::ALLOW, "read default allow");

    // bash è ASK per default
    CHECK(pm.check("bash", "ls") == PermissionAction::ASK, "bash default ask");

    // write è ALLOW (dopo la modifica)
    CHECK(pm.check("write", "test.txt") == PermissionAction::ALLOW, "write default allow");

    // Regole specifiche: rm -rf *
    CHECK(pm.check("bash", "rm -rf *") == PermissionAction::DENY, "rm -rf denied");

    // Globale: test non registrato
    CHECK(pm.check("unknown_tool", "test") == PermissionAction::ASK, "unknown tool default ask");

    std::cout << "  OK\n";
}

int main()
{
    std::cout << "=== Tool & Permission Tests ===\n\n";

    test_parse_tool_call();
    test_write_tool();
    test_bash_tool();
    test_glob_tool();
    test_read_tool();
    test_grep_tool();
    test_ls_tool();
    test_rm_tool();
    test_mv_tool();
    test_edit_tool();
    test_find_tool();
    test_fetch_tool();
    test_web_search_tool();
    test_hooks();
    test_git_tools();
    test_task_tools();
    test_permissions();

    std::cout << "\n=== " << passed << "/" << tests << " passed ===\n";
    return passed == tests ? 0 : 1;
}
