#include "tools.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <regex>

/*
 * ============================================================================
 * ToolRegistry: registro e dispatcher di strumenti per l'agente.
 *
 * Ogni tool ha un nome, una descrizione, uno schema dei parametri e un
 * esecutore. I tool vengono descritti al LLM nel system prompt in formato
 * JSON function-calling.
 *
 * Tool disponibili:
 * - bash: esegue comandi shell
 * - read: legge file di testo
 * - write: scrive file di testo
 * - grep: ricerca contenuti nei file
 * - glob: trova file per pattern
 *
 * Formato tool call (JSON):
 * {
 *   "tool": "bash",
 *   "args": { "command": "ls -la" }
 * }
 * ============================================================================
 */

ToolRegistry::ToolRegistry()
{
    // --- Tool: bash ---
    register_tool({
        "bash",
        "Esegue un comando shell e restituisce l'output. "
        "Utile per eseguire comandi, script, compilazioni, ecc.",
        {
            {"command", "string", "Il comando shell da eseguire", true}
        },
        [](const std::map<std::string, std::string> & args) -> ToolResult {
            const auto it = args.find("command");
            if (it == args.end()) {
                return {false, "", "Parametro 'command' mancante"};
            }

            // Esegue il comando via popen e cattura l'output
            // NOTA: in produzione usare qualcosa di piu' robusto (subprocess)
            FILE * pipe = popen(it->second.c_str(), "r");
            if (!pipe) {
                return {false, "", "Impossibile eseguire il comando: " + it->second};
            }

            std::string output;
            char buffer[4096];
            while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
                output += buffer;
            }
            int exit_code = pclose(pipe);

            // Limita la dimensione dell'output (protezione da output eccessivi)
            const size_t MAX_OUTPUT = 65536;
            if (output.size() > MAX_OUTPUT) {
                output.resize(MAX_OUTPUT);
                output += "\n... [output troncato a " + std::to_string(MAX_OUTPUT) + " bytes]";
            }

            if (exit_code != 0) {
                // Non consideriamo errore: il comando potrebbe fallire lecitamente
                // Aggiungiamo il codice di uscita all'output
                output += "\n[exit code: " + std::to_string(exit_code) + "]";
            }

            return {true, output, ""};
        }
    });

    // --- Tool: read ---
    register_tool({
        "read",
        "Legge il contenuto di un file di testo. "
        "Utile per esaminare codice, configurazioni, documenti.",
        {
            {"path", "string", "Percorso del file da leggere", true}
        },
        [](const std::map<std::string, std::string> & args) -> ToolResult {
            const auto it = args.find("path");
            if (it == args.end()) {
                return {false, "", "Parametro 'path' mancante"};
            }

            std::ifstream file(it->second);
            if (!file.is_open()) {
                return {false, "", "Impossibile aprire il file: " + it->second};
            }

            std::stringstream buffer;
            buffer << file.rdbuf();
            std::string content = buffer.str();

            // Limita la dimensione
            const size_t MAX_CONTENT = 65536;
            if (content.size() > MAX_CONTENT) {
                content.resize(MAX_CONTENT);
                content += "\n... [file troncato a " + std::to_string(MAX_CONTENT) + " bytes]";
            }

            return {true, content, ""};
        }
    });

    // --- Tool: write ---
    register_tool({
        "write",
        "Scrive contenuto in un file. "
        "Utile per creare o modificare file di codice, documenti, ecc. "
        "ATTENZIONE: sovrascrive il file esistente.",
        {
            {"path", "string", "Percorso del file da scrivere", true},
            {"content", "string", "Contenuto da scrivere nel file", true}
        },
        [](const std::map<std::string, std::string> & args) -> ToolResult {
            const auto it_path = args.find("path");
            const auto it_content = args.find("content");
            if (it_path == args.end()) {
                return {false, "", "Parametro 'path' mancante"};
            }
            if (it_content == args.end()) {
                return {false, "", "Parametro 'content' mancante"};
            }

            std::ofstream file(it_path->second);
            if (!file.is_open()) {
                return {false, "", "Impossibile scrivere il file: " + it_path->second};
            }

            file << it_content->second;
            file.close();

            return {true, "File scritto con successo: " + it_path->second + " (" +
                        std::to_string(it_content->second.size()) + " bytes)", ""};
        }
    });

    // --- Tool: grep ---
    register_tool({
        "grep",
        "Cerca un pattern regex nel contenuto dei file. "
        "Restituisce le righe corrispondenti con numeri di riga.",
        {
            {"pattern", "string", "Pattern regex da cercare", true},
            {"path", "string", "Percorso del file o directory (ricorsiva) in cui cercare", true}
        },
        [](const std::map<std::string, std::string> & args) -> ToolResult {
            const auto it_pat = args.find("pattern");
            const auto it_path = args.find("path");
            if (it_pat == args.end()) {
                return {false, "", "Parametro 'pattern' mancante"};
            }
            if (it_path == args.end()) {
                return {false, "", "Parametro 'path' mancante"};
            }

            // Delega al comando grep di sistema per semplicit
            const std::string cmd = "grep -rn --color=never '" + it_pat->second +
                                    "' '" + it_path->second + "' 2>&1 | head -200";

            FILE * pipe = popen(cmd.c_str(), "r");
            if (!pipe) {
                return {false, "", "Impossibile eseguire grep"};
            }

            std::string output;
            char buffer[4096];
            while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
                output += buffer;
            }
            pclose(pipe);

            if (output.empty()) {
                output = "[nessuna corrispondenza trovata]";
            }

            return {true, output, ""};
        }
    });

    // --- Tool: glob ---
    register_tool({
        "glob",
        "Trova file e directory usando un pattern glob. "
        "Supporta pattern come `**/*.cpp`, `src/**`, `*.txt`, ecc.",
        {
            {"pattern", "string", "Pattern glob per trovare i file", true}
        },
        [](const std::map<std::string, std::string> & args) -> ToolResult {
            const auto it = args.find("pattern");
            if (it == args.end()) {
                return {false, "", "Parametro 'pattern' mancante"};
            }

            // Delega al comando find di sistema
            const std::string cmd = "find . -path '" + it->second +
                                    "' 2>&1 | head -200";

            FILE * pipe = popen(cmd.c_str(), "r");
            if (!pipe) {
                return {false, "", "Impossibile eseguire find"};
            }

            std::string output;
            char buffer[4096];
            while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
                output += buffer;
            }
            pclose(pipe);

            if (output.empty()) {
                output = "[nessun file trovato]";
            }

            return {true, output, ""};
        }
    });

    // --- Tool: fetch ---
    register_tool({
        "fetch",
        "Scarica il contenuto di un URL. "
        "Utile per leggere documentazione, API, pagine web.",
        {
            {"url", "string", "L'URL da scaricare", true},
            {"format", "string", "Formato: \"text\" o \"markdown\" (default: text)", false},
            {"timeout", "number", "Timeout in secondi (default: 30)", false}
        },
        [](const std::map<std::string, std::string> & args) -> ToolResult {
            const auto it_url = args.find("url");
            if (it_url == args.end()) {
                return {false, "", "Parametro 'url' mancante"};
            }

            std::string url = it_url->second;
            int timeout = 30;
            auto it_timeout = args.find("timeout");
            if (it_timeout != args.end()) {
                try { timeout = std::stoi(it_timeout->second); }
                catch (...) {}
            }

            std::string cmd = "curl -sL --max-time " + std::to_string(timeout) + " '" + url + "' 2>/dev/null";
            FILE * pipe = popen(cmd.c_str(), "r");
            if (!pipe) {
                return {false, "", "Impossibile eseguire curl per: " + url};
            }

            std::string output;
            char buffer[4096];
            while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
                output += buffer;
            }
            int exit_code = pclose(pipe);

            if (exit_code != 0 && output.empty()) {
                return {false, "", "Errore nel download di " + url +
                        " (exit code: " + std::to_string(exit_code) + ")"};
            }

            const size_t MAX_OUTPUT = 65536;
            if (output.size() > MAX_OUTPUT) {
                output.resize(MAX_OUTPUT);
                output += "\n... [output troncato]";
            }

            return {true, output, ""};
        }
    });
}

