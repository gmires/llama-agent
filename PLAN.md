# Piano di Sviluppo — llama-agent

Agente AI conversazionale in C++ basato su llama.cpp con interfaccia TUI (FTXUI),
persistenza della KVCache su disco e sistema di strumenti (tools) stile opencode.

---

## Fase 1 — Scaffold del Progetto

- [x] Creare struttura directory (`src/`, `.cache/`)
- [x] `CMakeLists.txt` con dipendenze: llama.cpp (FetchContent), FTXUI (FetchContent)
- [ ] `src/main.cpp` — scheletro con `common_params_parse` per accettare tutti i flag di llama-cli
- [ ] Verificare che il progetto compili e linki

**Output**: `llama-agent --help` mostra l'uso completo (stesso output di `llama-cli`)

---

## Fase 2 — Core di Inferenza

- [ ] `src/agent.h / agent.cpp` — classe `Agent` che incapsula:
  - Inizializzazione backend (`llama_backend_init`)
  - Caricamento modello via `common_init_from_params`
  - Tokenizzazione / detokenizzazione
  - Loop di inferenza sincrono (prompt → decode → sample → stream)
- [ ] `src/reasoning.h / reasoning.cpp` — modulo di rilevamento:
  - Rileva token di thinking (DeepSeek: `...`)
  - Rileva transizione thinking → response
  - Classifica ogni token come `think` o `response`
- [ ] `src/streaming.h / streaming.cpp` — buffer di streaming:
  - Coda thread-safe di token
  - Callback per notifiche UI
  - Accumulo del testo parziale

**Output**: L'agente può generare testo dato un prompt, classificando i token in think/response

---

## Fase 3 — KVCache Persistente

- [ ] `src/kvcache.h / kvcache.cpp` — classe `KVCacheManager`:
  - Salva stato (`llama_state_save_file`) su `.cache/session.bin`
  - Carica stato (`llama_state_load_file`) da `.cache/session.bin`
  - Invalida cache se cambia modello o prompt lungo
  - Supporto cache multipla (hash della configurazione)
- [ ] Integrazione nel ciclo di vita dell'Agent:
  - All'avvio: carica cache se esiste
  - Dopo ogni risposta completa: salva cache
  - Opzione `--no-cache` per disabilitare

**Output**: Riavviando l'agente, il contesto della conversazione precedente è preservato

---

## Fase 4 — Interfaccia TUI (FTXUI)

- [ ] `src/ui.h / ui.cpp` — classe `UI`:
  - Schermata principale con `ftxui::App`
  - Split orizzontale: pannello THINKING (sinistra) + RESPONSE (destra)
  - Pannello THINKING: testo in colore cyan dim/italico, scrollabile
  - Pannello RESPONSE: testo in colore bianco brillante, scrollabile
  - Barra di input in basso (`ftxui::Input` con history)
  - Footer con statistiche: token count, tokens/sec, dimensione cache
  - Spinner / indicatore di attività durante l'inferenza
- [ ] Streaming live: aggiornamento token-by-token su entrambi i pannelli
- [ ] Supporto `--simple-ui` per fallback a console ANSIA (stile llama-cli)

**Output**: `llama-agent -m model.gguf` apre una UI TUI con split panes

---

## Fase 5 — Sistema di Strumenti (Tools)

- [ ] `src/tools.h / tools.cpp` — classe `ToolRegistry`:
  - `bash` — esegue comandi shell, cattura output
  - `read` — legge file di testo
  - `write` — scrive file di testo
  - `grep` — ricerca contenuti
  - `glob` — match pattern file
  - `web_fetch` — scarica URL (nella versione successiva)
  - Ogni tool ha: nome, descrizione, parametri (schema JSON-like), esecutore
- [ ] Formato di tool calling JSON compatibile con modelli GGUF
- [ ] Dispatch: estrae tool_call dalla risposta LLM → esegue → re-inietta risultato
- [ ] Limite di tool call per turno (anti-loop)

**Output**: L'agente può eseguire comandi e leggere/scrivere file

---

## Fase 6 — Sistema di Permessi

- [ ] `src/permissions.h / permissions.cpp` — classe `PermissionManager`:
  - Tre stati per ogni tool: `allow`, `ask`, `deny`
  - Pattern matching per percorsi file (es. `*.txt` allow, `*.key` deny)
  - Ereditarietà: globale → per-tool → per-pattern
  - Modalità `ask`: mostra richiesta di conferma nella UI
  - Persistenza delle risposte "always allow" per la sessione
- [ ] Integrazione UI: popup di conferma stile `ftxui::Modal`

**Output**: L'utente può configurare permessi granulari

---

## Fase 7 — Ciclo Agente Completo

- [ ] Integrazione di tutti i moduli:
  ```
  1. UI mostra input field
  2. Utente scrive prompt → Enter
  3. Agent tokenizza e llama_decode
  4. Streaming: pensiero → pannello THINKING
  5. Transizione → risposta → pannello RESPONSE
  6. Se tool_call: mostra nel footer, esegui tool, feedback
  7. Fine risposta: salva KVCache
  8. Torna a (1)
  ```
- [ ] Prompt di sistema predefinito con descrizione tools
- [ ] History conversazione in memoria
- [ ] Comandi slash: `/exit`, `/clear`, `/regen`
- [ ] Supporto `--single-turn` per uso non interattivo

**Output**: Agente conversazionale completo

---

## Fase 8 — Rifiniture

- [ ] Gestione errori robusta (modello non trovato, OOM, CTRL+C)
- [ ] Performance: benchmark tokens/sec
- [ ] Logging opzionale su file
- [ ] Completamento tab per percorsi
- [ ] Test manuali con modelli reali
- [ ] Documentazione README.md

**Output**: Release v0.1.0

---

## Dipendenze

| Libreria | Versione | Ruolo |
|----------|----------|-------|
| llama.cpp | latest (main) | Runtime ML, tokenizer, KVCache, sampling |
| FTXUI | v6.1.9 | TUI framework (split panes, input, colori) |
| Nlohmann JSON | (via llama.cpp) | JSON parsing per tool calling |

## Flag CLI Supportati

Tutti i flag di `llama-cli` sono supportati tramite `common_params_parse`:
`-m`, `-ngl`, `-c`, `-b`, `-p`, `--temp`, `--top-k`, `--top-p`, `--seed`, ecc.

Flag aggiuntivi specifici di llama-agent:

| Flag | Default | Descrizione |
|------|---------|-------------|
| `--simple-ui` | false | Usa console ANSI base invece di FTXUI |
| `--no-cache` | false | Disabilita KVCache persistente |
| `--cache-dir` | ./.cache | Directory per la KVCache |
| `--tool-limit` | 10 | Max tool call per turno |
| `--permission` | ask | Default permessi: allow/ask/deny |
