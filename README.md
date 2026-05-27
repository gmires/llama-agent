# llama-agent

Agente conversazionale AI con terminale TUI, basato su [llama.cpp](https://github.com/ggml-org/llama.cpp).
Supporta tool calling autonomo, cache persistente su disco, e interfaccia FTXUI con split pane.

## Dipendenze

- C++17, CMake >= 3.14, OpenSSL

Opzionali (almeno uno per accelerazione):
- CUDA Toolkit — GPU NVIDIA
- Vulkan SDK — GPU Vulkan
- ROCm — GPU AMD
- Metal — Apple Silicon
- OpenBLAS / MKL — CPU accelerata
- OpenMP — multi-threading CPU

## Compilazione

```bash
# Release CPU (default)
./build.sh release

# Accelerazione GPU
./build.sh cuda     # NVIDIA CUDA
./build.sh vulkan   # Vulkan (qualsiasi GPU)
./build.sh hip      # AMD ROCm
./build.sh metal    # Apple Silicon
./build.sh sycl     # Intel SYCL

# Accelerazione CPU
./build.sh blas     # BLAS (OpenBLAS, MKL, ...)
./build.sh openmp   # OpenMP

# Debug (solo sviluppo)
./build.sh debug
```

### Flag avanzati

Imposta la variabile `CMAKE_FLAGS_EXTRA` per flag CMake aggiuntivi:

```bash
CMAKE_FLAGS_EXTRA="-DLLAMA_CUDA_FA=ON -DLLAMA_CUDA_GRAPHS=ON" ./build.sh cuda
```

Oppure crea un file `build.conf` nella root del progetto:

```ini
# build.conf
LLAMA_CUDA=ON
LLAMA_CUDA_FA=ON
LLAMA_CUDA_GRAPHS=OFF
GGML_BLAS=ON
GGML_BLAS_VENDOR=MKL
```

## Utilizzo

```bash
./build/llama-agent -m <percorso-modello.gguf>
```

### Opzioni CLI specifiche di llama-agent

| Opzione | Default | Descrizione |
|---------|---------|-------------|
| `-m`, `--model` | — | Percorso modello GGUF |
| `--simple-ui` | off | Interfaccia testuale semplice (senza FTXUI) |
| `--no-cache` | off | Disabilita cache persistente su disco |
| `--cache-mode` | `fast` | Modalità cache: `fast` (stato binario, ms) o `token` (solo token, prefill) |
| `--tool-limit N` | `10` | Numero massimo di tool call per turno |
| `--single-turn` | off | Elabora il prompt `-p` ed esce (modalità non interattiva) |

Tutti i flag di [llama-cli](https://github.com/ggml-org/llama.cpp/blob/master/examples/main/README.md) sono supportati:
`--temp`, `--top-k`, `--top-p`, `--seed`, `-c` (ctx-size), `--threads`, `-ngl` (GPU layers), ecc.

### Comandi nella UI

| Comando | Azione |
|---------|--------|
| `/help` | Mostra aiuto e tool disponibili |
| `/clear` | Cancella cronologia e cache |
| `/regen` | Rigenera ultima risposta |
| `/exit` | Esci |

### Tasti

| Tasto | Azione |
|-------|--------|
| `Enter` | Invia messaggio |
| `Ctrl+Enter` / `Ctrl+J` | Nuova riga nell'input |
| `PgUp` / `PgDn` | Scroll cronologia |
| `Home` / `End` | Vai all'inizio/fine della cronologia |
| `Su` / `Giù` | Cronologia prompt precedenti |

---

## Tool calling

L'agente può eseguire strumenti autonomamente. Il modello emette un blocco JSON:

```json
{"tool": "nome", "args": {"param1": "valore"}}
```

Supporta anche il formato markdown: ` ```json ... ``` ` e le varianti `"function"` / `"tool_call"` come chiave.

### Robustezza parsing JSON

Il parser è progettato per gestire contenuti complessi (HTML, CSS, JavaScript) nei parametri:

- **`{}` nelle stringhe**: contenuti come `{ x: 10, y: 10 }` in JS non confondono il parser — `skip_json_string()` salta correttamente le stringhe JSON
- **`"` nelle stringhe**: virgolette escape-aware nei contenuti non troncano il valore
- **Pattern `chiave: valore`**: scan lineare fuori dalle stringhe — falsi key-value nel contenuto non generano falsi match

### Tool disponibili (7)

| Tool | Descrizione | Parametri |
|------|-------------|-----------|
| `bash` | Esegue comandi shell (2>&1) con timeout configurabile | `command` (obbligatorio), `timeout` (default: 30s) |
| `read` | Legge file di testo (max 64KB) | `path` |
| `write` | Scrive/crea file di testo | `path`, `content` |
| `grep` | Cerca pattern regex nei file (ricorsivo) | `pattern`, `path` |
| `glob` | Trova file per pattern glob (`**/*.cpp`, `*.txt`) | `pattern` |
| `find` | Ricerca file nativa C++ con `std::filesystem` | `path` (default: `.`), `pattern` (`*.cpp`, `test*`), `type` (file/dir/any), `max_depth` |
| `fetch` | Scarica URL via curl | `url`, `format` (text/markdown), `timeout` |

### Esempio di sessione

```
> Cerca nel codice tutti i file .h e contami quanti sono
  >> glob(pattern="*.h")
  [TOOL] glob OK: src/agent.h, src/tools.h, ...
  Trovati 7 file .h nel progetto.

> Leggi il file src/agent.h e dimmi cosa contiene
  >> read(path="src/agent.h")
  Il file definisce la classe Agent con metodi per init, eval_prompt, generate...

> Crea un file Hello World in Rust nella directory testp/
  >> bash(command="mkdir -p testp")
  >> write(path="testp/main.rs", content="fn main() { println!(\"Hello\"); }")
  Fatto: file testp/main.rs creato (42 bytes).
```

---

## Cache persistente

La KVCache viene salvata su disco dopo ogni conversazione. All'avvio successivo:

- **Fast mode (default)**: carica lo stato binario del contesto via `llama_state_save_file` / `llama_state_load_file` — ripristino in millisecondi. Se lo stato è corrotto (modello cambiato, versione llama.cpp diversa), cancella il file e ricade automaticamente sui token.
- **Token mode** (`--cache-mode token`): carica solo i token e ricostruisce la KVCache valutandoli in batch, con barra di progresso animata.

Disabilita con `--no-cache`.

### File generati in `.cache/`

| File | Descrizione |
|------|-------------|
| `cache_<hash>.bin` | Token della conversazione (MAGIC + VERSION + COUNT + tokens[]) |
| `cache_<hash>_state.bin` | Snapshot binario KVCache (solo in fast mode) |
| `cache_<hash>_prompt.bin` | Checkpoint prompt-only per `/regen` |
| `conversation.json` | Cronologia testuale in formato JSON |

### Esempio di flusso

```bash
# Prima esecuzione (lenta: valuta prompt ~900 token su CPU)
./build/llama-agent -m ~/models/qwen2.5-7b.gguf

# ... conversazione, la cache viene salvata automaticamente ...

# Seconda esecuzione (fast mode: ripristino in ~1s)
./build/llama-agent -m ~/models/qwen2.5-7b.gguf

# Con token mode (prefill con barra di progresso)
./build/llama-agent -m ~/models/qwen2.5-7b.gguf --cache-mode token
```

---

## Permessi

Sistema gerarchico a 3 livelli per controllare cosa i tool possono fare:

| Livello | Stato | Significato |
|---------|-------|-------------|
| **Globale** | `ALLOW` / `ASK` / `DENY` | Default per tutti i tool |
| **Per-tool** | `ALLOW` / `ASK` / `DENY` | Default per un tool specifico |
| **Per-pattern** | `ALLOW` / `ASK` / `DENY` | Per pattern di risorse (es. `*.txt`, `/etc/*`, `rm -rf *`) |

Pattern matching glob-like: `*.txt` matcha tutti i .txt, `rm *` matcha comandi pericolosi.

Default: `read`/`grep`/`glob`/`find` = ALLOW, `write` = ALLOW, `bash` = ASK, `fetch` = ASK.

---

## UI

### FTXUI (default)

Interfaccia moderna con:
- **Split pane**: area contenuto scrollabile + input multilinea + footer statistiche
- **Colori per ruolo**: messaggi utente in verde (`> `), assistant in bianco, tool call in blu, heading in giallo
- **Code block**: blocchi tra ` ``` ` con sfondo grigio e testo cyan
- **Thinking/Response split**: thinking in giallo con header `── Thinking ──`, response in bianco
- **Spinner**: `|/-\` animato durante la generazione
- **Footer**: token generati, T/s, utilizzo contesto (n_past/n_ctx), cache size
- **Overlay permessi**: finestra modale centrata per conferma tool (y/n/a)
- **Watchdog**: reset automatico dopo 15 minuti di generazione bloccata

### SimpleUI (`--simple-ui`)

Interfaccia console base con solo ANSI escape codes. Ideale per pipe, SSH, terminali minimali.

---

## Architettura

```
src/
├── main.cpp           — Entry point, CLI parsing, filter_agent_args
├── agent.h / .cpp     — Agent core: init, eval_prompt, generate, handle_tool_call
├── kvcache.h / .cpp   — Persistenza KVCache: token/state save/load, CacheMode
├── tools.h / .cpp     — Tool calling: 7 tools, JSON parsing, ToolRegistry
├── ui.h / .cpp        — UI base class + SimpleUI (ANSI console)
├── ui_ftxui.cpp       — FTXUI TUI: split pane, scroll, colori, permessi overlay
├── permissions.h/.cpp — Sistema permessi gerarchico (globale, per-tool, per-pattern)
├── reasoning.h/.cpp   — Rilevamento thinking/response (tag + euristiche)
└── streaming.h/.cpp   — Buffer streaming token thread-safe

toolstest/
├── test_tools.cpp     — 37 test per il parser JSON tool call
└── test_token_roundtrip.cpp — Test roundtrip tokenizzazione
```

---

## Test

```bash
cd toolstest/build && ./test_tools
```

37 parser test passano. Verificano: JSON diretto, markdown code block, stringhe con `{}` e `"`, escape JSON, falsi key-value, key mancanti di caratteri.

---

## Note

- La compilazione Debug (`./build.sh debug`) è estremamente lenta — usare solo per sviluppo.
- Su CPU senza BLAS, la prima valutazione del prompt richiede minuti (soprattutto modelli >2B). La fast cache risolve il problema nei riavvii successivi.
- Modelli piccoli (<3B) possono occasionalmente generare JSON malformato (es. caratteri iniziali delle chiavi mancanti). Il parser robusto gestisce molti casi, ma alcuni sono limiti del modello.
