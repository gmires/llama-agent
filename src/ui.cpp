#include "ui.h"
#include "common.h"  // per common_token_to_piece (solo se serve)

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>
#include <queue>
#include <iostream>

/*
 * ============================================================================
 * UI — Implementazione base (metodi condivisi)
 *
 * Definisce la classe base UI e l'implementazione simple-console.
 * L'implementazione FTXUI è in ui_ftxui.cpp.
 * ============================================================================
 */

// ===========================================================================
// UI base
// ===========================================================================

UI::UI() = default;
UI::~UI() = default;

void UI::set_prompt_callback(PromptCallback cb)
{
    prompt_callback_ = std::move(cb);
}

void UI::init(bool /*use_simple*/)
{
    // Implementazione base: non fa nulla
}

void UI::run()
{
    // Implementazione base: non fa nulla
}

void UI::stop()
{
    running_ = false;
}

void UI::stream_token(const std::string & /*text*/, TokenType /*type*/)
{
    // Implementazione base: non fa nulla
}

void UI::show_error(const std::string & message)
{
    fprintf(stderr, "\033[31m[ERRORE]\033[0m %s\n", message.c_str());
}

void UI::show_info(const std::string & message)
{
    fprintf(stdout, "\033[32m[INFO]\033[0m %s\n", message.c_str());
}

bool UI::ask_permission(const std::string & tool_name, const std::string & resource)
{
    // Default: chiedi sempre (sicurezza)
    fprintf(stdout, "\033[33m[PERMESSO]\033[0m Il tool '%s' vuole accedere a: %s\n",
            tool_name.c_str(), resource.c_str());
    fprintf(stdout, "  Consentire? [Y/n/a(always)]: ");
    fflush(stdout);

    char response = 'n';
    int result = scanf(" %c", &response);
    (void)result;

    return (response == 'y' || response == 'Y' || response == 'a' || response == 'A');
}

void UI::update_stats(int /*tokens_generated*/, float /*tokens_per_sec*/,
                      size_t /*cache_size*/, int /*n_ctx*/, int /*n_past*/)
{
}

void UI::show_history(const std::vector<std::pair<std::string, std::string>> & /*messages*/)
{
}

void UI::show_tool_execution(const std::string & tool_name, const std::string & args)
{
    fprintf(stdout, "\033[34m[TOOL]\033[0m Esecuzione: %s (%s)\n",
            tool_name.c_str(), args.c_str());
}

void UI::clear_response()
{
    // Implementazione base: non fa nulla
}


// ===========================================================================
// SimpleUI — Implementazione console base (stile llama-cli)
// ===========================================================================

struct SimpleUI::Impl {
    bool thinking_mode = false;
    int  last_token_count = 0;
};

SimpleUI::SimpleUI()
    : pimpl_(std::make_unique<Impl>())
{
}

SimpleUI::~SimpleUI() = default;

void SimpleUI::init(bool /*use_simple*/)
{
    // Ottieni le dimensioni del terminale per il separatore
    fprintf(stdout, "\n=== llama-agent (Simple UI) ===\n");
    fprintf(stdout, "Comandi: /exit, /clear, /regen\n\n");
    fflush(stdout);
}

void SimpleUI::run()
{
    running_ = true;
    std::string line;

    // Se c'è un prompt iniziale (da -p), invialo subito
    if (!initial_prompt_.empty()) {
        std::string prompt = initial_prompt_;
        initial_prompt_.clear();
        if (prompt_callback_) {
            prompt_callback_(prompt);
        }
        // In single-turn, dopo la generazione il programma esce
        // (l'agente chiama stop() al termine)
    }

    while (running_) {
        // Mostra il prompt
        fprintf(stdout, "\n\033[36m> \033[0m");
        fflush(stdout);

        // Leggi una riga di input
        if (!std::getline(std::cin, line)) {
            break;
        }

        if (line.empty()) continue;

        // Comandi
        if (line == "/exit") {
            break;
        }

        // Invia il prompt all'agente
        if (prompt_callback_) {
            prompt_callback_(line);
        }
    }
}

void SimpleUI::stream_token(const std::string & text, TokenType type)
{
    // Layout sequenziale: THINKING poi RESPONSE, con separatore
    switch (type) {
        case TokenType::THINKING:
            if (!pimpl_->thinking_mode) {
                fprintf(stdout, "\n\033[2;36m### Thinking ###\033[0m\n");
                pimpl_->thinking_mode = true;
            }
            fprintf(stdout, "\033[2;36m%s\033[0m", text.c_str());
            break;
        case TokenType::RESPONSE:
            if (pimpl_->thinking_mode) {
                fprintf(stdout, "\n\033[2;36m### End Thinking ###\033[0m\n\n");
                pimpl_->thinking_mode = false;
            }
            fprintf(stdout, "%s", text.c_str());
            break;
        default:
            fprintf(stdout, "%s", text.c_str());
            break;
    }
    fflush(stdout);
}

void SimpleUI::show_error(const std::string & message)
{
    fprintf(stderr, "\n\033[31mErrore: %s\033[0m\n", message.c_str());
    fflush(stderr);
}

void SimpleUI::show_info(const std::string & message)
{
    fprintf(stdout, "\n\033[32m%s\033[0m\n", message.c_str());
    fflush(stdout);
}

bool SimpleUI::ask_permission(const std::string & tool_name,
                               const std::string & resource)
{
    fprintf(stdout, "\n\033[33m[Permesso]\033[0m '%s' richiede: %s\n",
            tool_name.c_str(), resource.c_str());
    fprintf(stdout, "  Consentire? [y/N/a]: ");
    fflush(stdout);

    std::string response;
    std::getline(std::cin, response);

    if (response.empty()) return false;

    char c = response[0];
    return (c == 'y' || c == 'Y' || c == 'a' || c == 'A');
}

void SimpleUI::show_tool_execution(const std::string & tool_name,
                                    const std::string & args)
{
    fprintf(stdout, "\n\033[34m[Tool] Esecuzione: %s (%s)\033[0m\n",
            tool_name.c_str(), args.c_str());
    fflush(stdout);
}

void SimpleUI::clear_response()
{
    fprintf(stdout, "\n\033[33m[Rigenerazione in corso...]\033[0m\n");
    fflush(stdout);
}

void SimpleUI::update_stats(int tokens, float tps, size_t /*cache_size*/, int /*n_ctx*/, int /*n_past*/)
{
    // Non mostriamo statistiche in tempo reale nella simple UI
    (void)tokens;
    (void)tps;
}

void SimpleUI::show_history(const std::vector<std::pair<std::string, std::string>> & msgs)
{
    fprintf(stdout, "\n\033[36m--- Cronologia caricata (%zu messaggi) ---\033[0m\n", msgs.size());
    fflush(stdout);
    (void)msgs;
}

void SimpleUI::stop()
{
    running_ = false;
}

