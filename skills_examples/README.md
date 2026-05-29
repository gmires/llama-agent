# Skills Examples

Questa directory contiene skill di esempio per llama-agent.
Ogni skill è una cartella con un file `SKILL.md`.

## Come usarle

Copia la skill che ti interessa nella directory `.skills/` del tuo progetto:

```bash
cp -r skills_examples/cpp-coding-style .skills/
```

Oppure globalmente:

```bash
cp -r skills_examples/cpp-coding-style ~/.config/llama-agent/skills/
```

Poi nell'agente:
- `/skill` — elenca le skill caricate
- `/skill:cpp-coding-style` — mostra il contenuto

## Skill disponibili

| Skill | Descrizione |
|-------|-------------|
| `cpp-coding-style` | Convenzioni C++ per il progetto (naming, include, patterns, struttura file) |
| `python-coding-style` | Convenzioni Python (naming, type hints, imports, tool scripts) |
| `git-workflow` | Convenzioni git (commit, branch, comandi comuni, pre-commit checklist) |
| `rust-coding-style` | Convenzioni Rust (naming, Cargo.toml, patterns, testing) |

## Creare una nuova skill

1. Crea una directory con il nome della skill:
   ```bash
   mkdir -p .skills/mia-skill
   ```

2. Scrivi un file `SKILL.md` con le istruzioni:
   ```markdown
   # Mia Skill
   Descrizione di cosa fa questa skill.
   
   ## Istruzioni
   1. Primo passo
   2. Secondo passo
   
   ## Convenzioni
   - Regola 1
   - Regola 2
   ```

3. Riavvia llama-agent o usa `/reload` (se implementato).

Le skill vengono automaticamente iniettate nel system prompt e il modello le userà come riferimento quando pertinenti.
