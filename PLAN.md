# Piano di Sviluppo — llama-agent vs opencode

## Stato Attuale (Implementato)

### Core Inferenza
- [x] Backend llama.cpp via FetchContent
- [x] Caricamento modello (GGUF) con tutti i flag llama-cli
- [x] Context management (n_ctx, n_past, batch decode)
- [x] Sampling chain (temp, top-k, top-p, min-p, penalties, seed)
- [x] Tokenizzazione/detokenizzazione via common_*
- [x] ReasoningDetector (tag + heuristic thinking/response split)
- [x] StreamingBuffer thread-safe con callback UI

### KVCache Persistente
- [x] Token-file persistenza (evita binary state issues con modelli ricorrenti)
- [x] Ricostruzione KVCache all'avvio con progress bar animata
- [x] Checkpoint prompt-only per /regen
- [x] Salvataggio/caricamento cronologia conversazione JSON
- [x] Flag --no-cache

### UI
- [x] FTXUI TUI con split pane (thinking + response + input)
- [x] Colori: thinking YellowLight, response White
- [x] Scroll via PgUp/PgDn con focusPositionRelative + vscroll_indicator
- [ ] **PgUp/PgDn ancora non scrolla correttamente** (scroll_y cambia ma viewport fermo)
- [x] Footer statistiche (token, tps, cache_size)
- [x] Spinner durante generazione
- [x] SimpleUI (ANSI console, --simple-ui)
- [x] Comandi slash: /help, /clear, /regen, /exit
- [x] Supporto --single-turn con -p

### Strumenti (6 tools)
- [x] `bash` — esecuzione comandi shell
- [x] `read` — lettura file
- [x] `write` — scrittura file
- [x] `grep` — ricerca regex nei contenuti
- [x] `glob` — match pattern file
- [x] `fetch` — download URL
- [x] Tool calling JSON: `{"tool": "...", "args": {...}}`
- [x] Tool result injection nel contesto
- [x] Limite di tool call per turno (default 10)
- [x] ToolRegistry con schema JSON per system prompt

### Permessi
- [x] Sistema gerarchico: globale → per-tool → per-pattern
- [x] Tre stati: ALLOW / ASK / DENY
- [x] Pattern matching (glob-like: `*.txt`, `/etc/*`, `rm -rf *`)
- [x] Session permanent allow
- [x] Default: read/glob/grep=ALLOW, write=ALLOW, bash=ASK

### JSON Parsing & Grammar
- [x] Parsing JSON string-aware (skip_json_string): `{}` e `"` dentro contenuti non confondono il parser
- [x] Scansione lineare chiavi fuori dalle stringhe: falsi key-value nel content non generano falsi match
- [x] Regex trigger `{"tool` per attivazione lazy grammar
- [x] GBNF grammar per tool call JSON: root → tool-call → args → string → value

### Build
- [x] `build.sh` con 10 profili: debug, release, cuda, vulkan, hip, metal, sycl, blas, openmp, all
- [x] `CMAKE_FLAGS_EXTRA` env var per flag extra
- [x] `build.conf` file per configurazione persistente
- [x] `.gitignore`
- [x] README.md con build/usage/tool-calling docs
- [x] Git repo init con 2 commit

---

## Feature Analysis: llama-agent vs opencode

