/*
 * llama-agent — Agente AI conversazionale basato su llama.cpp con TUI.
 *
 * Utilizza llama.cpp come motore di inferenza locale, supporta tutti
 * i parametri CLI di llama-cli, e fornisce un'interfaccia FTXUI con
 * pannelli separati per THINKING e RESPONSE.
 *
 * La KVCache viene salvata su disco (.cache/) per preservare il contesto
 * tra sessioni.
 *
 * Utilizzo:
 *   llama-agent -m <modello.gguf> [opzioni llama-cli...]
 *   llama-agent -m <modello.gguf> --simple-ui  (modalitÃ  console base)
 *   llama-agent -m <modello.gguf> -p "Ciao!"   (singolo turno)
 *
 * Tutti i flag di llama-cli sono supportati:
 *   -m, --model         Percorso del modello GGUF
 *   -ngl, --n-gpu-layers  Layer da offloadare su GPU
 *   -c, --ctx-size      Dimensione del contesto
 *   -p, --prompt        Prompt iniziale
 *   --temp              Temperatura di campionamento
 *   --top-k, --top-p    Parametri di campionamento
 *   --seed              Seed per la generazione
 *   ... e tutti gli altri flag di common_params
 *
 * Flag specifici di llama-agent:
 *   --simple-ui         Usa interfaccia console base invece di FTXUI
 *   --no-cache          Disabilita il salvataggio della KVCache su disco
 *   --tool-limit N      Limita il numero di tool call per turno (default: 10)
 *
 * Esempi:
 *   # ModalitÃ  interattiva con TUI
 *   llama-agent -m ~/models/qwen2.5-7b-instruct-q4_k_m.gguf -c 8192
 *
 *   # ModalitÃ  singolo turno
 *   llama-agent -m model.gguf -p "Cosa sono i transformer?" --single-turn
 *
 *   # ModalitÃ  console base (utile per pipe/SSH)
 *   llama-agent -m model.gguf --simple-ui
 *
 *   # Con GPU
 *   llama-agent -m model.gguf -ngl 35
 *
 *   # Disabilitando la KVCache persistente
 *   llama-agent -m model.gguf --no-cache
 * ============================================================================
 */

#include "agent.h"
#include "ui.h"

#include "arg.h"
#include "common.h"
#include "log.h"
#include "llama.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <iostream>
#include <string>
#include <memory>

namespace fs = std::filesystem;

// ===========================================================================
// Prototipo funzione che aggiunge i flag specifici di llama-agent
// ===========================================================================

/**
 * Filtra i parametri CLI specifici di llama-agent da argv prima che
 * common_params_parse li veda (che fallirebbe su flag sconosciuti).
 *
 * I flag estratti vengono rimossi da argv spostando gli argomenti successivi.
 * Restituisce il nuovo argc.
 */
static int filter_agent_args(int argc, char ** argv,
                              bool & out_simple_ui, bool & out_no_cache,
                              int & out_tool_limit,
                              std::string & out_cache_mode)
{
    int write_idx = 1;

    for (int read_idx = 1; read_idx < argc; read_idx++) {
        std::string arg(argv[read_idx]);

        if (arg == "--simple-ui") {
            out_simple_ui = true;
            continue;
        }

        if (arg == "--no-cache") {
            out_no_cache = true;
            continue;
        }

        if (arg == "--cache-mode" && read_idx + 1 < argc) {
            out_cache_mode = argv[read_idx + 1];
            read_idx++;
            continue;
        }

        if (arg == "--tool-limit" && read_idx + 1 < argc) {
            out_tool_limit = std::atoi(argv[read_idx + 1]);
            read_idx++;
            continue;
        }

        argv[write_idx++] = argv[read_idx];
    }

    argv[write_idx] = nullptr;
    return write_idx;
}

// ===========================================================================
// Entry point
// ===========================================================================

