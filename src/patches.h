#ifndef LLAMA_AGENT_PATCHES_H
#define LLAMA_AGENT_PATCHES_H

#include <string>
#include <vector>
#include <optional>

/**
 * Risultato dell'applicazione di una patch.
 */
struct PatchResult {
    bool ok;
    std::string modified;   // contenuto modificato (se ok)
    std::string error;      // messaggio errore (se !ok)
};

/**
 * Applica una unified diff a un contenuto testuale.
 *
 * Supporta il formato standard:
 *   --- a/file  (opzionale)
 *   +++ b/file  (opzionale)
 *   @@ -L,C +L,C @@  (hunk header)
 *    context
 *   -old line
 *   +new line
 *
 * @param content  Contenuto originale del file
 * @param diff     Stringa diff in formato unified
 * @return PatchResult con contenuto modificato o errore
 */
PatchResult apply_unified_diff(const std::string & content, const std::string & diff);

#endif