// ===========================================================================
// register_tool — Aggiunge un tool al registro
// ===========================================================================

void ToolRegistry::register_tool(const ToolDefinition & tool)
{
    tool_index_[tool.name] = tools_.size();
    tools_.push_back(tool);
}

// ===========================================================================
// find — Cerca un tool per nome
// ===========================================================================

const ToolDefinition * ToolRegistry::find(const std::string & name) const
{
    auto it = tool_index_.find(name);
    if (it == tool_index_.end())
        return nullptr;
    return &tools_[it->second];
}

// ===========================================================================
// execute — Esegue un tool per nome
// ===========================================================================

ToolResult ToolRegistry::execute(
    const std::string & name,
    const std::map<std::string, std::string> & args)
{
    const ToolDefinition * tool = find(name);
    if (!tool) {
        return {false, "", "Tool sconosciuto: " + name};
    }

    // Validazione parametri richiesti
    for (const auto & param : tool->parameters) {
        if (param.required && args.find(param.name) == args.end()) {
            return {false, "",
                    "Parametro richiesto mancante: " + param.name +
                    " per il tool " + name};
        }
    }

    return tool->executor(args);
}

// ===========================================================================
// list_tool_names — Restituisce i nomi dei tool come stringa
// ===========================================================================

std::string ToolRegistry::list_tool_names() const
{
    std::string result;
    for (size_t i = 0; i < tools_.size(); i++) {
        if (i > 0) result += ", ";
        result += tools_[i].name;
    }
    return result;
}

