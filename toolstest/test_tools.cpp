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
    auto res = reg.execute("read", {{"path", "CMakeLists.txt"}});
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
    auto res = reg.execute("grep", {{"pattern", "int main"}, {"path", "src"}});
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
    test_permissions();

    std::cout << "\n=== " << passed << "/" << tests << " passed ===\n";
    return passed == tests ? 0 : 1;
}
