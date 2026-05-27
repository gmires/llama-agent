#include "ui.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <cstdio>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <queue>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <algorithm>

/*
 * ============================================================================
 * Interfaccia FTXUI — Layout sequenziale migliorato.
 *
 *   ┌──────────────────────────────────────────────────┐
 *   │  > messaggio utente                              │
 *   │  risposta precedente...                          │
 *   │                                                  │
 *   │  ── Thinking ──────────────────────────          │
 *   │  (testo pensiero in giallo dim)                  │
 *   │  ──────────────────────────────────────          │
 *   │                                                  │
 *   │  Risposta corrente...                            │
 *   ├──────────────────────────────────────────────────┤
 *   │  > Input multilinea...                           │
 *   │  Ctrl+Enter per andare a capo                   │
 *   ├──────────────────────────────────────────────────┤
 *   │  Token: 123  T/s: 5.2  CTX: 45%  [Stato]       │
 *   └──────────────────────────────────────────────────┘
 *
 * Caratteristiche:
 *   - Input multi-riga (Ctrl+Enter = newline, Enter = invia)
 *   - Thinking con header giallo ben visibile
 *   - Barra di contesto sempre visibile
 *   - Overlay permessi centrato condividato
 * ============================================================================
 */

#if !defined(LLAMA_AGENT_SIMPLE_UI)

using namespace ftxui;

// ===========================================================================
// Struttura interna (PIMPL)
// ===========================================================================

struct FTXUI::Impl {
    ScreenInteractive screen{ScreenInteractive::Fullscreen()};
    Component container;

    // Buffer di testo (thread-safe)
    std::mutex  text_mutex;
    std::string chat_log;
    std::string thinking_text;
    std::string response_text;
    std::string footer_text;
    std::string input_text;
    std::string input_placeholder = "Scrivi un messaggio (Ctrl+Enter per nuova riga, Enter per inviare)...";

    // Scroll state: 1.0 = fondo (auto), 0.0 = inizio
    float scroll_y = 1.0f;

    void append_to_chat(const std::string & role, const std::string & text) {
        std::lock_guard<std::mutex> tl(text_mutex);
        if (role == "user") {
            chat_log += "> " + text + "\n";
        } else if (role == "assistant" || role == "model") {
            chat_log += text + "\n\n";
        } else if (role == "thinking") {
            chat_log += "\u2500\u2500 Thinking \u2500\u2500\n" + text + "\n\u2500\u2500\u2500\u2500\u2500\u2500\n";
        } else if (role == "tool_start") {
            chat_log += "[TOOL] " + text + "\n";
        } else {
            chat_log += text + "\n";
        }
    }

    // Cronologia prompt (freccette su/giù)
    std::vector<std::string> prompt_history;
    int prompt_history_idx = -1;

    // Coda token dal thread di inferenza
    std::mutex  queue_mutex;
    std::queue<std::pair<std::string, TokenType>> token_queue;

    PromptCallback prompt_callback;

    std::atomic<bool>  running{false};
    std::atomic<bool>  generating{false};
    std::chrono::steady_clock::time_point generating_since;

    // Permessi
    std::atomic<bool>          permission_pending{false};
    std::string                permission_tool;
    std::string                permission_resource;
    std::mutex                 permission_mutex;
    std::condition_variable    permission_cv;
    bool                       permission_allowed = false;

