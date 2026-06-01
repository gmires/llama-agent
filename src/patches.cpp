#include "patches.h"

#include <sstream>
#include <vector>
#include <cstdio>

/*
 * ============================================================================
 * Unified diff parser e applicatore.
 *
 * Algoritmo:
 * 1. Parsing riga per riga: identifica hunk header (@@...@@)
 * 2. Per ogni hunk, trova la riga nel file originale
 * 3. Verifica che le righe di contesto matchino
 * 4. Applica le modifiche (rimuovi old, inserisci new)
 * 5. Se un hunk fallisce, riporta errore specifico
 * ============================================================================
 */

struct Hunk {
    int old_start;  // 1-indexed
    int old_count;
    int new_start;  // 1-indexed
    int new_count;

    // Righe dell'hunk: '-' rimuovi, '+' aggiungi, ' ' contesto
    std::vector<std::pair<char, std::string>> lines;
};

// Parsa @@ -L,C +L,C @@
static bool parse_hunk_header(const std::string & line, Hunk & h) {
    int os = 0, oc = 1, ns = 0, nc = 1;
    // Supporta: @@ -10 +12 @@  o  @@ -10,5 +12,7 @@
    // Il formato più comune: @@ -old_start[,old_count] +new_start[,new_count] @@
    const char * p = line.c_str();
    while (*p && *p != '@') p++;
    if (!*p) return false;
    // skip @@
    p++; if (*p == '@') p++;
    while (*p == ' ') p++;
    // parse -old_start[,old_count]
    if (*p != '-') return false;
    p++;
    os = 0;
    while (*p >= '0' && *p <= '9') { os = os * 10 + (*p - '0'); p++; }
    if (*p == ',') { p++; oc = 0; while (*p >= '0' && *p <= '9') { oc = oc * 10 + (*p - '0'); p++; } }
    while (*p == ' ') p++;
    // parse +new_start[,new_count]
    if (*p != '+') return false;
    p++;
    ns = 0;
    while (*p >= '0' && *p <= '9') { ns = ns * 10 + (*p - '0'); p++; }
    if (*p == ',') { p++; nc = 0; while (*p >= '0' && *p <= '9') { nc = nc * 10 + (*p - '0'); p++; } }

    h.old_start = os;
    h.old_count = oc > 0 ? oc : 1;
    h.new_start = ns;
    h.new_count = nc > 0 ? nc : 1;
    return h.old_start > 0;
}

PatchResult apply_unified_diff(const std::string & content, const std::string & diff) {
    // Split contenuto in righe (1-indexed per match con hunk)
    // NOTA: preserviamo il trailing newline come riga vuota finale
    std::vector<std::string> lines;
    {
        std::istringstream ss(content);
        std::string l;
        while (std::getline(ss, l)) lines.push_back(l);
        // Se il contenuto termina con \n, aggiungi una riga vuota
        if (!content.empty() && content.back() == '\n')
            lines.push_back("");
    }

    // Parsing diff
    std::vector<Hunk> hunks;
    Hunk current;
    bool in_hunk = false;
    int hunk_expected_old = 0;  // contatore per verificare old_count
    int hunk_expected_new = 0;

    {
        std::istringstream ss(diff);
        std::string l;
        while (std::getline(ss, l)) {
            while (!l.empty() && l.back() == '\r') l.pop_back();

            if (l.size() >= 4 && l[0] == '@' && l[1] == '@') {
                if (in_hunk) hunks.push_back(current);
                current = Hunk{};
                in_hunk = true;
                hunk_expected_old = 0;
                hunk_expected_new = 0;
                if (!parse_hunk_header(l, current)) {
                    return {false, "", "Hunk header non valido: " + l};
                }
            } else if (in_hunk) {
                if (l.empty()) {
                    // riga vuota in un hunk = contesto vuoto
                    current.lines.push_back({' ', ""});
                } else if (l[0] == ' ' || l[0] == '-' || l[0] == '+') {
                    current.lines.push_back({l[0], l.substr(1)});
                    if (l[0] == ' ' || l[0] == '-') hunk_expected_old++;
                    if (l[0] == ' ' || l[0] == '+') hunk_expected_new++;
                } else if (l[0] == '\\') {
                    // "No newline at end of file" — ignoriamo
                }
                // Altre righe (es. ---, +++, commenti) sono ignorate dentro l'hunk
            }
            // Righe prima del primo hunk sono ignorate
        }
        if (in_hunk) hunks.push_back(current);
    }

    if (hunks.empty()) {
        return {false, "", "Nessun hunk trovato nel diff"};
    }

    // Applica gli hunk (dall'ultimo al primo per non shiftare gli indici)
    for (int hi = (int)hunks.size() - 1; hi >= 0; hi--) {
        const Hunk & h = hunks[hi];

        // Converti old_start da 1-indexed a 0-indexed
        int idx = h.old_start - 1;
        if (idx < 0) {
            return {false, "", "Hunk #" + std::to_string(hi + 1) +
                    " old_start non valido: " + std::to_string(h.old_start)};
        }
        if (idx + h.old_count > (int)lines.size()) {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "Hunk #%d fallito: le righe %d-%d eccedono la fine del file (%zu righe)",
                     hi + 1, h.old_start, h.old_start + h.old_count - 1, lines.size());
            return {false, "", buf};
        }

        // Verifica contesto: ogni riga ' ' o '-' deve matchare
        int old_idx = idx;
        for (const auto & [ch, text] : h.lines) {
            if (ch == ' ') {
                if (old_idx >= (int)lines.size() || lines[old_idx] != text) {
                    char buf[256];
                    snprintf(buf, sizeof(buf),
                             "Hunk #%d fallito: contesto non matcha alla riga %d.\n"
                             "  Atteso:  %s\n  Trovato: %s",
                             hi + 1, old_idx + 1, text.c_str(),
                             old_idx < (int)lines.size() ? lines[old_idx].c_str() : "(EOF)");
                    return {false, "", buf};
                }
                old_idx++;
            } else if (ch == '-') {
                if (old_idx >= (int)lines.size() || lines[old_idx] != text) {
                    char buf[256];
                    snprintf(buf, sizeof(buf),
                             "Hunk #%d fallito: riga da rimuovere non matcha alla riga %d.\n"
                             "  Atteso: -%s\n  Trovato: %s",
                             hi + 1, old_idx + 1, text.c_str(),
                             old_idx < (int)lines.size() ? lines[old_idx].c_str() : "(EOF)");
                    return {false, "", buf};
                }
                old_idx++;
            }
            // '+' non ha corrispondenza nell'originale
        }

        // Applica modifiche: rimuovi old_count righe, inserisci new_count righe
        // Estrai le nuove righe (tipo '+')
        std::vector<std::string> new_lines;
        for (const auto & [ch, text] : h.lines) {
            if (ch == ' ') new_lines.push_back(text);
            else if (ch == '+') new_lines.push_back(text);
            // '-' è rimosso (non lo aggiungiamo)
        }

        // Rimuovi le vecchie righe e inserisci le nuove
        lines.erase(lines.begin() + idx, lines.begin() + idx + h.old_count);
        lines.insert(lines.begin() + idx, new_lines.begin(), new_lines.end());
    }

    // Ricostruisci il contenuto
    std::string result;
    for (size_t i = 0; i < lines.size(); i++) {
        result += lines[i];
        if (i < lines.size() - 1) result += '\n';
    }

    return {true, result, ""};
}
