#include "permissions.h"

#include <algorithm>
#include <set>

/*
 * ============================================================================
 * PermissionManager: sistema di permessi per l'esecuzione dei tool.
 *
 * Ispirato al sistema di permessi di opencode, permette di controllare
 * quali azioni l'agente puo' eseguire senza supervisione.
 *
 * Gerarchia di valutazione (dalla piu' specifica alla piu' generale):
 * 1. Regole tool + pattern (es. bash con "rm -rf /"  DENY)
 * 2. Permesso permanente di sessione (es. l'utente ha detto "always allow")
 * 3. Default per tool (es. read -> ALLOW, bash -> ASK)
 * 4. Default globale (ASK)
 * ============================================================================
 */

PermissionManager::PermissionManager()
{
    // Default: permessi ragionevoli per ogni tool
    set_default("read", PermissionAction::ALLOW);
    set_default("glob", PermissionAction::ALLOW);
    set_default("grep", PermissionAction::ALLOW);
    set_default("write", PermissionAction::ALLOW);
    set_default("bash", PermissionAction::ASK);

    // Regole speciali: comandi pericolosi sempre negati
    add_rule("bash", "rm -rf *", PermissionAction::DENY);
    add_rule("bash", "rm -rf /*", PermissionAction::DENY);
    add_rule("bash", "dd if=*", PermissionAction::DENY);
    add_rule("bash", "> /dev/sd*", PermissionAction::DENY);
    add_rule("bash", "mkfs*", PermissionAction::DENY);
    add_rule("bash", "chmod 777 *", PermissionAction::DENY);
}

PermissionAction PermissionManager::check(
    const std::string & tool_name,
    const std::string & resource)
{
    // 0. Auto-allow globale (--yes flag)
    if (auto_allow_) return PermissionAction::ALLOW;

    // 1. Verifica i permessi permanenti di sessione
    const auto session_it = session_allowed_.find(tool_name);
    if (session_it != session_allowed_.end()) {
        if (resource.empty() || session_it->second.count(resource) ||
            session_it->second.count("*")) {
            return PermissionAction::ALLOW;
        }
    }

    // 2. Verifica le regole specifiche del tool
    const auto tool_it = tool_permissions_.find(tool_name);
    if (tool_it != tool_permissions_.end()) {
        // Cerca prima le regole con pattern
        for (const auto & rule : tool_it->second.rules) {
            if (match_pattern(rule.pattern, resource)) {
                return rule.action;
            }
        }
    }

    // 3. Default del tool
    if (tool_it != tool_permissions_.end()) {
        return tool_it->second.default_action;
    }

    // 4. Default globale
    return global_default_;
}

void PermissionManager::set_default(const std::string & tool_name,
                                    PermissionAction action)
{
    tool_permissions_[tool_name].default_action = action;
}

void PermissionManager::add_rule(const std::string & tool_name,
                                 const std::string & pattern,
                                 PermissionAction action)
{
    tool_permissions_[tool_name].rules.push_back({pattern, action});
}

void PermissionManager::allow_permanently(const std::string & tool_name,
                                          const std::string & resource)
{
    session_allowed_[tool_name].insert(resource.empty() ? "*" : resource);
}

void PermissionManager::reset_session()
{
    session_allowed_.clear();
}

void PermissionManager::set_global_default(PermissionAction action)
{
    global_default_ = action;
}

bool PermissionManager::match_pattern(const std::string & pattern,
                                      const std::string & resource) const
{
    if (resource.empty()) {
        return false;
    }

    // Pattern semplici:
    // - "*" matcha tutto
    // - "*.txt" matcha file con estensione .txt
    // - "/etc/*" matcha tutto sotto /etc/

    if (pattern == "*") {
        return true;
    }

    // Pattern con wildcard alla fine: "/etc/*"
    if (pattern.size() > 1 && pattern.back() == '*') {
        std::string prefix = pattern.substr(0, pattern.size() - 1);
        if (resource.compare(0, prefix.size(), prefix) == 0) {
            return true;
        }
    }

    // Pattern con estensione: "*.txt"
    if (pattern.size() > 1 && pattern.front() == '*') {
        std::string suffix = pattern.substr(1);
        if (resource.size() >= suffix.size() &&
            resource.compare(resource.size() - suffix.size(), suffix.size(), suffix) == 0) {
            return true;
        }
    }

    // Match letterale
    if (pattern == resource) {
        return true;
    }

    return false;
}