    Component build_layout()
    {
        auto renderer = Renderer([this] {
            // --- Watchdog: resetta generating se bloccato da troppo tempo ---
            if (generating) {
                auto now = std::chrono::steady_clock::now();
                if (now - generating_since > std::chrono::minutes(15)) {
                    generating = false;
                    std::lock_guard<std::mutex> tl(text_mutex);
                    footer_text = "[Watchdog] Generazione bloccata, reset.";
                }
            }

            // --- Svuota coda token ---
            {
                std::lock_guard<std::mutex> ql(queue_mutex);
                while (!token_queue.empty()) {
                    auto [text, type] = token_queue.front();
                    token_queue.pop();
                    std::lock_guard<std::mutex> tl(text_mutex);
                    switch (type) {
                        case TokenType::THINKING: thinking_text += text; break;
                        case TokenType::TOOL_CALL:
                        case TokenType::RESPONSE:
                        case TokenType::UNKNOWN:
                        default:                  response_text += text; break;
                    }
                }
            }

            std::lock_guard<std::mutex> tl(text_mutex);

            // Spinner
            static int spinner_frame = 0;
            if (generating) spinner_frame++;
            const char * spinner_chars = "|/-\\";
            std::string spinner_str;
            if (generating) {
                spinner_str = " " + std::string(1, spinner_chars[(spinner_frame / 4) % 4]);
            }

            // --- Cronologia conversazione (con colori per ruolo) ---
            Elements chat_elements;
            if (!chat_log.empty()) {
                std::istringstream stream(chat_log);
                std::string line;
                bool in_code_block = false;
                std::string code_lang;
                while (std::getline(stream, line)) {
                    // Rimuovi carriage return
                    while (!line.empty() && line.back() == '\r')
                        line.pop_back();

                    // Code block start/end
                    if (line.size() >= 3 && line.substr(0, 3) == "```") {
                        if (!in_code_block) {
                            in_code_block = true;
                            code_lang = line.size() > 3 ? line.substr(3) : "";
                            chat_elements.push_back(
                                text("\u250C\u2500 " + code_lang) | dim | color(Color::GrayDark));
                        } else {
                            in_code_block = false;
                            chat_elements.push_back(
                                text("\u2514\u2500") | dim | color(Color::GrayDark));
                        }
                        continue;
                    }

                    if (in_code_block) {
                        chat_elements.push_back(
                            text(" " + line) | color(Color::CyanLight) | bgcolor(Color::Grey19));
                    } else if (line.size() > 2 && line[0] == '>' && line[1] == ' ') {
                        chat_elements.push_back(
                            text(line) | bold | color(Color::GreenLight));
                    } else if (line.size() > 2 && line[0] == '#' && line[1] == '#') {
                        chat_elements.push_back(
                            text(line) | bold | color(Color::YellowLight));
                    } else if (line.size() > 1 && line[0] == '#') {
                        chat_elements.push_back(
                            text(line) | bold | color(Color::Yellow));
                    } else if (line.size() > 4 && line.substr(0, 4) == "[TOO") {
                        chat_elements.push_back(
                            text(line) | dim | color(Color::BlueLight));
                    } else {
                        chat_elements.push_back(
                            text(line) | color(Color::White));
                    }
                }
            }
            Element chat_log_elem = vbox(chat_elements);

            // --- Sezione THINKING (se presente) ---
            Element thinking_elem = emptyElement();
            if (!thinking_text.empty()) {
                thinking_elem = vbox(Elements{
                    text(" \u2500\u2500 Thinking \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500")
                        | color(Color::Yellow) | bold,
                    paragraph(thinking_text) | color(Color::YellowLight),
                    text(" \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500")
                        | color(Color::Yellow),
                    text(""),
                });
            }

            // --- Sezione RESPONSE (turno corrente) ---
            Element response_elem = emptyElement();
            if (!response_text.empty()) {
                response_elem = vbox(Elements{
                    paragraph(response_text) | color(Color::White),
                });
            }

            // --- Input multilinea ---
            std::string display_text = input_text.empty() ? input_placeholder : input_text;
            int input_lines = 1 + (int)std::count(input_text.begin(), input_text.end(), '\n');
            if (input_lines < 3) input_lines = 3;
            if (input_lines > 8) input_lines = 8;

            Element input_area = vbox(Elements{
                hbox(Elements{
                    text(" > ") | bold | color(Color::Green),
                    paragraph(display_text)
                        | color(input_text.empty() ? Color::GrayDark : Color::White)
                        | flex,
                }),
            }) | bgcolor(Color::Black) | yflex | size(HEIGHT, LESS_THAN, input_lines + 1);

            // --- Suggerimenti slash ---
            Element hint_elem = emptyElement();
            if (!input_text.empty() && input_text[0] == '/' && !generating) {
                hint_elem = text("  /help  /clear  /regen  /exit")
                    | color(Color::GrayDark);
            }

            // --- Barra di stato ---
            Element stats_elem = text(footer_text) | color(Color::GrayLight) | flex;
            Element footer_bar = hbox(Elements{
                text(spinner_str) | color(Color::Green) | bold,
                stats_elem,
            }) | hcenter | bgcolor(Color::Black);

            // --- Area di contenuto con scroll ---
            Element content = vbox(Elements{
                chat_log_elem,
                thinking_elem,
                response_elem,
            });

            // Durante la generazione, il contenuto scorre automaticamente in fondo
            Element content_area;
            if (generating) {
                scroll_y = 1.0f;
            }
            content_area = content | focusPositionRelative(0.0f, scroll_y) | frame | flex;

            // --- Layout verticale completo ---
            Element doc = vbox(Elements{
                content_area | flex,
                input_area,
                hint_elem,
                footer_bar,
            });

            // --- Overlay permessi ---
            if (permission_pending) {
                std::string perm_title = "   Permesso: " + permission_tool + "   ";
                std::string perm_body = permission_resource;
                if (perm_body.size() > 80)
                    perm_body = perm_body.substr(0, 80) + "...";
                Element overlay = vbox(Elements{
                    text(perm_title) | bold | hcenter | color(Color::White) | bgcolor(Color::Blue),
                    text(""),
                    paragraph(perm_body) | color(Color::White),
                    text(""),
                    text("[y] Allow  [n] Deny  [a] Always allow") | hcenter | color(Color::Yellow),
                }) | border | bgcolor(Color::Black);

                doc = dbox(Elements{
                    doc,
                    overlay | center | bgcolor(Color::Black),
                });
            }

            return doc;
        });

        // Gestione eventi
        auto event_handler = CatchEvent(renderer, [this](Event event) {
            // Permessi pending: intercetta y/n/a
            if (permission_pending) {
                if (event.is_character()) {
                    std::string ch = event.character();
                    bool allowed = false;
                    if (ch == "y" || ch == "Y" || ch == "a" || ch == "A") {
                        allowed = true;
                    } else if (ch == "n" || ch == "N") {
                        allowed = false;
                    } else {
                        return true;
                    }
                    {
                        std::lock_guard<std::mutex> lk(permission_mutex);
                        permission_allowed = allowed;
                        permission_pending = false;
                    }
                    permission_cv.notify_one();
                }
                return true;
            }

            // Enter: invia il prompt
            if (event == Event::Return) {
                if (generating) return true;

                std::string prompt;
                {
                    std::lock_guard<std::mutex> tl(text_mutex);
                    prompt = input_text;
                    input_text.clear();
                }
                // Normalizza: rimuovi newline finali
                while (!prompt.empty() && (prompt.back() == '\n' || prompt.back() == '\r'))
                    prompt.pop_back();
                // Se vuoto dopo normalizzazione, svuota comunque
                if (prompt.empty()) {
                    return true;
                }

                // Salva nella cronologia
                if (prompt_history.empty() || prompt_history.back() != prompt) {
                    prompt_history.push_back(prompt);
                }
                prompt_history_idx = -1;

                // Prepara UI
                {
                    std::lock_guard<std::mutex> tl(text_mutex);
                    chat_log += "> " + prompt + "\n";
                    thinking_text.clear();
                    response_text.clear();
                    footer_text = "Generazione in corso...";
                }

                if (prompt == "/exit") {
                    screen.Exit();
                    return true;
                }

                generating = true;
                generating_since = std::chrono::steady_clock::now();
                if (prompt_callback) {
                    prompt_callback(prompt);
                }
                return true;
            }

            // Ctrl+J (^J, Ctrl+Enter in molti terminali): inserisce newline
            if (event == Event::CtrlJ || (event.is_character() && event.character() == "\n")) {
                if (generating) return true;
                std::lock_guard<std::mutex> tl(text_mutex);
                input_text += '\n';
                return true;
            }

            // Backspace
            if (event == Event::Backspace) {
                std::lock_guard<std::mutex> tl(text_mutex);
                if (!input_text.empty()) {
                    input_text.pop_back();
                }
                return true;
            }

            // Caratteri stampabili
            if (event.is_character() && event.character() != "\n" && event.character() != "\r") {
                std::lock_guard<std::mutex> tl(text_mutex);
                input_text += event.character();
                return true;
            }

            // Freccia SU: cronologia
            if (event == Event::ArrowUp) {
                std::lock_guard<std::mutex> tl(text_mutex);
                if (!prompt_history.empty()) {
                    if (prompt_history_idx < 0)
                        prompt_history_idx = (int)prompt_history.size() - 1;
                    else if (prompt_history_idx > 0)
                        prompt_history_idx--;
                    input_text = prompt_history[prompt_history_idx];
                }
                return true;
            }

            // Freccia GIU: cronologia
            if (event == Event::ArrowDown) {
                std::lock_guard<std::mutex> tl(text_mutex);
                if (prompt_history_idx >= 0) {
                    prompt_history_idx++;
                    if (prompt_history_idx >= (int)prompt_history.size()) {
                        prompt_history_idx = -1;
                        input_text.clear();
                    } else {
                        input_text = prompt_history[prompt_history_idx];
                    }
                }
                return true;
            }

            // PageUp: scrolla su di ~1/3 pagina
            if (event == Event::PageUp) {
                scroll_y = std::max(0.0f, scroll_y - 0.3f);
                return true;
            }

            // PageDown: scrolla giu di ~1/3 pagina
            if (event == Event::PageDown) {
                scroll_y = std::min(1.0f, scroll_y + 0.3f);
                return true;
            }

            // Home: vai all'inizio
            if (event == Event::Home) {
                scroll_y = 0.0f;
                return true;
            }

            // End: vai alla fine
            if (event == Event::End) {
                scroll_y = 1.0f;
                return true;
            }

            return false;
        });

        return event_handler;
    }
};

