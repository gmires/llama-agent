#include "tools.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <regex>
#include <filesystem>
#include <fnmatch.h>

namespace fs = std::filesystem;

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
        "Esegue un comando shell e restituisce stdout+stderr. "
        "Output limitato a 64KB. Usa 'timeout' per specificare secondi (default 30). "
        "Utile per eseguire comandi, script, compilazioni, git, ls, find, ecc.",
        {
            {"command", "string", "Il comando shell da eseguire", true},
            {"timeout", "number", "Timeout in secondi (default: 30)", false}
        },
        [](const std::map<std::string, std::string> & args) -> ToolResult {
            const auto it = args.find("command");
            if (it == args.end()) {
                return {false, "", "Parametro 'command' mancante"};
            }

            int timeout_sec = 30;
            auto it_timeout = args.find("timeout");
            if (it_timeout != args.end()) {
                try { timeout_sec = std::stoi(it_timeout->second); }
                catch (...) {}
            }

            std::string cmd = it->second;
            cmd += " 2>&1";

            FILE * pipe = popen(cmd.c_str(), "r");
            if (!pipe) {
                return {false, "", "Impossibile eseguire il comando: " + it->second};
            }

            std::string output;
            char buffer[4096];
            auto start_time = std::chrono::steady_clock::now();

            while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
                output += buffer;

                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - start_time).count();
                if (elapsed > timeout_sec) {
                    pclose(pipe);
                    if (output.size() < 50000) output += "\n";
                    output += "[timeout dopo " + std::to_string(timeout_sec) + "s]";
                    const size_t MAX_OUTPUT = 65536;
                    if (output.size() > MAX_OUTPUT) {
                        output.resize(MAX_OUTPUT - 30);
                        output += "\n... [output troncato]";
                    }
                    return {true, output, ""};
                }

                const size_t MAX_SIZE = 65536;
                if (output.size() > MAX_SIZE) {
                    pclose(pipe);
                    output.resize(MAX_SIZE - 30);
                    output += "\n... [output troncato]";
                    return {true, output, ""};
                }
            }

            int exit_code = pclose(pipe);

            if (exit_code != 0) {
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

    // --- Tool: find ---
    register_tool({
        "find",
        "Cerca file e directory ricorsivamente. Restituisce percorsi relativi. "
        "Supporta: path iniziale, pattern nome (es. *.cpp, test*, *config*), "
        "tipo (file/directory/any), profondit massima. "
        "Grep per il contenuto, find per i nomi dei file.",
        {
            {"path", "string", "Directory da cui iniziare (default: .)", false},
            {"pattern", "string", "Pattern per il nome file (es. *.cpp, test*). Usa * per tutti.", true},
            {"type", "string", "Tipo: file, directory, any (default: any)", false},
            {"max_depth", "number", "Profondit massima (default: illimitata)", false}
        },
        [](const std::map<std::string, std::string> & args) -> ToolResult {
            std::string search_path = ".";
            auto it_path = args.find("path");
            if (it_path != args.end()) search_path = it_path->second;

            std::string pattern = "*";
            auto it_pat = args.find("pattern");
            if (it_pat != args.end()) pattern = it_pat->second;

            std::string type_filter = "any";
            auto it_type = args.find("type");
            if (it_type != args.end()) type_filter = it_type->second;

            int max_depth = -1;
            auto it_depth = args.find("max_depth");
            if (it_depth != args.end()) {
                try { max_depth = std::stoi(it_depth->second); }
                catch (...) {}
            }

            if (!fs::exists(search_path) || !fs::is_directory(search_path)) {
                return {false, "", "Directory non trovata: " + search_path};
            }

            std::string output;
            int count = 0;
            const int MAX_RESULTS = 500;
            const int MAX_OUTPUT = 32768;

            try {
                auto iter = fs::recursive_directory_iterator(
                    search_path,
                    fs::directory_options::skip_permission_denied);
                for (; iter != fs::recursive_directory_iterator(); ++iter) {
                    const auto & entry = *iter;
                    int depth = iter.depth();
                    if (max_depth >= 0 && depth > max_depth) {
                        if (depth >= max_depth) iter.disable_recursion_pending();
                        continue;
                    }

                    std::string name = entry.path().filename().string();

                    if (fnmatch(pattern.c_str(), name.c_str(), 0) != 0)
                        continue;

                    if (type_filter == "file" && !entry.is_regular_file()) continue;
                    if (type_filter == "directory" && !entry.is_directory()) continue;

                    std::string rel_path = fs::relative(entry.path(), search_path).string();
                    if (entry.is_directory()) rel_path += "/";

                    output += rel_path + "\n";
                    count++;

                    if (count >= MAX_RESULTS) {
                        output += "... [limite " + std::to_string(MAX_RESULTS) +
                                  " risultati raggiunto]";
                        break;
                    }

                    if (output.size() > MAX_OUTPUT) {
                        output.resize(MAX_OUTPUT - 40);
                        output += "\n... [output troncato]";
                        break;
                    }
                }
            } catch (const std::exception & e) {
                if (count == 0)
                    return {false, "", "Errore ricerca: " + std::string(e.what())};
            }

            if (output.empty()) {
                output = "[nessun file trovato per: " + pattern + "]";
            }

            return {true, output, ""};
        }
    });
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
 * Avanza l'indice i fino a trovare un carattere c NON dentro una stringa JSON.
 * Salta correttamente le stringhe JSON (con escape \"), non conta {} dentro le stringhe.
 */
static size_t skip_json_string(const std::string & str, size_t i)
{
    if (i >= str.size() || str[i] != '"') return i;
    i++; // salta la " di apertura
    while (i < str.size()) {
        if (str[i] == '\\' && i + 1 < str.size()) {
            i += 2; // salta carattere escaped
        } else if (str[i] == '"') {
            return i + 1; // fine stringa
        } else {
            i++;
        }
    }
    return i;
}

/**
 * Trova il carattere c a partire da pos, saltando le stringhe JSON.
 * Restituisce npos se non trovato.
 */
static size_t find_char_outside_string(const std::string & str, char c, size_t pos)
{
    while (pos < str.size()) {
        if (str[pos] == '"') {
            pos = skip_json_string(str, pos);
        } else if (str[pos] == c) {
            return pos;
        } else {
            pos++;
        }
    }
    return std::string::npos;
}

/**
 * Estrae il valore di una chiave JSON da una stringa strutturata.
 * Gestisce correttamente stringhe JSON nidificate (con \" interni).
 */
static std::string extract_json_value(const std::string & str, size_t key_pos)
{
    if (key_pos == std::string::npos) return "";

    // Cerca i due punti dopo la chiave
    size_t colon = find_char_outside_string(str, ':', key_pos);
    if (colon == std::string::npos) return "";

    // Salta spazi
    size_t val_start = colon + 1;
    while (val_start < str.size() && (str[val_start] == ' ' || str[val_start] == '\t' || str[val_start] == '\n' || str[val_start] == '\r'))
        val_start++;

    if (val_start >= str.size()) return "";

    // Valore con virgolette
    if (str[val_start] == '"') {
        // Estrai il contenuto della stringa JSON rispettando gli escape
        size_t content_start = val_start + 1;
        size_t i = content_start;
        std::string result;
        while (i < str.size()) {
            if (str[i] == '\\' && i + 1 < str.size()) {
                result += str[i];
                result += str[i + 1];
                i += 2;
            } else if (str[i] == '"') {
                return unescape_json(result);
            } else {
                result += str[i];
                i++;
            }
        }
        return unescape_json(result);
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

    // Scansiona args_obj linearmente, trovando "key": fuori dalle stringhe JSON.
    // Questo evita falsi match quando il contenuto (es. JS) contiene "key": patterns.
    {
        size_t i = 0;
        while (i < args_obj.size()) {
            // Salta le stringhe JSON
            if (args_obj[i] == '"') {
                size_t str_end = skip_json_string(args_obj, i);
                // Verifica se questa stringa è una chiave (seguita da ':')
                size_t after_str = str_end;
                while (after_str < args_obj.size() &&
                       (args_obj[after_str] == ' ' || args_obj[after_str] == '\t' ||
                        args_obj[after_str] == '\n' || args_obj[after_str] == '\r'))
                    after_str++;
                if (after_str < args_obj.size() && args_obj[after_str] == ':') {
                    // Estrai il nome della chiave (senza virgolette)
                    std::string key = unescape_json(args_obj.substr(i + 1, str_end - i - 2));
                    if (key != "tool" && key != "function" && key != "tool_call") {
                        std::string value = extract_json_value(args_obj, i);
                        if (!value.empty()) {
                            out_args[key] = value;
                        }
                    }
                    i = after_str + 1; // salta ':'
                } else {
                    i = str_end; // non era una chiave, continua
                }
            } else if (args_obj[i] == '{' || args_obj[i] == '}') {
                i++; // oggetti annidati, saltiamo
            } else {
                i++;
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
    // Usa skip_json_string per non contare {} dentro stringhe JSON
    size_t brace_start = text.find('{');
    if (brace_start != std::string::npos) {
        int depth = 0;
        for (size_t i = brace_start; i < text.size();) {
            if (text[i] == '"') {
                i = skip_json_string(text, i);
            } else if (text[i] == '{') {
                depth++;
                i++;
            } else if (text[i] == '}') {
                depth--;
                if (depth == 0) {
                    return text.substr(brace_start, i - brace_start + 1);
                }
                i++;
            } else {
                i++;
            }
        }
    }

    return "";
}
