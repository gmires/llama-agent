# llama-agent

Agente conversazionale AI con terminale TUI, basato su [llama.cpp](https://github.com/ggml-org/llama.cpp).

## Dipendenze

- C++17, CMake ≥ 3.14, OpenSSL

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

Esempio con Gemma 4:
```bash
./build/llama-agent -m ../models/gemma-4-E2B-it-UD-Q5_K_XL.gguf
```

### Comandi nella UI

| Tasto | Azione |
|-------|--------|
| `Enter` | Invia messaggio |
| `Ctrl+Enter` | Nuova riga nell'input |
| `PgUp` / `PgDn` | Scroll cronologia |
| `Su` / `Giù` | Cronologia prompt |

### Comandi testuali

- `/help` — Mostra aiuto
- `/clear` — Cancella cronologia
- `/regen` — Rigenera ultima risposta
- `/exit` — Esci

### Opzioni CLI

Tutte le opzioni di [llama-cli](https://github.com/ggml-org/llama.cpp/blob/master/examples/main/README.md) sono supportate:

| Opzione | Default | Descrizione |
|---------|---------|-------------|
| `-m` | — | Percorso modello GGUF |
| `--threads` | `6` | Thread CPU |
| `--n-ctx` | `2048` | Dimensione contesto |
| `--temp` | `0.8` | Temperatura sampling |
| `--simple-ui` | off | Interfaccia testuale semplice (senza FTXUI) |
| `--no-cache` | off | Disabilita cache persistente |

## Tool calling

L'agente può eseguire strumenti in modo autonomo. Il modello decide quando usarli
emettendo un blocco JSON nel formato:

```json
{"tool": "nome", "args": {"param1": "valore"}}
```

### Tool disponibili

| Tool | Descrizione |
|------|-------------|
| `bash` | Esegue comandi shell (script, compilazioni, fs) |
| `read` | Legge file di testo (codice, configurazioni) |
| `write` | Scrive file di testo (ATTENZIONE: sovrascrive) |
| `grep` | Cerca contenuti nei file con regex |
| `glob` | Trova file per pattern (`**/*.cpp`, `*.txt`, ...) |
| `fetch` | Scarica URL (documentazione, API, pagine web) |

### Esempio di sessione

```
> Cerca nel codice tutti i file .h e contami quanti sono
  [Il modello usa glob per trovare i file, poi bash per contarli]
  Risultato: trovati 42 file .h nel progetto.

> Leggi il file src/agent.h e dimmi cosa contiene
  [Il modello usa read per leggere il file]
  Il file contiene la definizione della classe Agent con i metodi...

> Aggiungi un coment in testa al CMakeLists.txt
  [Il modello usa read per leggere, poi write per scrivere]
  Fatto, ho aggiunto il commento.
```

## Cache persistente

La KVCache viene salvata su disco dopo ogni conversazione.
All'avvio successivo viene ricostruita automaticamente con barra di progresso,
permettendo risposte veloci senza dover rivalutare l'intero contesto.

Disabilita con `--no-cache`.

### Esempio di flusso tipico

```bash
# Prima esecuzione (lenta: valuta l'intero prompt ~900 token su CPU)
./build/llama-agent -m ../models/gemma-4-E2B-it-UD-Q5_K_XL.gguf

# ... conversazione normale ...
# La cache viene salvata automaticamente alla fine

# Seconda esecuzione (veloce: ricostruisce la cache e valuta solo i nuovi token)
./build/llama-agent -m ../models/gemma-4-E2B-it-UD-Q5_K_XL.gguf
# -> Barra di progresso: [=========>] 100%
# -> Prompt successivi: immediati
```

## Test

```bash
cd testp && ./build/test_tools   # Test suite strumenti
```

## Codice

```
src/
  main.cpp           — Entry point, CLI
  agent.h / .cpp     — Agente conversazionale (init, prompt, generazione)
  kvcache.h / .cpp   — Cache persistente su disco
  tools.h / .cpp     — Tool calling (web, file, comandi)
  ui.h / .cpp        — Interfaccia base + SimpleUI
  ui_ftxui.cpp       — Interfaccia FTXUI (TUI moderna con split, scroll, colori)
  permissions.h/.cpp — Gestione permessi tool
  reasoning.h/.cpp   — Rilevamento pensiero (thinking/response)
  streaming.h/.cpp   — Buffer streaming token
```

## Note

- La compilazione Debug (`./build.sh debug`) è estremamente lenta — usare solo per sviluppo.
- Su CPU senza BLAS, la prima valutazione del prompt richiede tempo (soprattutto con modelli >2B). La cache persistente risolve il problema nelle sessioni successive.
