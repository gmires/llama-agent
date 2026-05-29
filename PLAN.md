# Piano di Sviluppo — llama-agent

## Stato Attuale (Implementato)

### Core Inferenza
- [x] Backend llama.cpp via FetchContent
- [x] Caricamento modello (GGUF) con tutti i flag llama-cli
- [x] Context management (n_ctx, n_past, batch decode) con gestione overflow
- [x] Sampling chain (temp, top-k, top-p, min-p, penalties, seed)
- [x] Tokenizzazione/detokenizzazione via common_*
- [x] ReasoningDetector (split thinking/response)
- [x] StreamingBuffer thread-safe con callback UI

### KVCache Persistente
- [x] Fast mode: `llama_state_save_file` / `llama_state_load_file` — ripristino in ms
- [x] Token mode: token-file con ricostruzione KVCache + barra di progresso
- [x] Fallback automatico: stato binario corrotto -> ricostruzione da token
- [x] Checkpoint prompt-only per /regen
- [x] Cronologia conversazione JSON (salva/carica)
- [x] Flag: `--cache-mode fast|token`, `--no-cache`

### UI
- [x] FTXUI TUI con messaggi strutturati (6 tipi: USER/ASSISTANT/THINKING/TOOL_CALL/TOOL_RESULT/SYSTEM)
- [x] Tool call a blocchi: `▸ Tool: name(args)` blu bold, `│ risultato` blu dim
- [x] Thinking collassabile con toggle tasto `T` (▼/▶)
- [x] Scroll PgUp/PgDn/Home/End con `focusPositionRelative`
- [x] Code block inline (` ``` `) con sfondo grigio e testo cyan
- [x] Colori per ruolo: utente verde `❯`, assistant bianco, heading giallo, sistema grigio
- [x] Footer con statistiche + hint tasti
- [x] Spinner animato durante generazione
- [x] SimpleUI alternativa (ANSI, --simple-ui)
- [x] Comandi slash: /help, /clear, /regen, /model, /session, /stats, /exit
- [x] Supporto --single-turn con -p

### Tool Calling (12 tools)
- [x] `bash` — comandi shell con timeout configurabile, cattura stderr
- [x] `read` — lettura file
- [x] `write` — scrittura file
- [x] `grep` — ricerca regex ricorsiva
- [x] `glob` — match pattern file con brace expansion (`*.{h,cpp}`)
- [x] `find` — ricerca file nativa C++ (std::filesystem + fnmatch, max_depth, brace expansion)
- [x] `fetch` — download URL
- [x] `ls` — elenca directory (tipo, dimensione, nome)
- [x] `rm` — elimina file (protetto da permessi)
- [x] `mv` — sposta/rinomina file/directory
- [x] `edit` — sostituisce stringa in file (match unico, più sicuro di write)
- [x] `web_search` — cerca su DuckDuckGo (titolo, URL, snippet)
- [x] Tool call JSON: `{"tool": "...", "args": {...}}` con supporto markdown
- [x] Tool result injection nel contesto
- [x] Limite tool call per turno (--tool-limit)
- [x] ToolRegistry con schema JSON per system prompt
- [x] Hook before/after tool call (path protection + auto-truncation)
- [x] Test suite: 88 test (37 parser + 12 tool exec + hooks + permissions)

### JSON Parsing
- [x] Estrazione JSON block: `skip_json_string()` ignora `{}` dentro stringhe
- [x] Estrazione valore: escape-aware, non troncata da `"` interne
- [x] Parsing tool call: scan lineare chiavi fuori dalle stringhe, no falsi match
- [x] Supporto: key alias (`tool`/`function`/`tool_call`), args alias (`args`/`parameters`/`params`)
- [x] Brace expansion: `*.{h,cpp}` → `*.h`, `*.cpp` in glob/find
- [x] 88 test totali (37 parser + 12 tool exec + 7 hook + permissions)

### Permessi
- [x] Sistema gerarchico: globale → per-tool → per-pattern
- [x] Tre stati: ALLOW / ASK / DENY
- [x] Pattern matching glob-like
- [x] UI overlay permessi in FTXUI
- [x] Default: read/grep/glob/find=ALLOW, write=ALLOW, bash=ASK, fetch=ASK

### Build
- [x] `build.sh` con 10 profili: debug, release, cuda, vulkan, hip, metal, sycl, blas, openmp, all
- [x] `CMAKE_FLAGS_EXTRA` per flag CMake aggiuntivi
- [x] `build.conf` per configurazione persistente
- [x] README.md e PLAN.md aggiornati

---

## Feature Raccomandate (basato su analisi di pi-mono)

### P0 — Priorità Massima (stabilità e usabilità)

- [ ] **Session management con tree**:
  - Sessioni JSONL con `id`/`parentId` (come pi) — branching senza duplicare file
  - `--session <name>` per sessioni multiple
  - `--continue` per riprendere ultima sessione
  - `--fork <session>` per creare branch
  - `/tree` comando per navigare l'albero della sessione
  - Struttura: ogni entry ha id, parentId, turn (user/assistant/toolResult), timestamp

- [ ] **Skills system** (Agent Skills standard):
  - Carica file `SKILL.md` da `.skills/`, `~/.config/llama-agent/skills/`
  - `/skill:name` per invocare manualmente
  - Inietta skills nel system prompt come ` <skill name="...">...</skill>`
  - Il modello decide quando caricare automaticamente
  - Sistema plugin-like senza codice: basta scrivere un file markdown

- [ ] **Config file** (JSON):
  ```json
  {
    "model": "~/models/qwen2.5-7b.gguf",
    "cache_mode": "fast",
    "n_ctx": 8192,
    "temperature": 0.7,
    "permissions": {
      "bash": "ask",
      "write": "allow"
    },
    "system_prompt": "Sei un assistente esperto in C++...",
    "context_files": ["AGENTS.md", "CLAUDE.md"]
  }
  ```
  - Caricamento: `~/.config/llama-agent/config.json` (globale) + `.llama-agent.json` (progetto)
  - Merge: progetto sovrascrive globale

### P1 — Alta Priorità (funzionalità chiave)

- [x] **Tool: smart edit** — string replacement con context matching (implementato)
- [x] **Tool result strutturato** — `is_error`, `details` map per metadati
- [x] **before_tool_call / after_tool_call hooks** — path protection, truncation, metadati
- [ ] **Markdown rendering migliorato** — syntax highlighting code block, link cliccabili
- [x] **Compattazione contesto** — Summary Compression: trigger all'80%, scansiona tool call scartati, preserva file operations/ricerche/errori, `/compact` manuale

### P2 — Media Priorità (espansione)

- [ ] **Extensions system**:
  - Directory `~/.config/llama-agent/extensions/`
  - Extension = shared library `.so` con API:
    ```cpp
    extern "C" void register_extension(ToolRegistry & tools, PermissionManager & perms);
    ```
  - Ogni extension può: registrare tool, aggiungere comandi slash, hook sugli eventi
  - Caricamento automatico a startup

- [x] **Più tool file-system**:
  - `ls <path>` — lista contenuto directory (colonne: tipo, dimensione, nome)
  - `rm <path>` — elimina file (protetto da permessi)
  - `mv <from> <to>` — sposta/rinomina file
  - [ ] `tree <path> <max_depth>` — visualizzazione ad albero

- [x] **Comandi slash estesi**:
  - `/model` — mostra modello corrente
  - `/session` — mostra info sessione (messaggi, token, cache)
  - `/stats` — statistiche dettagliate (CPU, GPU, sampling, contesto)
  - `/compact` — compatta contesto con Summary Compression

- [ ] **Multi-provider abstraction**:
  - `class Backend` virtuale: llama.cpp locale + API remote
  - Provider iniziale: OpenAI-compatible API (`--provider openai --api-key ...`)
  - Switch a runtime, stesso flusso di tool calling

- [x] **Web search tool**:
  - `web_search <query> [num=5]` — via DuckDuckGo HTML (curl + parsing)
  - Restituisce titolo, URL, snippet per ogni risultato

### P3 — Bassa Priorità (nice to have)

- [ ] **TUI miglioramenti**: autocomplete file path, mouse wheel, syntax highlighting

- [ ] **Steering messages**: invia messaggi mentre l'agente lavora

- [x] **Tool: task management**:
  - `task_create <title> [description]` — crea task
  - `task_list` — mostra task con stato ([ ]/[~]/[x])
  - `task_update <id> <status>` — aggiorna (todo/in_progress/done)
  - Persistenza: `.cache/tasks.json`

- [x] **Tool: git integration**:
  - `git_diff [stat=true]` — diff working tree
  - `git_log [n=10]` — ultimi commit
  - `git_status` — working tree status
  - `git_branch` — branch locali

- [ ] **LSP integration**: `lsp_diagnostics`, `lsp_hover`

- [ ] **Agent profiles**: `--profile coder/researcher/sysadmin`

---

## Architettura Proposta

```
llama-agent/
├── src/
│   ├── main.cpp              # Entry point, CLI
│   ├── agent.h / .cpp        # Core loop, orchestration
│   ├── kvcache.h / .cpp      # Persistenza KVCache (fast + token mode)
│   ├── tools.h / .cpp        # ToolRegistry + 7 core tools
│   ├── permissions.h / .cpp  # Sistema permessi gerarchico
│   ├── sessions/             # NEW: Session tree JSONL
│   │   ├── session.h / .cpp
│   │   └── tree.h / .cpp
│   ├── skills/               # NEW: Agent Skills loader
│   │   └── skills.h / .cpp
│   ├── extensions/           # NEW: Extension loader
│   │   └── extensions.h / .cpp
│   ├── backends/             # NEW: Multi-provider
│   │   ├── backend.h         # Interfaccia virtuale
│   │   ├── llama_backend.h/cpp  # llama.cpp locale
│   │   └── openai_backend.h/cpp # API OpenAI-compatibile
│   ├── reasoning.h / .cpp    # Thinking/response detection
│   ├── streaming.h / .cpp    # Token streaming buffer
│   ├── ui.h / .cpp           # UI base + SimpleUI
│   └── ui_ftxui.cpp          # FTXUI TUI
├── toolstest/                # Test suite parsing
├── build.sh                  # Multi-profile build
├── CMakeLists.txt
└── README.md
```

## Metrica di Successo v0.2.0

- [ ] Session management con tree JSONL: `--session`, `--continue`, `--fork`, `/tree`
- [ ] Skills system: `SKILL.md` loading, `/skill:name`
- [ ] Config file JSON: globale + progetto, merge
- [ ] Tool `edit` con string replacement
- [ ] before_tool_call / after_tool_call hooks
- [ ] Markdown code block con syntax highlighting base

## Riferimenti

- [pi-mono](https://github.com/earendil-works/pi) — ispirazione per UI (differential rendering, autocomplete), tools (edit, grep, find, ls), session tree, extensions, skills
- [Agent Skills standard](https://agentskills.io) — formato `SKILL.md` per skills portabili
- [llama.cpp](https://github.com/ggml-org/llama.cpp) — backend di inferenza