// ===========================================================================
// Costruttore / Distruttore
// ===========================================================================

FTXUI::FTXUI() : pimpl_(std::make_unique<Impl>()) {}
FTXUI::~FTXUI() { stop(); }

// ===========================================================================
// init
// ===========================================================================

void FTXUI::init(bool /*use_simple*/)
{
    pimpl_->running = true;
    pimpl_->footer_text = "Pronto. Carica un modello con -m <path> e scrivi un messaggio.";
    pimpl_->prompt_callback = prompt_callback_;
    pimpl_->container = pimpl_->build_layout();
}

// ===========================================================================
// set_prompt_callback
// ===========================================================================

void FTXUI::set_prompt_callback(PromptCallback cb)
{
    UI::set_prompt_callback(std::move(cb));
    if (pimpl_) {
        pimpl_->prompt_callback = prompt_callback_;
    }
}

// ===========================================================================
// run
// ===========================================================================

void FTXUI::run()
{
    if (!initial_prompt_.empty()) {
        std::string prompt = initial_prompt_;
        initial_prompt_.clear();
        {
            std::lock_guard<std::mutex> tl(pimpl_->text_mutex);
            pimpl_->thinking_text.clear();
            pimpl_->response_text.clear();
            pimpl_->footer_text = "Generazione in corso...";
        }
        pimpl_->generating = true;
        pimpl_->generating_since = std::chrono::steady_clock::now();
        if (prompt_callback_) {
            prompt_callback_(prompt);
        }
    }
    pimpl_->screen.Loop(pimpl_->container);
}

