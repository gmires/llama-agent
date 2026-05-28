#ifndef LLAMA_AGENT_TOOLS_H
#define LLAMA_AGENT_TOOLS_H

#include <string>
#include <vector>
#include <map>
#include <functional>
#include <memory>

/**
 * Risultato dell'esecuzione di un tool.
 */
struct ToolResult {
    bool success;
    std::string output;     // Testo prodotto dal tool (stdout, contenuto file, ecc.)
    std::string error;      // Messaggio di errore se success == false
};

/**
 * Schema di un parametro di un tool (formato JSON-like per la serializzazione).
 */
struct ToolParamSchema {
    std::string name;
    std::string type;       // "string", "number", "boolean", "array"
    std::string description;
    bool required = true;
};

/**
 * Definizione di un tool.
 */
struct ToolDefinition {
    std::string name;                         // Nome del tool (es. "bash")
    std::string description;                  // Descrizione per il LLM
    std::vector<ToolParamSchema> parameters;  // Parametri accettati

    // Esecutore: funzione che prende parametri JSON e restituisce risultato
    std::function<ToolResult(const std::map<std::string, std::string> &)> executor;
};

/**
 * Registro dei tool disponibili per l'agente.
 *
 * Gestisce la definizione, la validazione e l'esecuzione dei tool.
 * I tool vengono descritti al LLM tramite il system prompt in formato
 * JSON function-calling compatibile con i modelli GGUF pi recenti.
 */
class ToolRegistry {
public:
    ToolRegistry();

    /**
     * Registra un nuovo tool.
     */
    void register_tool(const ToolDefinition & tool);

    /**
     * Trova un tool per nome.
     * Restituisce nullptr se non trovato.
     */
    const ToolDefinition * find(const std::string & name) const;

    /**
     * Esegue un tool per nome con i parametri specificati.
     */
    ToolResult execute(const std::string & name,
                       const std::map<std::string, std::string> & args);

    /**
     * Genera la descrizione JSON dei tool per il system prompt.
     * Formato compatibile con OpenAI function calling.
     */
    std::string to_json_schema() const;

    /**
     * Restituisce tutti i tool registrati.
     */
    const std::vector<ToolDefinition> & get_all() const { return tools_; }

    /**
     * Restituisce una stringa con i nomi dei tool separati da virgola.
     */
    std::string list_tool_names() const;

    /**
     * Estrae una tool call da una stringa di risposta del LLM.
     * Supporta formati: JSON diretto e markdown ```json ... ```
     * Restituisce true se trovata, popola name e args.
     */
    bool parse_tool_call(const std::string & response,
                         std::string & out_name,
                         std::map<std::string, std::string> & out_args);

    /**
     * Debug: restituisce il blocco JSON estratto (per diagnostica).
     */
    std::string extract_json_block_debug(const std::string & text) const {
        return extract_json_block(text);
    }

private:
    std::vector<ToolDefinition> tools_;
    std::map<std::string, size_t> tool_index_;  // name → indice in tools_

    /**
     * Cerca un pattern di tool call JSON nel testo.
     */
    std::string extract_json_block(const std::string & text) const;
};

#endif // LLAMA_AGENT_TOOLS_H
