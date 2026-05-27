# llama-agent

Agente conversazionale AI con terminale TUI, basato su [llama.cpp](https://github.com/ggml-org/llama.cpp).

## Dipendenze

- C++17
- CMake ≥ 3.14
- OpenSSL

## Compilazione

```bash
# Build Release (consigliato)
./build.sh release

# Build Debug
./build.sh debug
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
| `--simple-ui` | off | Usa interfaccia testuale semplice (senza FTXUI) |

## Cache persistente

La KVCache viene salvata su disco dopo ogni conversazione.
All'avvio successivo viene ricostruita automaticamente, permettendo risposte veloci senza dover rivalutare l'intero contesto.

## Codice

```
src/
  main.cpp           — Entry point
  agent.h / .cpp     — Agente conversazionale
  kvcache.h / .cpp   — Cache persistente su disco
  tools.h / .cpp     — Tool calling
  ui.h / .cpp        — Interfaccia base
  ui_ftxui.cpp       — Interfaccia FTXUI (TUI moderna)
  permissions.h/.cpp — Gestione permessi tool
  reasoning.h/.cpp   — Rilevamento pensiero
  streaming.h/.cpp   — Buffer streaming
```

## Note

- Richiede `-march=native` per prestazioni ottimali su CPU.
- Compilazione Debug (`./build.sh debug`) è estremamente lenta — usare solo per sviluppo.