// ===========================================================================
// stop
// ===========================================================================

void FTXUI::stop()
{
    pimpl_->running = false;
    pimpl_->screen.Exit();
}

// ===========================================================================
// stream_token
// ===========================================================================

void FTXUI::stream_token(const std::string & text, TokenType type)
{
    {
        std::lock_guard<std::mutex> ql(pimpl_->queue_mutex);
        pimpl_->token_queue.push({text, type});
    }
    pimpl_->screen.RequestAnimationFrame();
}

// ===========================================================================
// show_error / show_info
// ===========================================================================

void FTXUI::show_error(const std::string & message)
{
    {
        std::lock_guard<std::mutex> tl(pimpl_->text_mutex);
        pimpl_->footer_text = "[ERRORE] " + message;
    }
    pimpl_->generating = false;
    pimpl_->screen.RequestAnimationFrame();
}

void FTXUI::show_info(const std::string & message)
{
    {
        std::lock_guard<std::mutex> tl(pimpl_->text_mutex);
        pimpl_->footer_text = message;
    }
    pimpl_->screen.RequestAnimationFrame();
}

// ===========================================================================
// set_generating
// ===========================================================================

void FTXUI::set_generating(bool gen)
{
    {
        std::lock_guard<std::mutex> tl(pimpl_->text_mutex);
        if (!gen && pimpl_->generating) {
            if (!pimpl_->thinking_text.empty())
                pimpl_->chat_log += "\u2500\u2500 Thinking \u2500\u2500\n" + pimpl_->thinking_text + "\n\u2500\u2500\u2500\u2500\u2500\u2500\n\n";
            if (!pimpl_->response_text.empty())
                pimpl_->chat_log += pimpl_->response_text + "\n\n";
            pimpl_->thinking_text.clear();
            pimpl_->response_text.clear();
            pimpl_->footer_text = "Pronto. Scrivi o /help per comandi.";
        }
    }
    pimpl_->generating = gen;
    pimpl_->screen.RequestAnimationFrame();
}