std::string ToolRegistry::to_json_schema() const
{
    // Genera una descrizione JSON dei tool per il system prompt
    // Formato compatibile con OpenAI function calling
    std::string schema = "{\n  \"tools\": [\n";

    for (size_t i = 0; i < tools_.size(); i++) {
        const auto & tool = tools_[i];
        schema += "    {\n";
        schema += "      \"name\": \"" + tool.name + "\",\n";
        schema += "      \"description\": \"" + tool.description + "\",\n";
        schema += "      \"parameters\": {\n";
        schema += "        \"type\": \"object\",\n";
        schema += "        \"properties\": {\n";

        for (size_t j = 0; j < tool.parameters.size(); j++) {
            const auto & param = tool.parameters[j];
            schema += "          \"" + param.name + "\": {\n";
            schema += "            \"type\": \"" + param.type + "\",\n";
            schema += "            \"description\": \"" + param.description + "\"\n";
            schema += "          }";
            if (j < tool.parameters.size() - 1) {
                schema += ",";
            }
            schema += "\n";
        }

        schema += "        },\n";
        schema += "        \"required\": [";
        bool first = true;
        for (const auto & param : tool.parameters) {
            if (param.required) {
                if (!first) schema += ", ";
                schema += "\"" + param.name + "\"";
                first = false;
            }
        }
        schema += "]\n";
        schema += "      }\n";
        schema += "    }";
        if (i < tools_.size() - 1) {
            schema += ",";
        }
        schema += "\n";
    }

    schema += "  ]\n}";
    return schema;
}

/**
 * Converte le sequenze di escape JSON in caratteri reali.
 * Supporta: \n, \t, \r, \\, \", \/
 */
static std::string unescape_json(const std::string & s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            switch (s[i + 1]) {
                case 'n':  out += '\n'; i++; break;
                case 't':  out += '\t'; i++; break;
                case 'r':  out += '\r'; i++; break;
                case '\\': out += '\\'; i++; break;
                case '"':  out += '"';  i++; break;
                case '/':  out += '/';  i++; break;
                default:   out += s[i]; break;
            }
        } else {
            out += s[i];
        }
    }
    return out;
}

/**
 * Estrae il valore di una chiave JSON da una stringa strutturata.
 * Gestisce valori con virgolette escaped (\\\") e multi-riga.
 * Cerchiamo "key": "value" oppure "key": value (senza virgolette).
 */
