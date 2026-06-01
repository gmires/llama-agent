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
| `--tool-limit N` | `0` | Limite tool call per turno (0 = illimitato) |
| `--tool-log` | off | Log dettagliato delle tool call su stderr (debug) |
| `--yes`, `-y` | off | Auto-consenti tutti i permessi, non chiedere mai |
| `--single-turn` | off | Elabora il prompt `-p` ed esce (modalità non interattiva) |

Tutti i flag di [llama-cli](https://github.com/ggml-org/llama.cpp/blob/master/examples/main/README.md) sono supportati:
`--temp`, `--top-k`, `--top-p`, `--seed`, `-c` (ctx-size), `--threads`, `-ngl` (GPU layers), ecc.

### Comandi nella UI

| Comando | Azione |
|---------|--------|
| `/help` | Mostra aiuto e tool disponibili |
| `/clear` | Cancella cronologia e cache |
| `/regen` | Rigenera ultima risposta |
| `/compact` | Compatta contesto (preserva ultimi messaggi) |
| `/model` | Mostra modello e parametri correnti |
| `/session` | Mostra info sessione (messaggi, token, cache) |
| `/stats` | Statistiche dettagliate (CPU, GPU, sampling) |
| `/exit` | Esci |

### Tasti

| Tasto | Azione |
|-------|--------|
| `Enter` | Invia messaggio |
| `Ctrl+Enter` / `Ctrl+J` | Nuova riga nell'input |
| `←` / `→` | Muovi cursore nell'input |
| `Ctrl+A` / `Ctrl+E` | Inizio/fine riga |
| `Ctrl+W` | Cancella parola |
| `Delete` | Cancella carattere a destra |
| `Tab` | Autocompletamento percorso file |
| `PgUp` / `PgDn` | Scroll cronologia (o rotella mouse) |
| `Home` / `End` | Inizio/fine input (se testo) o scroll estremi |
| `Su` / `Giù` | Cronologia prompt precedenti |
| `Ctrl+T` | Comprimi/espandi blocchi Thinking |
| `Esc` | Interrompe la generazione in corso |

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

### Tool disponibili (20)

| Tool | Descrizione | Parametri |
|------|-------------|-----------|
| `bash` | Esegue comandi shell (2>&1) con timeout configurabile | `command`, `timeout` |
| `read` | Legge file di testo (max 64KB) | `path` |
| `write` | Scrive/crea file di testo | `path`, `content` |
| `edit` | Sostituisce stringa in file (match unico) | `path`, `old_string`, `new_string` |
| `grep` | Cerca pattern regex nei file (ricorsivo) | `pattern`, `path` |
| `glob` | Trova file per pattern glob con brace expansion | `pattern` |
| `find` | Ricerca file nativa C++ (fnmatch + brace expansion) | `path`, `pattern`, `type`, `max_depth` |
| `ls` | Elenca directory (tipo, dimensione, nome) | `path` |
| `tree` | Visualizzazione ricorsiva ad albero | `path`, `max_depth` (default: 3) |
| `rm` | Elimina file | `path` |
| `mv` | Sposta/rinomina file o directory | `from`, `to` |
| `fetch` | Scarica URL via curl | `url`, `format`, `timeout` |
| `web_search` | Cerca su DuckDuckGo (titolo, URL, snippet) | `query`, `num` |
| `git_diff` | Diff working tree (--stat default) | `stat` |
| `git_log` | Ultimi commit (--oneline) | `n` (default: 10) |
| `git_status` | Stato working tree (--short) | — |
| `git_branch` | Branch locali | — |
| `task_create` | Crea task in `.cache/tasks.json` | `title`, `description` |
| `task_list` | Mostra task: `[ ]` todo, `[~]` progress, `[x]` done | — |
| `task_update` | Aggiorna stato task (accetta `#1` o `1`) | `id`, `status` |

### Hook system

Due hook intercettano ogni esecuzione di tool:

- **before hook**: eseguito prima del tool. Può bloccare l'esecuzione (es. path assoluti bloccati per `write`/`rm`/`edit`). Se ritorna `is_error=true`, il tool non viene eseguito.
- **after hook**: eseguito dopo il tool. Può modificare il risultato, troncare output lunghi (>16KB), aggiungere metadati (`details["tool"]`, `details["truncated"]`).

### Compattazione contesto (Summary Compression)

Quando il contesto raggiunge l'80% della capacità, invece di resettare completamente, l'agente:

1. Scansiona i messaggi scartati con `parse_tool_call` per estrarre tutte le operazioni
2. Cataloga: file letti, creati, modificati, eliminati, ricerche, errori
3. Costruisce un summary strutturato con sezioni (`Work performed`, `Files created`, `Searches`, `Errors`)
4. Preserva gli ultimi ~1/3 dei messaggi
5. Ricostruisce la KVCache con system prompt + summary + messaggi recenti

Il modello conserva così il contesto di ciò che ha fatto senza perdere traccia dei file e delle decisioni prese.

Comando manuale: `/compact` forza la compattazione immediata.

### Config file JSON

Crea `.llama-agent.json` nella root del progetto:

```json
{
  "model": "~/models/qwen2.5-7b.gguf",
  "cache_mode": "fast",
  "n_ctx": 8192,
  "n_predict": 8192,
  "temperature": 0.7,
  "n_gpu_layers": 35,
  "tool_limit": 0
}
```

Caricamento: `~/.config/llama-agent/config.json` (globale) + `.llama-agent.json` (progetto). Il progetto sovrascrive il globale. I flag CLI hanno priorità sul file.

### Skills system

Le skills estendono le capacità del modello con istruzioni specifiche, senza scrivere codice.

Crea una directory `.skills/nome_skill/SKILL.md`:

```markdown
# C++ Coding Style
Usa sempre snake_case per variabili e funzioni.
Usa `const auto &` per i parametri.
```

Comandi:
- `/skill` — elenca le skill disponibili
- `/skill:nome` — mostra il contenuto di una skill

Le skill vengono iniettate nel system prompt e il modello può usarle come riferimento.

Vedi `skills_examples/` per esempi pronti all'uso (C++, Python, Rust, Git).

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

Interfaccia moderna con messaggi strutturati ispirata a pi:

- **Messaggi strutturati**: ogni messaggio ha un tipo (USER, ASSISTANT, THINKING, TOOL_CALL, TOOL_RESULT, SYSTEM) e uno stile visivo distinto
- **Tool call a blocchi**: `▸ Tool: write (path="...", ...)` in blu bold, risultato `│ OK: 523 bytes` in blu dim
- **Thinking collassabile**: `▼ Thinking` / `▶ Thinking` con toggle `Ctrl+T`
- **Code block**: ` ``` ` blocchi con sfondo grigio e testo cyan
- **Colori per ruolo**: utente in verde bold `❯`, assistant in bianco, heading in giallo, sistema con sfondo grigio
- **Input area**: prompt `❯ ` verde, cursore visibile (reverse video), supporto multilinea, hint comandi slash
- **Tab completion**: autocompletamento percorsi file nel filesystem
- **Footer**: token generati, T/s, contesto (n_past/n_ctx), hint tasti (←→ cursore, Tab=completa, Ctrl+T=thinking)
- **Scroll**: PgUp/PgDn/Home/End + rotella mouse
- **Spinner**: `|/-\` animato durante la generazione
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
├── tools.h / .cpp     — Tool calling: 19 tools, JSON parsing, ToolRegistry, hooks

toolstest/
├── test_tools.cpp     — 104 test (parser, tool exec, hooks, git, task, permissions)
└── test_token_roundtrip.cpp — Test roundtrip tokenizzazione
```

---

## Test

```bash
cd toolstest/build && ./test_tools
```

**104 test**: 37 parser JSON, 12 tool execution, 7 hook, 7 git/task, permissions. Verificano: parsing robusto (stringhe con `{}`, escape, falsi key-value), esecuzione tool file-system, git, task CRUD, hook chain, gestione permessi.

---

## Note

- La compilazione Debug (`./build.sh debug`) è estremamente lenta — usare solo per sviluppo.
- Su CPU senza BLAS, la prima valutazione del prompt richiede minuti (soprattutto modelli >2B). La fast cache risolve il problema nei riavvii successivi.
- Modelli piccoli (<3B) possono occasionalmente generare JSON malformato (es. caratteri iniziali delle chiavi mancanti). Il parser robusto gestisce molti casi, ma alcuni sono limiti del modello.
