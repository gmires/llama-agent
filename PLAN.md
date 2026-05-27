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
- [x] Comandi slash: /help, /clear, /regen, /exit
- [x] Supporto --single-turn con -p

### Tool Calling (7 tools)
- [x] `bash` — comandi shell con timeout configurabile, cattura stderr
- [x] `read` — lettura file
- [x] `write` — scrittura file
- [x] `grep` — ricerca regex ricorsiva
- [x] `glob` — match pattern file
- [x] `find` — ricerca file nativa C++ (std::filesystem + fnmatch, max_depth, tipo)
- [x] `fetch` — download URL
- [x] Tool call JSON: `{"tool": "...", "args": {...}}` con supporto markdown
- [x] Tool result injection nel contesto
- [x] Limite tool call per turno (--tool-limit)
- [x] ToolRegistry con schema JSON per system prompt

### JSON Parsing
- [x] Estrazione JSON block: `skip_json_string()` ignora `{}` dentro stringhe
- [x] Estrazione valore: escape-aware, non troncata da `"` interne
- [x] Parsing tool call: scan lineare chiavi fuori dalle stringhe, no falsi match
- [x] Supporto: key alias (`tool`/`function`/`tool_call`), args alias (`args`/`parameters`/`params`)
- [x] 37 parser test passano

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

- [ ] **Tool: smart edit** (sostituisce write raw):
  - `edit <file> <"old_string"> <"new_string">` — string replacement con context matching
  - `edit <file> <start_line> <end_line> <"new_content">` — line-based editing
  - Verifica pre-edit: il file esiste, old_string è unico
  - Diff preview prima dell'applicazione
  - Rollback automatico se la modifica rompe la compilazione (opzionale)

- [ ] **Tool result strutturato**:
  ```cpp
  struct ToolResult {
      bool success;
      bool is_error = false;          // esplicito: errore del tool
      std::string content;            // per il LLM
      std::map<std::string, std::string> details;  // metadati (file_size, match_count, ...)
  };
  ```
  - Separa contenuto LLM da metadati
  - Tool possono troncare output ma mantenere info complete nei details

- [ ] **before_tool_call / after_tool_call hooks**:
  - `beforeToolCall`: path protection, sandbox, conferma aggiuntiva
  - `afterToolCall`: filtra output, aggiungi metadati, tronca se troppo lungo

- [ ] **Markdown rendering migliorato**:
  - Code block syntax highlighting (delega a libreria esterna o regex base per C++/Python/JS)
  - Link cliccabili
  - Liste indentate con bullet `•`

- [ ] **Compattazione contesto** (context compaction):
  - **Problema attuale**: quando `n_past_ + nuovi_token > n_ctx - 128`, il contesto viene resettato completamente (`llama_memory_clear`) — si perde tutta la cronologia
  - **Soglia compattazione**: trigger a ~80% del contesto (`n_past_ > 0.8 * n_ctx`)
  - **Strategia**:
    1. Rileva superamento soglia prima di aggiungere nuovi token
    2. Separa i messaggi in "vecchi" (primi 60%) e "recenti" (ultimi 40%)
    3. Genera un riassunto dei messaggi vecchi con una chiamata LLM dedicata:
       ```
       "Riassumi questa conversazione in 3-5 frasi, preservando informazioni su file modificati e decisioni prese: ..."
       ```
    4. Ricostruisci il prompt: system + `<summary>...</summary>` + messaggi recenti
    5. Rivaluta il nuovo prompt compresso
  - **Configurabile**: `--compact-threshold 0.8` (frazione del contesto), `--compact-keep-last N` (messaggi recenti da preservare)
  - **Comando manuale**: `/compact` per forzare la compattazione immediata
  - **Compattazione automatica**: abilitata di default, disabilitabile con `--no-compact`
  - **Salvataggio**: i messaggi originali rimangono in `conversation.json`; il summary è solo per la KVCache corrente
  - **Riferimento**: simile al `/compact` di pi (lossy, history originale preservata nel file JSONL)

### P2 — Media Priorità (espansione)

- [ ] **Extensions system**:
  - Directory `~/.config/llama-agent/extensions/`
  - Extension = shared library `.so` con API:
    ```cpp
    extern "C" void register_extension(ToolRegistry & tools, PermissionManager & perms);
    ```
  - Ogni extension può: registrare tool, aggiungere comandi slash, hook sugli eventi
  - Caricamento automatico a startup

- [ ] **Più tool file-system**:
  - `ls <path>` — lista contenuto directory (colonne: nome, tipo, dimensione)
  - `tree <path> <max_depth>` — visualizzazione ad albero
  - `rm <path>` — elimina file (protetto da permessi)
  - `mv <from> <to>` — sposta/rinomina file

- [ ] **Comandi slash estesi**:
  - `/model` — mostra modello corrente
  - `/session` — mostra info sessione (ID, file, messaggi, token, costo)
  - `/stats` — statistiche dettagliate (memoria, throughput, latenza)
  - `/compact` — compatta contesto (riassumi messaggi vecchi, mantieni ultimi N)

- [ ] **Multi-provider abstraction**:
  - `class Backend` virtuale: llama.cpp locale + API remote
  - Provider iniziale: OpenAI-compatible API (`--provider openai --api-key ...`)
  - Switch a runtime, stesso flusso di tool calling

- [ ] **Web search tool**:
  - `web_search <query> [num_results=5]` — via SearXNG self-hosted o API
  - Config: URL server, API key
  - Render risultati come contesto strutturato

### P3 — Bassa Priorità (nice to have)

- [ ] **TUI miglioramenti**:
  - Autocompletamento file path (`@` + Tab, come pi) con fuzzy search
  - Mouse wheel scroll
  - Syntax highlighting nel code block con shiki/tree-sitter

- [ ] **Steering messages**:
  - Invia messaggi mentre l'agente lavora (durante esecuzione tool)
  - Coda messaggi: steering (consegnato dopo turno corrente) e follow-up (dopo che l'agente finisce)

- [ ] **Tool: task management**:
  - `task_create <title> <description>` — crea task
  - `task_list` — mostra task attivi
  - `task_update <id> <status>` — aggiorna stato (todo/in_progress/done)
  - Persistenza: `.cache/tasks.json`

- [ ] **Tool: git integration**:
  - `git_diff` — mostra diff non committato
  - `git_log [n=10]` — ultimi commit
  - `git_status` — stato working tree
  - `git_branch` — branch corrente + lista

- [ ] **LSP integration**:
  - `lsp_diagnostics <file>` — errori/warning
  - `lsp_hover <file> <line> <col>` — info simbolo
  - Cliente LSP leggero via stdio

- [ ] **Agent profiles**:
  - Profili predefiniti: `coder` (bash, read, write, grep, find), `researcher` (read, grep, fetch, find), `sysadmin` (bash, read, find)
  - Ogni profilo: system prompt, tool set, permission defaults
  - `--profile coder`

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