| Feature | opencode | llama-agent | Priority |
|---------|----------|-------------|----------|
| Chat TUI | ✅ | ✅ FTXUI + SimpleUI | - |
| KVCache persistente | ❌ | ✅ Token-based | - |
| Tool calling | ✅ 13 tools | ✅ 6 tools + lazy grammar GBNF | - |
| Systema permessi | ✅ Granulare | ✅ Gerarchico 3-stati | - |
| Session management (--continue, --session, --fork) | ✅ | ❌ | **HIGH** |
| Multi-provider (Anthropic, OpenAI, Google, Groq, AWS, GCP, Azure, DeepSeek, xAI, HuggingFace, Together, Ollama, LM Studio, etc.) | ✅ | ❌ (solo llama.cpp) | **HIGH** |
| Web search (tavily, google, etc.) | ✅ | ❌ | **HIGH** |
| LSP integration (code editing con diagnostics) | ✅ | ❌ | **MEDIUM** |
| Agent system (create/customize agents) | ✅ | ❌ | **MEDIUM** |
| Sub-agents (task orchestration) | ✅ | ❌ | **LOW** |
| Task management (todo.md, todo list UI) | ✅ | ❌ | **MEDIUM** |
| MCP server mode | ✅ | ❌ | **MEDIUM** |
| File editing with diagnostics + lint | ✅ | ❌ (write è raw) | **MEDIUM** |
| Multi-turn planning | ✅ | ❌ | **LOW** |
| Extensions/plugins | ✅ | ❌ | **LOW** |
| Session state persistence (full snapshot) | ✅ | ❌ (solo token) | **LOW** |
| Image/video input | ✅ | ❌ | **LOW** |
| Custom slash commands | ❌ | ✅ /help, /clear, /regen, /exit | - |
| Progress bar cache rebuild | ❌ | ✅ | - |

---

## TODO List (Prioritized)

### P0 — Must Have (prima release)

- [ ] **Fix PgUp/PgDn scrolling** — `scroll_y` cambia ma viewport non si muove. Probabile soluzione: `Container::Vertical` con focusability sugli elementi interni, invece di raw yframe
- [x] **JSON parsing robusto** — 3 bug fixati in `extract_json_block`, `extract_json_value`, `parse_tool_call` (string-aware parsing con `skip_json_string`)
- [x] **Grammar-constrained decoding** — `llama_sampler_init_grammar_lazy_patterns` per tool call JSON con trigger `{"tool`
- [ ] **Session management system** — salva/carica conversazione completa:
  - `--session <name>` per sessioni multiple (invece di singolo .cache/)
  - `--continue` per riprendere ultima sessione
  - `--fork <session>` per creare un branch della sessione
  - Salva: KVCache tokens + cronologia JSON + metadata (modello, parametri, timestamp)
  - Struttura directory: `.sessions/<session_name>/`

### P1 — High Priority

- [ ] **Config file** (JSON/YAML), caricato automaticamente:
  ```yaml
  model: ~/models/qwen2.5-7b.gguf
  default_session: main
  ngl: 35
  temperature: 0.7
  tools:
    bash: allow
    write: ask
    fetch: ask
  ```

- [ ] **Multi-provider abstraction layer**:
  - `class Backend` virtuale (llama.cpp locale, API remota via HTTP)
  - Provider: OpenAI-compatible API, Ollama, Anthropic
  - `--provider openai --api-key ... --model gpt-4`
  - `--provider ollama --model llama3`
  - Switch a runtime tra provider

- [ ] **Web search tool**:
  - `web_search(query, num_results=5)` — chiama API search (Tavily, Google, Bing, o SearXNG self-hosted)
  - Render risultati come contesto strutturato
  - Config API key via config file o env var

- [ ] **LSP integration**:
  - `lsp_hover(file, line, col)` — mostra info simbolo
  - `lsp_diagnostics(file)` — errori/warning del file corrente
  - `lsp_goto_definition(file, line, col)` — naviga a definizione
  - `lsp_complete(file, line, col)` — completamento codice
  - Cliente LSP leggero via socket/stdio

### P2 — Medium Priority

- [ ] **Agent profiles**:
  - `agent create --name "coder" --system-prompt "..." --tools "bash,read,write,grep,lsp"`
  - `agent list` / `agent use <name>`
  - Profili predefiniti: `coder`, `researcher`, `writer`, `default`
  - Ogni profilo ha: system prompt, tool set, permission defaults, model

- [ ] **Smart file editing** (sostituisce write raw):
  - `edit <file> <old_string> <new_string>` con context matching
  - `edit <file> <line> <new_content>` per line-based editing
  - `insert <file> <after_line> <content>`
  - Verifica diagnostica post-edit (se LSP attivo)
  - Diff preview prima dell'applicazione

- [ ] **Task management**:
  - `/todo` — mostra task list corrente
  - `/todo add "fix scrolling bug"` — aggiunge task
  - `/todo done 1` — segna task completato
  - `/todo clear` — pulisce task completati
  - Salva/carica task da `.session/tasks.json`

