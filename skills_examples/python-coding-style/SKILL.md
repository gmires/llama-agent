# Python Coding Style

## Naming
- Classes: `PascalCase` (es. `TokenStream`, `ModelLoader`)
- Functions/Methods: `snake_case` (es. `process_tokens()`, `load_config()`)
- Variables: `snake_case` (es. `token_count`, `cache_dir`)
- Constants: `UPPER_SNAKE_CASE` (es. `MAX_RETRIES`, `DEFAULT_PORT`)
- Private: `_leading_underscore` (es. `_internal_state`, `_parse_json()`)

## Imports
1. Standard library first (`import os, sys`)
2. Third-party packages (`import numpy, requests`)
3. Local modules (`from .tools import ToolRegistry`)
- One import per line for named imports
- Never use `import *`

## Type Hints
- Always annotate function signatures: `def process(data: list[str]) -> dict[str, int]:`
- Use `Optional[T]` instead of `T | None` for Python <3.10
- Use `Protocol` for duck typing, `ABC` for abstract classes

## Tool Scripts
- Shebang: `#!/usr/bin/env python3`
- Use `argparse` for CLI arguments
- Return JSON output via stdout, errors via stderr
- Logging: `logging` module, not `print()`
- Handle exceptions gracefully with `try/except`
