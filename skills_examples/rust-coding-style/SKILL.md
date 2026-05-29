# Rust Coding Style

## Naming
- Types/Traits: `PascalCase` (es. `ToolRegistry`, `ParseResult`)
- Functions/Methods: `snake_case` (es. `parse_json()`, `load_config()`)
- Variables: `snake_case` (es. `token_count`, `file_path`)
- Constants/Statics: `UPPER_SNAKE_CASE` (es. `MAX_RETRIES`, `DEFAULT_BUFFER`)
- Lifetimes: short lowercase (`'a`, `'de`)

## Project Structure
```
src/
  main.rs       — entry point
  lib.rs        — library root
  agent.rs      — Agent module
  tools.rs      — Tool definition and registry
  kvcache.rs    — Cache persistence
  ui.rs         — Terminal UI
  permissions.rs — Permission system
```

## Cargo.toml
- Pin dependencies to minor version: `serde = "1.0"`
- Use `[features]` for optional functionality
- `[profile.release]` with `opt-level = 3`, `lto = true`

## Patterns
- Prefer `Result<T, E>` and `?` operator over `unwrap()` and `expect()`
- Use `match` for exhaustive handling, not `if let` for fallible cases
- Derive `Debug, Clone` for data types
- Use `impl Trait` for return types, `dyn Trait` for dynamic dispatch
- Avoid `unsafe` unless absolutely necessary
- Log with the `log` crate, not `println!()`

## Testing
- Unit tests in the same file: `#[cfg(test)] mod tests { ... }`
- Integration tests in `tests/` directory
- Use `#[test]` attribute, `assert!()` and `assert_eq!()` macros
