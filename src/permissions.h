#ifndef LLAMA_AGENT_PERMISSIONS_H
#define LLAMA_AGENT_PERMISSIONS_H

#include <string>
#include <vector>
#include <map>
#include <set>
#include <functional>

/**
 * Azione consentita per un tool.
 */
enum class PermissionAction {
    ALLOW,  // Esegui automaticamente
    ASK,    // Chiedi conferma all'utente
    DENY    // Nega sempre
};

/**
 * Regola di permesso associata a un pattern.
 */
struct PermissionRule {
    std::string pattern;    // Pattern di corrispondenza (es. "*.txt", "/etc/*")
    PermissionAction action;
};

/**
 * Configurazione dei permessi per un tool specifico.
 */
struct ToolPermission {
    PermissionAction default_action = PermissionAction::ASK;
    std::vector<PermissionRule> rules;
};

/**
 * Gestore dei permessi per l'esecuzione dei tool.
 *
 * Implementa un sistema a cascata:
 * 1. Regole specifiche per tool + pattern (es. bash con percorso "*.key" → deny)
 * 2. Regola di default per tool (es. read → allow)
 * 3. Regola globale di default (es. ask)
 *
 * Le risposte dell'utente ("always allow for this session") vengono
 * memorizzate per evitare richieste ripetute.
 */
class PermissionManager {
public:
    PermissionManager();

    /**
     * Controlla se un'azione è consentita.
     * @param tool_name Nome del tool (es. "bash", "read")
     * @param resource Risorsa specifica (es. percorso file, comando)
     * @return ALLOW, ASK, o DENY
     */
    PermissionAction check(const std::string & tool_name,
                           const std::string & resource = "");

    /**
     * Imposta il permesso di default per un tool.
     */
    void set_default(const std::string & tool_name, PermissionAction action);

    /**
     * Aggiunge una regola per un tool specifico.
     * @param tool_name Nome del tool
     * @param pattern Pattern di percorso (es. "/home/*", "*.txt")
     * @param action Azione da intraprendere
     */
    void add_rule(const std::string & tool_name,
                  const std::string & pattern,
                  PermissionAction action);

    /**
     * Registra una risposta positiva persistente per la sessione.
     * Dopo questa chiamata, check() restituirà ALLOW senza chiedere.
     */
    void allow_permanently(const std::string & tool_name,
                           const std::string & resource);

    /**
     * Resetta tutte le autorizzazioni permanenti della sessione.
     */
    void reset_session();

    /**
     * Imposta il permesso globale di default.
     */
    void set_global_default(PermissionAction action);

private:
    PermissionAction global_default_ = PermissionAction::ASK;
    std::map<std::string, ToolPermission> tool_permissions_;
    std::map<std::string, std::set<std::string>> session_allowed_;  // tool → {resources}

    /**
     * Verifica se un percorso corrisponde a un pattern glob.
     */
    bool match_pattern(const std::string & pattern,
                       const std::string & resource) const;
};

#endif // LLAMA_AGENT_PERMISSIONS_H
