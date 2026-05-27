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
#include <iostream>
#include <string>
#include <memory>

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
    int  tool_limit = 10;
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

    // --- Validazione parametri ---
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

    // Imposta modalitÃ  cache prima di init
    agent->set_cache_mode(mode);

    // --- Inizializzazione agente (carica modello e contesto) ---
    if (!agent->init()) {
        fprintf(stderr, "\033[31mErrore fatale: impossibile inizializzare l'agente.\033[0m\n");
        return 1;
    }

    // --- Inizializzazione UI ---
    ui->init(simple_ui);

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
