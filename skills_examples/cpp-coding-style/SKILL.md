# C++ Coding Style

## Naming
- Classes/Structs: `PascalCase` (es. `KVCacheManager`, `ToolRegistry`)
- Methods/Functions: `snake_case` (es. `build_system_prompt()`, `parse_tool_call()`)
- Private members: `trailing_underscore_` (es. `kvcache_`, `tools_`, `n_past_`)
- Constants: `UPPER_SNAKE_CASE` (es. `MAX_TOKENS`, `CACHE_MAGIC`)
- Parameters: `snake_case` (es. `cache_dir`, `n_ctx`)

## Includes
- Standard library: `#include <header>` (es. `<string>`, `<vector>`)
- Project headers: `#include "header.h"` (es. `"agent.h"`, `"tools.h"`)
- Third-party: same as project headers

## Patterns
- Prefer `const auto &` for read-only parameters and loop variables
- Use `std::unique_ptr` for owned resources, raw pointers for non-owned
- RAII everywhere — no manual `new`/`delete`
- Early returns instead of deep nesting
- Use `enum class` not plain `enum`
- Error handling: return false/error, don't throw exceptions

## Tool Development
- Each tool is a `ToolDefinition` with name, description, parameters, executor
- Register via `register_tool()` in `ToolRegistry` constructor
- Test in `toolstest/test_tools.cpp` with `CHECK` macro
- ToolResult: `{success, output, error, is_error, details}`
- Output limit: 16KB for text, 64KB for files

## File Structure
```
src/
  main.cpp       — CLI entry point
  agent.h/.cpp   — Core Agent class
  tools.h/.cpp   — ToolRegistry + 20 tools
  kvcache.h/.cpp — Token/state cache persistence
  ui.h/.cpp      — UI base + SimpleUI
  ui_ftxui.cpp   — FTXUI TUI implementation
  permissions.h/.cpp   — Permission system
  reasoning.h/.cpp     — Thinking/response detection
  streaming.h/.cpp     — Token streaming buffer
toolstest/
  test_tools.cpp       — 104 tests
skills_examples/       — Example skills
```