static std::string extract_json_value(const std::string & str, size_t key_pos)
{
    if (key_pos == std::string::npos) return "";

    // Cerca i due punti dopo la chiave
    size_t colon = str.find(':', key_pos);
    if (colon == std::string::npos) return "";

    // Salta spazi
    size_t val_start = colon + 1;
    while (val_start < str.size() && (str[val_start] == ' ' || str[val_start] == '\t' || str[val_start] == '\n' || str[val_start] == '\r'))
        val_start++;

    if (val_start >= str.size()) return "";

    // Valore con virgolette
    if (str[val_start] == '"') {
        std::string result;
        bool escaped = false;
        for (size_t i = val_start + 1; i < str.size(); i++) {
            if (escaped) {
                result += str[i];
                escaped = false;
            } else if (str[i] == '\\') {
                result += str[i];
                escaped = true;
            } else if (str[i] == '"') {
                return unescape_json(result);
            } else {
                result += str[i];
            }
        }
        return unescape_json(result); // fallback
    }

    // Valore senza virgolette (numeri, booleani, null)
    size_t end = val_start;
    while (end < str.size() && str[end] != ',' && str[end] != '}' && str[end] != ' ' && str[end] != '\t' && str[end] != '\n' && str[end] != '\r')
        end++;
    return str.substr(val_start, end - val_start);
}

bool ToolRegistry::parse_tool_call(
    const std::string & response,
    std::string & out_name,
    std::map<std::string, std::string> & out_args)
{
    std::string json_str = extract_json_block(response);
    if (json_str.empty()) return false;

    // --- Estrae il nome del tool ---
    // Supporta: "tool", "function", "tool_call"
    std::regex name_regex("\"(?:tool|function|tool_call)\"\\s*:\\s*\"([^\"]+)\"");
    std::smatch name_match;
    if (!std::regex_search(json_str, name_match, name_regex)) return false;
    out_name = name_match[1].str();

    // --- Estrae i parametri ---
    // Cerca: "args", "parameters", "params", "input", "arguments"
    size_t args_start = std::string::npos;
    for (const char * key : {"\"args\"", "\"parameters\"", "\"params\"", "\"input\"", "\"arguments\""}) {
        args_start = json_str.find(key);
        if (args_start != std::string::npos) break;
    }

    if (args_start == std::string::npos) {
        // Prova senza chiave args: cerca coppie chiave-valore direttamente nel JSON
        args_start = 0;
    }

    // Trova l'oggetto args: {...}
    size_t brace_start = json_str.find('{', args_start);
    if (brace_start == std::string::npos) return !out_name.empty();

    int depth = 0;
    size_t brace_end = std::string::npos;
    for (size_t i = brace_start; i < json_str.size(); i++) {
        if (json_str[i] == '{') depth++;
        else if (json_str[i] == '}') {
            depth--;
            if (depth == 0) { brace_end = i; break; }
        }
    }
    if (brace_end == std::string::npos) return !out_name.empty();

    // Estrai coppie chiave-valore dall'oggetto args
    std::string args_obj = json_str.substr(brace_start + 1, brace_end - brace_start - 1);

    // Trova tutte le chiavi con valori: "key": "value" o "key": value
    {
        std::regex kv_regex("\"([^\"]+)\"\\s*:");
        std::sregex_iterator it(args_obj.begin(), args_obj.end(), kv_regex);
        for (; it != std::sregex_iterator(); ++it) {
            std::string key = (*it)[1].str();
            if (key == "tool" || key == "function" || key == "tool_call") continue;
            std::string value = extract_json_value(args_obj, it->position());
            if (!value.empty()) {
                out_args[key] = value;
            }
        }
    }

    return !out_name.empty();
}

std::string ToolRegistry::extract_json_block(const std::string & text) const
{
    // Cerca un blocco JSON nel testo

    // Prima prova: ```json ... ```
    std::regex json_block_regex("```json\\s*([\\s\\S]*?)```");
    std::smatch match;
    if (std::regex_search(text, match, json_block_regex)) {
        return match[1].str();
    }

    // Seconda prova: {...} (oggetto JSON diretto)
    size_t brace_start = text.find('{');
    if (brace_start != std::string::npos) {
        // Trova la chiusura corrispondente
        int depth = 0;
        for (size_t i = brace_start; i < text.size(); i++) {
            if (text[i] == '{') depth++;
            else if (text[i] == '}') {
                depth--;
                if (depth == 0) {
                    return text.substr(brace_start, i - brace_start + 1);
                }
            }
        }
    }

    return "";
}