// ===========================================================================
// ask_permission
// ===========================================================================

bool FTXUI::ask_permission(const std::string & tool_name,
                            const std::string & resource)
{
    {
        std::lock_guard<std::mutex> lk(pimpl_->permission_mutex);
        pimpl_->permission_tool = tool_name;
        pimpl_->permission_resource = resource;
        pimpl_->permission_allowed = false;
        pimpl_->permission_pending = true;
    }
    pimpl_->screen.RequestAnimationFrame();

    {
        std::unique_lock<std::mutex> lk(pimpl_->permission_mutex);
        pimpl_->permission_cv.wait(lk, [this] {
            return !pimpl_->permission_pending;
        });
    }

    return pimpl_->permission_allowed;
}

// ===========================================================================
// update_stats
// ===========================================================================

void FTXUI::update_stats(int tokens_generated, float tokens_per_sec,
                          size_t cache_size, int n_ctx, int n_past)
{
    std::stringstream ss;
    ss << "Token: " << tokens_generated
       << "  T/s: " << std::fixed << std::setprecision(1) << tokens_per_sec;

    if (n_ctx > 0) {
        int pct = (int)((float)n_past / n_ctx * 100.0f + 0.5f);
        ss << "  CTX: " << n_past << "/" << n_ctx << " (" << pct << "%)";
    } else {
        ss << "  Cache: ";
        if (cache_size > 1024 * 1024)
            ss << (cache_size / (1024 * 1024)) << "MB";
        else if (cache_size > 1024)
            ss << (cache_size / 1024) << "KB";
        else
            ss << cache_size << "B";
    }

    {
        std::lock_guard<std::mutex> tl(pimpl_->text_mutex);
        pimpl_->footer_text = ss.str();
    }
    pimpl_->screen.RequestAnimationFrame();
}

// ===========================================================================
// show_history
// ===========================================================================

void FTXUI::show_history(const std::vector<std::pair<std::string, std::string>> & messages)
{
    std::lock_guard<std::mutex> tl(pimpl_->text_mutex);
    pimpl_->chat_log.clear();
    for (const auto & [role, content] : messages) {
        if (role == "user") {
            pimpl_->chat_log += "> " + content + "\n";
        } else if (role == "assistant" || role == "model") {
            pimpl_->chat_log += content + "\n\n";
        } else if (role == "system") {
            pimpl_->chat_log += "[SYSTEM] " + content + "\n";
        } else if (role == "tool") {
            pimpl_->chat_log += "[TOOL] " + content + "\n";
        }
    }
    pimpl_->screen.RequestAnimationFrame();
}

// ===========================================================================
// show_tool_execution
// ===========================================================================

void FTXUI::show_tool_execution(const std::string & tool_name,
                                 const std::string & args)
{
    {
        std::lock_guard<std::mutex> tl(pimpl_->text_mutex);
        pimpl_->footer_text = "[Tool] " + tool_name + "(" + args + ")";
    }
    pimpl_->screen.RequestAnimationFrame();
}

// ===========================================================================
// clear_response
// ===========================================================================

void FTXUI::clear_response()
{
    {
        std::lock_guard<std::mutex> tl(pimpl_->text_mutex);
        pimpl_->thinking_text.clear();
        pimpl_->response_text.clear();
    }
    pimpl_->generating = true;
    pimpl_->generating_since = std::chrono::steady_clock::now();
    pimpl_->footer_text = "Rigenerazione...";
    pimpl_->screen.RequestAnimationFrame();
}

#endif // !defined(LLAMA_AGENT_SIMPLE_UI)
