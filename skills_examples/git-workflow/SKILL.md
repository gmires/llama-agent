# Git Workflow

## Commit Conventions
- Messages in italiano o inglese, prima lettera maiuscola
- Formato: `type: breve descrizione`
- Types: `feat`, `fix`, `refactor`, `docs`, `test`, `build`, `chore`
- Esempi:
  - `feat: aggiunto tool tree per visualizzazione directory`
  - `fix: corretto overflow testo nella FTXUI`
  - `docs: aggiornato README con 20 tools`

## Common Commands
```bash
# Stato
git status
git diff                    # modifiche non staged
git diff --staged           # modifiche staged
git log --oneline -n 10     # ultimi 10 commit

# Branch
git branch                  # lista branch locali
git checkout -b feat/nome   # crea e switcha nuovo branch

# Commit
git add src/file.cpp        # stage specifico
git commit -m "messaggio"   # commit
git push origin branch      # push

# Undo
git reset HEAD file         # unstage
git checkout -- file        # scarta modifiche
git reset --soft HEAD~1     # undo ultimo commit (keep changes)
```

## Branch Strategy
- `main` — stabile, sempre compilabile
- `feat/nome` — nuove feature
- `fix/nome` — bug fix
- Mai pushare su `main` direttamente; usa PR o merge locale

## Before Commit
- `./build.sh release` compila senza errori
- `cd toolstest/build && ./test_tools` tutti i test passano
- `grep -r "TODO\|FIXME" src/` nessun TODO incompleto
