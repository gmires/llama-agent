#include "tools.h"
#include "patches.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <regex>
#include <filesystem>
#include <fnmatch.h>

namespace fs = std::filesystem;

/**
 * Espande pattern con parentesi graffe: *.{h,cpp} -> [*.h, *.cpp]
 * Supporta un solo livello di {} per semplicità.
 * Es: file.{html,css,js} -> [file.html, file.css, file.js]
 */
static std::vector<std::string> expand_braces(const std::string & pattern) {
    std::vector<std::string> result;
    size_t open = pattern.find('{');
    size_t close = pattern.find('}', open);
    if (open == std::string::npos || close == std::string::npos) {
        result.push_back(pattern);
        return result;
    }
    std::string prefix = pattern.substr(0, open);
    std::string suffix = pattern.substr(close + 1);
    std::string middle = pattern.substr(open + 1, close - open - 1);
    std::istringstream ss(middle);
    std::string part;
    while (std::getline(ss, part, ',')) {
        result.push_back(prefix + part + suffix);
    }
    return result;
}

/**
 * Converte un pattern con parentesi in regex base per find -path.
 * Es: *.h -> *.h,  *.{h,cpp} -> "*.h -o -name *.cpp"
 */
static std::string pattern_to_find_args(const std::string & pattern) {
    auto parts = expand_braces(pattern);
    if (parts.size() == 1) return parts[0];
    std::string out;
    for (size_t i = 0; i < parts.size(); i++) {
        if (i > 0) out += " -o -name ";
        out += "\"" + parts[i] + "\"";
    }
    return out;
}

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

            return {true, content, "", false, {{"file_size", std::to_string(content.size())}}};
        }
    });

    // --- Tool: write ---
    register_tool({
        "write",
        "Scrive/CREA un NUOVO file. Crea automaticamente le directory padre.\n"
        "IMPORTANTE: PER MODIFICARE file esistenti, USA 'diff_apply', NON 'write'.\n"
        "Le virgolette nel contenuto vanno escapate con \\\".\n"
        "ATTENZIONE: sovrascrive completamente il file esistente.",
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

            // Crea directory padre se necessario (es. dir/file.py)
            std::string path_str = it_path->second;
            fs::path parent = fs::path(path_str).parent_path();
            if (!parent.empty() && !fs::exists(parent)) {
                std::error_code ec;
                fs::create_directories(parent, ec);
                if (ec) {
                    return {false, "", "Impossibile creare directory: " + parent.string()};
                }
            }

            std::ofstream file(path_str);
            if (!file.is_open()) {
                return {false, "", "Impossibile scrivere il file: " + path_str};
            }

            file << it_content->second;
            file.close();

            return {true, "File scritto con successo: " + path_str + " (" +
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
        "Supporta brace expansion: *.{html,css,js} -> *.html, *.css, *.js. "
        "Supporta pattern annidati come `**/*.cpp`, `src/**`, `*.txt`, ecc.",
        {
            {"pattern", "string", "Pattern glob per trovare i file (supporta {a,b})", true}
        },
        [](const std::map<std::string, std::string> & args) -> ToolResult {
            const auto it = args.find("pattern");
            if (it == args.end()) {
                return {false, "", "Parametro 'pattern' mancante"};
            }

            std::string pattern = it->second;
            auto parts = expand_braces(pattern);

            std::string output;
            for (const auto & pat : parts) {
                // Usa find -path per pattern ricorsivi
                std::string cmd = "find . -path '*" + pat + "*' 2>&1 | head -100";
                FILE * pipe = popen(cmd.c_str(), "r");
                if (!pipe) continue;

                char buffer[4096];
                while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
                    output += buffer;
                    if (output.size() > 16384) break;
                }
                pclose(pipe);
            }

            if (output.empty()) {
                output = "[nessun file trovato per: " + pattern + "]";
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

    // Esplodi pattern con parentesi: *.{h,cpp} -> [*.h, *.cpp]
    auto patterns = expand_braces(pattern);
    bool found = false;
    for (const auto & pat : patterns) {
        if (fnmatch(pat.c_str(), name.c_str(), 0) == 0) {
            found = true;
            break;
        }
    }
    if (!found) continue;

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

    // --- Tool: ls ---
    register_tool({
        "ls",
        "Elenca file e directory in una cartella. Per ogni voce mostra: "
        "tipo (d/-), dimensione, nome. Utile per esplorare il filesystem.",
        {
            {"path", "string", "Directory da elencare (default: .)", false}
        },
        [](const std::map<std::string, std::string> & args) -> ToolResult {
            std::string path = ".";
            auto it = args.find("path");
            if (it != args.end()) path = it->second;
            if (!fs::exists(path) || !fs::is_directory(path))
                return {false, "", "Directory non trovata: " + path};
            std::string output;
            int count = 0;
            try {
                for (const auto & e : fs::directory_iterator(path,
                         fs::directory_options::skip_permission_denied)) {
                    std::string n = e.path().filename().string();
                    std::string t = e.is_directory() ? "d" : "-";
                    std::string s = "-";
                    if (e.is_regular_file()) {
                        auto sz = e.file_size();
                        if (sz>1024*1024) s=std::to_string(sz/(1024*1024))+"M";
                        else if (sz>1024) s=std::to_string(sz/1024)+"K";
                        else s=std::to_string(sz)+"B";
                    }
                    char buf[256];
                    snprintf(buf,sizeof(buf),"%s %6s  %s", t.c_str(), s.c_str(), n.c_str());
                    output += buf; output += "\n";
                    if (++count>=200){output+="... [truncated]\n";break;}
                    if (output.size()>8192){output+="... [truncated]\n";break;}
                }
            } catch(const std::exception& e){
                return {false,"","Errore: "+std::string(e.what())};
            }
            if (output.empty()) output="[directory vuota]";
            return {true, output, ""};
        }
    });

    // --- Tool: rm ---
    register_tool({
        "rm",
        "Elimina un file. ATTENZIONE: operazione irreversibile. "
        "Non elimina directory non vuote.",
        {
            {"path", "string", "File da eliminare", true}
        },
        [](const std::map<std::string, std::string> & args) -> ToolResult {
            auto it=args.find("path");
            if(it==args.end()) return {false,"","Parametro 'path' mancante"};
            if(!fs::exists(it->second))
                return {false,"","File non trovato: "+it->second};
            if(fs::is_directory(it->second)&&!fs::is_empty(it->second))
                return {false,"","Directory non vuota: "+it->second};
            std::error_code ec;
            fs::remove(it->second,ec);
            if(ec) return {false,"","Errore: "+ec.message()};
            return {true,"Eliminato: "+it->second,""};
        }
    });

    // --- Tool: mv ---
    register_tool({
        "mv",
        "Sposta o rinomina un file/directory.",
        {
            {"from", "string", "Percorso sorgente", true},
            {"to",   "string", "Percorso destinazione", true}
        },
        [](const std::map<std::string, std::string> & args) -> ToolResult {
            auto it_from=args.find("from"), it_to=args.find("to");
            if(it_from==args.end()) return {false,"","'from' mancante"};
            if(it_to==args.end())   return {false,"","'to' mancante"};
            if(!fs::exists(it_from->second))
                return {false,"","File non trovato: "+it_from->second};
            std::error_code ec;
            fs::rename(it_from->second,it_to->second,ec);
            if(ec) return {false,"","Spostamento fallito: "+ec.message()};
            return {true,it_from->second+" -> "+it_to->second,""};
        }
    });

    // --- Tool: edit ---
    register_tool({
        "edit",
        "Sostituisce una stringa in un file (match unico). Alternativa semplice a diff_apply.\n"
        "Per modifiche complesse o multi-riga, preferisci 'diff_apply'.\n"
        "La vecchia stringa deve apparire ESATTAMENTE UNA VOLTA nel file.",
        {
            {"path",       "string", "File da modificare", true},
            {"old_string", "string", "Testo da sostituire (deve essere unico)", true},
            {"new_string", "string", "Nuovo testo da inserire", true}
        },
        [](const std::map<std::string, std::string> & args) -> ToolResult {
            auto it_p=args.find("path"),it_o=args.find("old_string"),it_n=args.find("new_string");
            if(it_p==args.end()) return {false,"","'path' mancante"};
            if(it_o==args.end()) return {false,"","'old_string' mancante"};
            if(it_n==args.end()) return {false,"","'new_string' mancante"};
            std::ifstream in(it_p->second);
            if(!in.is_open()) return {false,"","Impossibile leggere: "+it_p->second};
            std::stringstream buf; buf<<in.rdbuf(); in.close();
            std::string c=buf.str(), o=it_o->second, n=it_n->second;
            size_t p=c.find(o);
            if(p==std::string::npos)
                return {false,"","Stringa non trovata nel file: "+o.substr(0,60)};
            if(c.find(o,p+1)!=std::string::npos)
                return {false,"","Stringa trovata piu' di una volta."};
            c.replace(p,o.size(),n);
            std::ofstream out(it_p->second);
            if(!out.is_open()) return {false,"","Impossibile scrivere: "+it_p->second};
            out<<c; out.close();
            return {true,"Modificato: "+it_p->second+" ("+
                     std::to_string(o.size())+"->"+std::to_string(n.size())+" byte)",""};
        }
    });

    // --- Tool: diff_apply ---
    register_tool({
        "diff_apply",
        "STRUMENTO PRINCIPALE DI EDITING. Applica una unified diff a un file.\n"
        "USA QUESTO per modificare file esistenti, NON 'write'.\n"
        "Il formato richiesto:\n"
        "```diff\n"
        "@@ -L,C +L,C @@\n"
        " riga di contesto (deve matchare ESATTAMENTE nel file)\n"
        "-riga da rimuovere\n"
        "+riga da aggiungere\n"
        " riga di contesto\n"
        "```\n"
        "REGOLE:\n"
        "- Includi 2-3 righe di contesto prima e dopo la modifica\n"
        "- Le righe di contesto devono essere COPIATE ESATTAMENTE dal file\n"
        "- Supporta hunk multipli per modifiche in punti diversi del file\n"
        "- Il diff fallisce se il contesto non matcha — questo PREVIENE modifiche errate",
        {
            {"path", "string", "Percorso del file da modificare", true},
            {"diff", "string", "Unified diff da applicare", true}
        },
        [](const std::map<std::string, std::string> & args) -> ToolResult {
            const auto it_path = args.find("path");
            const auto it_diff = args.find("diff");
            if (it_path == args.end()) return {false, "", "Parametro 'path' mancante"};
            if (it_diff == args.end()) return {false, "", "Parametro 'diff' mancante"};

            // Leggi file
            std::ifstream in(it_path->second);
            if (!in.is_open())
                return {false, "", "Impossibile leggere: " + it_path->second};
            std::string content((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
            in.close();

            // Applica patch
            PatchResult pr = apply_unified_diff(content, it_diff->second);
            if (!pr.ok)
                return {false, "", pr.error};

            // Crea directory padre
            std::string path_str = it_path->second;
            fs::path parent = fs::path(path_str).parent_path();
            if (!parent.empty() && !fs::exists(parent)) {
                std::error_code ec;
                fs::create_directories(parent, ec);
                if (ec)
                    return {false, "", "Impossibile creare directory: " + parent.string()};
            }

            // Scrivi
            std::ofstream out(path_str);
            if (!out.is_open())
                return {false, "", "Impossibile scrivere: " + path_str};
            out << pr.modified;
            out.close();

            return {true, "Patch applicata a " + path_str + " (" +
                    std::to_string(content.size()) + " -> " +
                    std::to_string(pr.modified.size()) + " byte)", ""};
        }
    });

    // --- Tool: tree ---
    register_tool({
        "tree",
        "Mostra la struttura ad albero di una directory. "
        "Utile per esplorare la struttura del progetto.",
        {
            {"path", "string", "Directory radice (default: .)", false},
            {"max_depth", "number", "Profondità massima (default: 3)", false}
        },
        [](const std::map<std::string, std::string> & args) -> ToolResult {
            std::string root = ".";
            int max_depth = 3;
            auto it = args.find("path");
            if (it != args.end()) root = it->second;
            auto it_d = args.find("max_depth");
            if (it_d != args.end()) try { max_depth = std::stoi(it_d->second); } catch(...) {}

            if (!fs::exists(root) || !fs::is_directory(root))
                return {false, "", "Directory non trovata: " + root};

            std::string output = root + "\n";
            int count = 0;

            std::function<void(const std::string &, const std::string &, int)> walk;
            walk = [&](const std::string & dir, const std::string & prefix, int depth) {
                if (depth > max_depth || output.size() > 8192) return;
                try {
                    std::vector<fs::directory_entry> entries;
                    for (const auto & e : fs::directory_iterator(dir,
                             fs::directory_options::skip_permission_denied))
                        entries.push_back(e);
                    std::sort(entries.begin(), entries.end(), [](const auto & a, const auto & b) {
                        if (a.is_directory() != b.is_directory()) return a.is_directory();
                        return a.path().filename() < b.path().filename();
                    });
                    for (size_t i = 0; i < entries.size(); i++) {
                        bool last = (i == entries.size() - 1);
                        std::string name = entries[i].path().filename().string();
                        std::string branch = last ? "\u2514\u2500\u2500 " : "\u251C\u2500\u2500 ";
                        output += prefix + branch + name;
                        if (entries[i].is_directory()) {
                            output += "/\n";
                            std::string new_prefix = prefix + (last ? "    " : "\u2502   ");
                            walk(entries[i].path().string(), new_prefix, depth + 1);
                        } else {
                            output += "\n";
                        }
                        count++;
                        if (count > 200 || output.size() > 8192) {
                            output += prefix + "...\n";
                            return;
                        }
                    }
                } catch (...) {}
            };
            walk(root, "", 0);
            if (output.size() > 8192) output = output.substr(0, 8192) + "\n...";
            return {true, output, ""};
        }
    });

    // --- Tool: web_search ---
    register_tool({
        "web_search",
        "Cerca su DuckDuckGo e restituisce titoli, URL e snippet dei risultati. "
        "Utile per trovare informazioni aggiornate, documentazione, esempi di codice.",
        {
            {"query",    "string", "Query di ricerca", true},
            {"num",      "number", "Numero risultati (default: 5, max: 10)", false}
        },
        [](const std::map<std::string, std::string> & args) -> ToolResult {
            auto it_q = args.find("query");
            if(it_q==args.end()) return {false,"","Parametro 'query' mancante"};
            int num = 5;
            auto it_n = args.find("num");
            if(it_n!=args.end()){try{num=std::stoi(it_n->second);}catch(...){}}
            if(num<1) num=1; if(num>10) num=10;

            // URL-encode semplice per la query
            std::string q = it_q->second;
            std::string encoded;
            for (char c : q) {
                if (c == ' ') encoded += '+';
                else if (c == '&') encoded += "%26";
                else if (c == '=') encoded += "%3D";
                else if (c == '?') encoded += "%3F";
                else encoded += c;
            }

            // DDG richiede POST ora (non più GET)
            std::string cmd = "curl -sL --max-time 15 -X POST "
                "-H \"User-Agent: Mozilla/5.0 (X11; Linux x86_64)\" "
                "-d \"q=" + encoded + "\" "
                "\"https://html.duckduckgo.com/html/\" 2>/dev/null";
            FILE* pipe = popen(cmd.c_str(),"r");
            if(!pipe) return {false,"","Impossibile eseguire curl"};

            std::string html;
            char buf[8192];
            while(fgets(buf,sizeof(buf),pipe)) html+=buf;
            pclose(pipe);

            if(html.empty()) return {false,"","Nessuna risposta da DuckDuckGo"};

            // Estrai risultati: <a rel="nofollow" class="result__a" href="URL">TITLE</a>
            // e <a class="result__snippet">SNIPPET</a>
            std::string output;
            int found=0;
            size_t pos=0;
            while(found<num && pos<html.size()){
                size_t link = html.find("class=\"result__a\"",pos);
                if(link==std::string::npos) break;
                size_t href_s = html.find("href=\"",link);
                if(href_s==std::string::npos||href_s>link+200){pos=link+1;continue;}
                href_s+=6;
                size_t href_e = html.find("\"",href_s);
                if(href_e==std::string::npos){pos=link+1;continue;}
                std::string url = html.substr(href_s,href_e-href_s);

                size_t title_s = html.find(">",href_e);
                if(title_s==std::string::npos||title_s>href_e+500){pos=link+1;continue;}
                title_s++;
                size_t title_e = html.find("</a>",title_s);
                if(title_e==std::string::npos){pos=link+1;continue;}
                std::string title = html.substr(title_s,title_e-title_s);
                // rimuovi tag HTML dal titolo
                size_t tag;
                while((tag=title.find('<'))!=std::string::npos){
                    size_t tag_e=title.find('>',tag);
                    if(tag_e!=std::string::npos) title.erase(tag,tag_e-tag+1);
                    else break;
                }

                // Cerca snippet
                std::string snippet="";
                size_t snip = html.find("class=\"result__snippet\"",title_e);
                if(snip!=std::string::npos&&snip<title_e+2000){
                    size_t snip_s=html.find(">",snip);
                    if(snip_s!=std::string::npos){
                        snip_s++;
                        size_t snip_e=html.find("</a>",snip_s);
                        if(snip_e!=std::string::npos&&snip_e<snip_s+2000){
                            snippet=html.substr(snip_s,snip_e-snip_s);
                            while((tag=snippet.find('<'))!=std::string::npos){
                                size_t te=snippet.find('>',tag);
                                if(te!=std::string::npos) snippet.erase(tag,te-tag+1);
                                else break;
                            }
                        }
                    }
                }

                output += std::to_string(found+1)+". "+title+"\n";
                output += "   "+url+"\n";
                if(!snippet.empty()) output += "   "+snippet+"\n";
                output += "\n";
                found++;
                pos=title_e;
            }
            if(output.empty()) output="[nessun risultato trovato per: "+it_q->second+"]";
            return {true,output,""};
        }
    });

    // --- Tool: git_diff ---
    register_tool({
        "git_diff",
        "Mostra le modifiche non committate (working tree vs HEAD). "
        "Usa --stat per un riepilogo, senza argomenti per il diff completo.",
        {
            {"stat", "string", "Se \"true\", mostra solo il riepilogo (default: true)", false}
        },
        [](const std::map<std::string, std::string> & args) -> ToolResult {
            bool stat = true;
            auto it = args.find("stat");
            if (it != args.end()) stat = (it->second != "false");
            std::string cmd = stat ? "git diff --stat 2>&1" : "git diff 2>&1";
            FILE* p = popen(cmd.c_str(), "r");
            if (!p) return {false,"","Impossibile eseguire git diff"};
            std::string out; char buf[4096];
            while (fgets(buf,sizeof(buf),p)) { out+=buf; if(out.size()>16384) break; }
            pclose(p);
            if(out.empty()) out="[nessuna modifica non committata]";
            return {true,out,""};
        }
    });

    // --- Tool: git_log ---
    register_tool({
        "git_log",
        "Mostra gli ultimi commit. Usa il formato --oneline.",
        {
            {"n", "number", "Numero di commit da mostrare (default: 10)", false}
        },
        [](const std::map<std::string, std::string> & args) -> ToolResult {
            int n = 10;
            auto it = args.find("n");
            if (it != args.end()) try { n = std::stoi(it->second); } catch(...) {}
            std::string cmd = "git log --oneline -n "+std::to_string(n)+" 2>&1";
            FILE* p = popen(cmd.c_str(),"r");
            if(!p) return {false,"","Impossibile eseguire git log"};
            std::string out; char buf[4096];
            while(fgets(buf,sizeof(buf),p)) out+=buf;
            pclose(p);
            if(out.empty()) out="[nessun commit]";
            return {true,out,""};
        }
    });

    // --- Tool: git_status ---
    register_tool({
        "git_status",
        "Mostra lo stato del working tree (file modificati, nuovi, staged).",
        {},
        [](const std::map<std::string, std::string> &) -> ToolResult {
            FILE* p = popen("git status --short 2>&1","r");
            if(!p) return {false,"","Impossibile eseguire git status"};
            std::string out; char buf[4096];
            while(fgets(buf,sizeof(buf),p)) out+=buf;
            pclose(p);
            if(out.empty()) out="[working tree pulito]";
            return {true,out,""};
        }
    });

    // --- Tool: git_branch ---
    register_tool({
        "git_branch",
        "Mostra i branch locali. Il branch corrente è marcato con *.",
        {},
        [](const std::map<std::string, std::string> &) -> ToolResult {
            FILE* p = popen("git branch 2>&1","r");
            if(!p) return {false,"","Impossibile eseguire git branch"};
            std::string out; char buf[4096];
            while(fgets(buf,sizeof(buf),p)) out+=buf;
            pclose(p);
            if(out.empty()) out="[nessun branch]";
            return {true,out,""};
        }
    });

    // --- Tool: task_create ---
    register_tool({
        "task_create",
        "Crea un nuovo task nella lista. I task vengono salvati in .cache/tasks.json.",
        {
            {"title", "string", "Titolo del task", true},
            {"description", "string", "Descrizione opzionale", false}
        },
        [](const std::map<std::string, std::string> & args) -> ToolResult {
            auto it = args.find("title");
            if (it == args.end()) return {false,"","Parametro 'title' mancante"};
            std::string desc = "";
            auto it_d = args.find("description");
            if (it_d != args.end()) desc = it_d->second;

            // Carica task esistenti
            std::string path = ".cache/tasks.json";
            fs::create_directories(".cache");
            std::vector<std::map<std::string,std::string>> tasks;
            std::ifstream in(path);
            if (in.is_open()) {
                std::string json((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
                in.close();
                // Parsing semplice: [{"id":"1","title":"...","status":"..."}]
                size_t pos = 0;
                while ((pos = json.find("{\"id\"", pos)) != std::string::npos) {
                    std::map<std::string,std::string> t;
                    size_t end = json.find("}", pos);
                    if (end == std::string::npos) break;
                    std::string obj = json.substr(pos, end-pos+1);
                    for (const char* key : {"id","title","status","description"}) {
                        size_t kp = obj.find(std::string("\"")+key+"\"");
                        if (kp == std::string::npos) continue;
                        size_t vs = obj.find("\"", kp+strlen(key)+3);
                        if (vs == std::string::npos) continue;
                        size_t ve = obj.find("\"", vs+1);
                        if (ve == std::string::npos) continue;
                        t[key] = obj.substr(vs+1, ve-vs-1);
                    }
                    if (!t.empty()) tasks.push_back(t);
                    pos = end + 1;
                }
            }

            int new_id = 1;
            for (const auto& t : tasks) {
                try { int tid = std::stoi(t.at("id")); if (tid >= new_id) new_id = tid+1; }
                catch(...) {}
            }

            std::map<std::string,std::string> task;
            task["id"] = std::to_string(new_id);
            task["title"] = it->second;
            task["status"] = "todo";
            task["description"] = desc;
            tasks.push_back(task);

            // Salva
            std::ofstream out(path);
            if (!out.is_open()) return {false,"","Impossibile salvare tasks.json"};
            out << "[\n";
            for (size_t i=0;i<tasks.size();i++) {
                out << "  {\"id\":\""<<tasks[i]["id"]<<"\","
                    << "\"title\":\""<<tasks[i]["title"]<<"\","
                    << "\"status\":\""<<tasks[i]["status"]<<"\","
                    << "\"description\":\""<<tasks[i]["description"]<<"\"}";
                if (i<tasks.size()-1) out << ",";
                out << "\n";
            }
            out << "]\n";
            out.close();

            return {true,"Task #"+std::to_string(new_id)+" creato: "+it->second,""};
        }
    });

    // --- Tool: task_list ---
    register_tool({
        "task_list",
        "Mostra la lista dei task con ID, stato e titolo.",
        {},
        [](const std::map<std::string, std::string> &) -> ToolResult {
            std::string path = ".cache/tasks.json";
            fs::create_directories(".cache");
            std::ifstream in(path);
            if (!in.is_open()) return {true,"[nessun task]",""};
            std::string json((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
            in.close();

            std::string out;
            size_t pos = 0;
            while ((pos = json.find("{\"id\"", pos)) != std::string::npos) {
                size_t end = json.find("}", pos);
                if (end == std::string::npos) break;
                std::string obj = json.substr(pos, end-pos+1);
                std::string id, title, status;
                for (const char* key : {"id","title","status"}) {
                    size_t kp = obj.find(std::string("\"")+key+"\"");
                    if (kp == std::string::npos) continue;
                    size_t vs = obj.find("\"", kp+strlen(key)+3);
                    if (vs == std::string::npos) continue;
                    size_t ve = obj.find("\"", vs+1);
                    if (ve == std::string::npos) continue;
                    std::string val = obj.substr(vs+1, ve-vs-1);
                    if (key==std::string("id")) id=val;
                    else if (key==std::string("title")) title=val;
                    else if (key==std::string("status")) status=val;
                }
                if (!id.empty()) {
                    std::string icon = (status=="done")?"[x]":(status=="in_progress")?"[~]":"[ ]";
                    out += icon+" #"+id+" "+title+"\n";
                }
                pos = end + 1;
            }
            if (out.empty()) out="[nessun task]";
            return {true,out,""};
        }
    });

    // --- Tool: task_update ---
    register_tool({
        "task_update",
        "Aggiorna lo stato di un task. Stati: todo, in_progress, done.",
        {
            {"id",     "string", "ID del task da aggiornare", true},
            {"status", "string", "Nuovo stato: todo, in_progress, done", true}
        },
        [](const std::map<std::string, std::string> & args) -> ToolResult {
            auto it_id = args.find("id");
            auto it_st = args.find("status");
            if (it_id==args.end()) return {false,"","'id' mancante"};
            if (it_st==args.end()) return {false,"","'status' mancante"};
            std::string target_id = it_id->second;
            // Il modello potrebbe passare "#1" invece di "1" — rimuovi #
            if (!target_id.empty() && target_id[0] == '#') target_id = target_id.substr(1);
            std::string new_status = it_st->second;
            if (new_status!="todo"&&new_status!="in_progress"&&new_status!="done")
                return {false,"","Stato non valido: "+new_status};

            std::string path = ".cache/tasks.json";
            fs::create_directories(".cache");
            std::ifstream in(path);
            if (!in.is_open()) return {false,"","Nessun task file trovato"};
            std::string json((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
            in.close();

            bool found=false;
            size_t pos=0;
            while ((pos=json.find("{\"id\"",pos))!=std::string::npos) {
                size_t end=json.find("}",pos);
                if(end==std::string::npos) break;
                std::string obj=json.substr(pos,end-pos+1);
                size_t ip=obj.find("\"id\"");
                if(ip==std::string::npos){pos=end+1;continue;}
                size_t vs=obj.find("\"",ip+5);
                if(vs==std::string::npos){pos=end+1;continue;}
                size_t ve=obj.find("\"",vs+1);
                if(ve==std::string::npos){pos=end+1;continue;}
                std::string tid=obj.substr(vs+1,ve-vs-1);
                if(tid==target_id){
                    size_t sp=obj.find("\"status\"");
                    if(sp==std::string::npos){pos=end+1;continue;}
                    size_t svs=obj.find("\"",sp+9);
                    if(svs==std::string::npos){pos=end+1;continue;}
                    size_t sve=obj.find("\"",svs+1);
                    if(sve==std::string::npos){pos=end+1;continue;}
                    std::string old_status=obj.substr(svs+1,sve-svs-1);
                    json.replace(pos+svs+1,sve-svs-1,new_status);
                    found=true;
                    break;
                }
                pos=end+1;
            }
            if(!found) return {false,"","Task #"+target_id+" non trovato"};

            std::ofstream out(path);
            if(!out.is_open()) return {false,"","Impossibile salvare"};
            out<<json;
            out.close();
            return {true,"Task #"+target_id+" -> "+new_status,""};
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
        return {false, "", "Tool sconosciuto: " + name, true};
    }

    // Validazione parametri richiesti
    for (const auto & param : tool->parameters) {
        if (param.required && args.find(param.name) == args.end()) {
            return {false, "",
                    "Parametro richiesto mancante: " + param.name +
                    " per il tool " + name, true};
        }
    }

    // before hook
    if (before_hook_) {
        auto block = before_hook_(name, args);
        if (block.is_error) return block;
    }

    ToolResult result = tool->executor(args);

    // after hook
    if (after_hook_) {
        result = after_hook_(name, args, result);
    }

    return result;
}

void ToolRegistry::set_before_hook(
    std::function<ToolResult(const std::string &, const std::map<std::string, std::string> &)> hook)
{
    before_hook_ = std::move(hook);
}

void ToolRegistry::set_after_hook(
    std::function<ToolResult(const std::string &, const std::map<std::string, std::string> &,
                              const ToolResult &)> hook)
{
    after_hook_ = std::move(hook);
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
    // Prima prova: ```json ... ```
    std::regex json_block_regex("```json\\s*([\\s\\S]*?)```");
    std::smatch match;
    if (std::regex_search(text, match, json_block_regex)) {
        return match[1].str();
    }

    // Seconda prova: {...} oggetto JSON diretto.
    // Cerca {"tool" o {"function" — non il primo '{' generico,
    // che potrebbe essere un esempio di codice (es. { x = 10 }).
    size_t scan_pos = 0;
    while (scan_pos < text.size()) {
        size_t brace_start = std::string::npos;

        // Cerca {"tool" o {"function" da scan_pos
        size_t p1 = text.find("{\"tool\"", scan_pos);
        size_t p2 = text.find("{\"function\"", scan_pos);
        size_t p3 = text.find("{\"tool_call\"", scan_pos);

        if (p1 != std::string::npos) brace_start = p1;
        if (p2 != std::string::npos && (brace_start == std::string::npos || p2 < brace_start))
            brace_start = p2;
        if (p3 != std::string::npos && (brace_start == std::string::npos || p3 < brace_start))
            brace_start = p3;

        if (brace_start == std::string::npos) break; // nessun tool call JSON trovato

        // Trova il matching '}' partendo da brace_start
        // skip_json_string per non contare {} dentro stringhe
        int depth = 0;
        bool has_tool_key = false;
        bool valid = true;
        for (size_t i = brace_start; i < text.size();) {
            if (text[i] == '"') {
                size_t str_end = skip_json_string(text, i);
                // Verifica se questa stringa matcha "tool" o "function"
                if (!has_tool_key) {
                    std::string key = text.substr(i + 1, str_end - i - 2);
                    if (key == "tool" || key == "function" || key == "tool_call")
                        has_tool_key = true;
                }
                i = str_end;
            } else if (text[i] == '{') {
                depth++;
                i++;
            } else if (text[i] == '}') {
                depth--;
                if (depth == 0) {
                    if (has_tool_key) {
                        return text.substr(brace_start, i - brace_start + 1);
                    }
                    // Questo {...} non contiene "tool"/"function" — salta e continua
                    scan_pos = i + 1;
                    valid = false;
                    break;
                }
                i++;
            } else {
                i++;
            }
        }
        if (!valid) continue;
        if (depth != 0) break; // sbilanciato, impossibile completare
        break; // non dovrebbe succedere
    }

    return "";
}