int main(int argc, char ** argv)
{
    // --- CURL ---
    // Imposta la localitÃ  (richiesto da llama.cpp)
    std::setlocale(LC_NUMERIC, "C");

    // --- Parametri specifici di llama-agent ---
    bool simple_ui = false;
    bool no_cache = false;
    int  tool_limit = 0;   // 0 = illimitato
    std::string cache_mode = "fast";

    // --- Parametri comuni (stessi di llama-cli) ---
    common_params params;
    params.verbosity = LOG_LEVEL_ERROR;  // output meno verboso di default

    // Inizializzazione libreria common
    common_init();

    // --- Parsing argomenti CLI ---
    // Prima: filtra i flag specifici di llama-agent da argv
    argc = filter_agent_args(argc, argv, simple_ui, no_cache, tool_limit, cache_mode);

    // Poi: parsing standard dei parametri llama-cli con l'argv filtrato
    if (!common_params_parse(argc, argv, params, LLAMA_EXAMPLE_CLI)) {
        // common_params_parse mostrerÃ  l'aiuto in caso di errore
        return 1;
    }

    // Se richiesto --help o --usage, common_params_parse ha giÃ  stampato
    // l'aiuto e restituito false. Usciamo.
    if (params.usage) {
        return 0;
    }

    // --- Carica configurazione da file JSON ---
    // Ordine: ~/.config/llama-agent/config.json (globale) poi .llama-agent.json (progetto)
    auto load_config_json = [](const std::string & path,
                                std::map<std::string, std::string> & cfg) {
        std::ifstream f(path);
        if (!f.is_open()) return;
        std::string json((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
        f.close();
        // Parsing semplice: cerca "chiave": "valore" o "chiave": numero
        size_t pos = 0;
        while ((pos = json.find("\"", pos)) != std::string::npos) {
            size_t key_end = json.find("\"", pos + 1);
            if (key_end == std::string::npos) break;
            std::string key = json.substr(pos + 1, key_end - pos - 1);
            size_t colon = json.find(":", key_end);
            if (colon == std::string::npos) { pos = key_end + 1; continue; }
            size_t val_start = colon + 1;
            while (val_start < json.size() && (json[val_start] == ' ' || json[val_start] == '\t' ||
                   json[val_start] == '\n' || json[val_start] == '\r')) val_start++;
            if (val_start >= json.size()) break;
            std::string val;
            if (json[val_start] == '"') {
                size_t ve = json.find("\"", val_start + 1);
                if (ve != std::string::npos) {
                    val = json.substr(val_start + 1, ve - val_start - 1);
                    pos = ve + 1;
                } else break;
            } else {
                size_t ve = val_start;
                while (ve < json.size() && json[ve] != ',' && json[ve] != '}' &&
                       json[ve] != '\n' && json[ve] != '\r') ve++;
                val = json.substr(val_start, ve - val_start);
                while (!val.empty() && (val.back() == ' ' || val.back() == '\t'))
                    val.pop_back();
                pos = ve;
            }
            if (!key.empty() && !val.empty() && key != "permissions" && key != "skills")
                cfg[key] = val;
        }
        fprintf(stderr, "[Config] Caricato: %s (%zu chiavi)\n", path.c_str(), cfg.size());
    };

    std::map<std::string, std::string> config;
    // Globale
    {
        const char * home = getenv("HOME");
        if (home) {
            std::string global = std::string(home) + "/.config/llama-agent/config.json";
            load_config_json(global, config);
        }
    }
    // Progetto
    load_config_json(".llama-agent.json", config);

    // Applica config ai params (solo se non già impostati da CLI)
    auto set_if = [&](const std::string & key, auto & target) {
        auto it = config.find(key);
        if (it != config.end()) {
            std::stringstream ss(it->second);
            ss >> target;
        }
    };
    if (params.model.path.empty()) {
        auto it = config.find("model");
        if (it != config.end()) params.model.path = it->second;
    }
    if (params.n_ctx == 0) set_if("n_ctx", params.n_ctx);
    if (params.n_predict < 0) set_if("n_predict", params.n_predict);
    if (params.n_gpu_layers == -1) set_if("n_gpu_layers", params.n_gpu_layers);
    if (params.sampling.temp <= 0.0f) {
        auto it = config.find("temperature");
        if (it != config.end()) {
            std::stringstream ss(it->second);
            ss >> params.sampling.temp;
        }
    }
    if (cache_mode == "fast") {
        auto it = config.find("cache_mode");
        if (it != config.end()) cache_mode = it->second;
    }
    if (tool_limit == 0) {
        set_if("tool_limit", tool_limit);
    }
    if (params.model.path.empty() && params.model.hf_repo.empty()) {
        fprintf(stderr, "\033[31mErrore: nessun modello specificato.\033[0m\n");
        fprintf(stderr, "  Usa: %s -m <percorso_modello.gguf>\n", argv[0]);
        fprintf(stderr, "  Oppure: %s --hf-repo <repo> --hf-file <file>\n", argv[0]);
        fprintf(stderr, "  Usa --help per l'aiuto completo.\n");
        return 1;
    }

    // --- Configurazione cache ---
    bool cache_disabled = false;
    if (no_cache) {
        fprintf(stderr, "[Config] KVCache persistente disabilitata\n");
        cache_disabled = true;
    }
    if (params.path_prompt_cache.empty() && !cache_disabled) {
        params.path_prompt_cache = ".cache";
    }
    if (!cache_disabled) {
        fprintf(stderr, "[Config] KVCache directory: %s\n",
                params.path_prompt_cache.c_str());
    }

    // Imposta modalitÃ  cache
    CacheMode mode = CacheMode::FAST;
    if (cache_mode == "token") {
        mode = CacheMode::TOKEN;
        fprintf(stderr, "[Config] Cache mode: token (solo token, prefill all'avvio)\n");
    } else {
        fprintf(stderr, "[Config] Cache mode: fast (stato binario)\n");
    }

    fprintf(stderr, "[Config] Tool limit: %d per turno\n", tool_limit);
    fprintf(stderr, "\n");

    // --- Creazione interfaccia utente ---
    std::unique_ptr<UI> ui;

    if (simple_ui) {
        // Interfaccia console base (stile llama-cli)
        // Utile per pipe, SSH, o terminali minimali
        ui = std::make_unique<SimpleUI>();
        fprintf(stderr, "[UI] ModalitÃ  console base (--simple-ui)\n");
    } else {
        // Interfaccia FTXUI moderna (split panes, colori, input field)
        ui = std::make_unique<FTXUI>();
        fprintf(stderr, "[UI] ModalitÃ  TUI FTXUI\n");
    }

    // --- Creazione agente ---
    auto agent = std::make_unique<Agent>(params, cache_disabled);

    // Imposta modalità cache prima di init
    agent->set_cache_mode(mode);

    // Imposta limite tool call (0 = illimitato)
    agent->set_tool_limit(tool_limit);

    // --- Carica skills da directory .skills/ e ~/.config/llama-agent/skills/ ---
    {
        std::map<std::string, std::string> skills;
        auto load_skills_dir = [&](const std::string & dir) {
            if (!fs::exists(dir) || !fs::is_directory(dir)) return;
            for (const auto & entry : fs::directory_iterator(dir)) {
                if (!entry.is_directory()) continue;
                std::string skill_name = entry.path().filename().string();
                std::string skill_md = entry.path().string() + "/SKILL.md";
                if (fs::exists(skill_md)) {
                    std::ifstream f(skill_md);
                    if (f.is_open()) {
                        std::string content((std::istreambuf_iterator<char>(f)),
                                             std::istreambuf_iterator<char>());
                        skills[skill_name] = content;
                        fprintf(stderr, "[Skills] Caricata: %s (%zu byte)\n",
                                skill_name.c_str(), content.size());
                    }
                }
            }
        };
        load_skills_dir(".skills");
        const char * home = getenv("HOME");
        if (home) load_skills_dir(std::string(home) + "/.config/llama-agent/skills");
        if (!skills.empty()) {
            agent->set_skills(skills);
            fprintf(stderr, "[Skills] %zu skills caricate\n", skills.size());
        }
    }

    // --- Inizializzazione agente (carica modello e contesto) ---
    if (!agent->init()) {
        fprintf(stderr, "\033[31mErrore fatale: impossibile inizializzare l'agente.\033[0m\n");
        return 1;
    }

    // --- Inizializzazione UI ---
    ui->init(simple_ui);

    // Collega callback abort (Escape interrompe la generazione)
    Agent * raw_agent = agent.get();
    ui->set_abort_callback([raw_agent]() {
        raw_agent->abort();
        fprintf(stderr, "\n[Interrotto] Generazione fermata dall'utente.\n");
    });

    // --- Avvio agente ---
    // L'agente collega il callback dei prompt e avvia il loop della UI
    fprintf(stderr, "\n=== llama-agent avviato ===\n");
    fprintf(stderr, "  Comandi: /exit /clear /regen\n");
    fprintf(stderr, "  Scrivi un messaggio e premi Enter per iniziare.\n\n");

    agent->start(*ui);

    // --- Cleanup ---
    fprintf(stderr, "\n=== Arresto in corso... ===\n");

    ui->stop();
    agent.reset();

    fprintf(stderr, "Arrivederci!\n");

    return 0;
}