- [ ] **MCP server mode**:
  - `--mcp` flag: avvia come server MCP (Model Context Protocol)
  - Espone tools come MCP resources
  - Permette a editor esterni (Cursor, VS Code via Continue) di usare llama-agent come backend AI

### P3 — Nice to Have

- [ ] **Multi-agent orchestration**:
  - `agent spawn <profile> "risolvi questo bug"` — lancia sub-agente in thread separato
  - Risultato asincrono: sub-agente risponde quando ha finito
  - Task routing: router agent distribuisce subtask ad agenti specializzati

- [ ] **Full session snapshot** (alternative a token-file):
  - `llama_state_save_file` + `llama_state_load_file` per snapshot binario completo
  - Avanzamento: più veloce del token rebuild ma fragile con modelli ricorrenti
  - Opzione: `--cache-mode token|binary`

- [ ] **Multi-modal input**:
  - Supporto immagini (llama.cpp multimodal via mmproj)
  - `/image <path>` — carica immagine nel contesto
  - Vision tool: `describe_image(path)` — descrive contenuto immagine

- [ ] **Plugin/extensions system**:
  - Directory `~/.config/llama-agent/plugins/`
  - Plugin = shared library `.so` o script `.lua` con funzione `register_tools()`
  - API plugin: `register_tool(name, description, schema, executor)`

- [ ] **TUI improvements**:
  - Mouse wheel scroll
  - Tab completion per path nel input field
  - Syntax highlighting nei tool output
  - Split verticale/horizontale configurabile
  - Pannello cronologia laterale
  - Tema chiaro/scuro

- [ ] **Grid search / benchmark**:
  - `--benchmark` flag: testa modello su prompt standard
  - Report: tps, memory usage, cache size, context utilization
  - Confronto tra parametri (temp, top-p, context size)

---

## Architettura Future

```
llama-agent/
├── src/
│   ├── main.cpp           # Entry point, CLI parsing
│   ├── agent.cpp/h        # Core loop, orchestration
│   ├── backends/          # Multi-provider
│   │   ├── backend.h      # Interfaccia virtuale Backend
│   │   ├── llama_backend.cpp/h  # llama.cpp locale
│   │   ├── openai_backend.cpp/h # API OpenAI-compatibile
│   │   └── ollama_backend.cpp/h # Ollama API
│   ├── sessions/          # Session management
│   │   ├── session.h/cpp  # Session salva/carica/fork
│   │   └── history.h/cpp  # Cronologia conversazione
│   ├── agents/            # Agent system
│   │   ├── agent_profile.h/cpp  # Profili agente
│   │   └── orchestrator.h/cpp   # Multi-agent dispatch
│   ├── tools/
│   │   ├── tools.cpp/h    # ToolRegistry + 6 core tools
│   │   ├── lsp.cpp/h      # LSP integration tool
│   │   └── web.cpp/h      # Web search tool
│   ├── permissions.cpp/h  # Permission system
│   ├── kvcache.cpp/h      # KVCache persistence
│   ├── reasoning.cpp/h    # Thinking/response detection
│   ├── streaming.cpp/h    # Token streaming buffer
│   ├── ui.cpp/h           # UI base + SimpleUI
│   └── ui_ftxui.cpp       # FTXUI TUI
├── build.sh               # Multi-profile build
├── CMakeLists.txt
└── README.md
```

## Dipendenze Future

| Libreria | Scopo |
|----------|-------|
| httplib (cpp-httplib) | Chiamate HTTP per provider esterni e web search |
| nlohmann/json | Già presente via llama.cpp, per parsing JSON robusto |
| Lua/sol2 | Opzionale: plugin system scripting |
| libcurl | Per fetch tool + API calls |

## Metriche di Successo v0.1.0

- [ ] PgUp/PgDn scrolling funzionante
- [ ] Session management: `--session`, `--continue`, `--fork`
- [ ] Config file: `.llama-agent.json` o `llama-agent.yml`
- [ ] Almeno un provider remoto (OpenAI-compatible)
- [ ] Web search tool funzionante
- [ ] LSP integration base (diagnostics + hover)

```
