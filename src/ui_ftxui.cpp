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
#include <vector>
#include <filesystem>
namespace fs = std::filesystem;

/*
 * ============================================================================
 * Interfaccia FTXUI — Stile pi: messaggi strutturati, tool call a blocchi.
 *
 *   ┌─ Chat ──────────────────────────────────────────┐
 *   │                                                  │
 *   │  >  utente: messaggio verde                      │
 *   │                                                  │
 *   │  assistant: risposta bianca con markdown         │
 *   │  ```cpp                                          │
 *   │  code block con sfondo                           │
 *   │  ```                                             │
 *   │                                                  │
 *   │  ╭─ Tool: write (path="x", content="...") ──────╮
 *   │  │ OK: 523 bytes scritti                         │
 *   │  ╰───────────────────────────────────────────────╯
 *   │                                                  │
 *   │  ── Thinking ────────────────────────────        │
 *   │  (pensiero in giallo dim, collassabile col T)    │
 *   │  ────────────────────────────────────────        │
 *   │                                                  │
 *   ├──────────────────────────────────────────────────┤
 *   │  >  Input multilinea                             │
 *   ├──────────────────────────────────────────────────┤
 *   │  Token: 123  T/s: 5.2  CTX: 45%  (PgUp/PgDn)   │
 *   └──────────────────────────────────────────────────┘
 * ============================================================================
 */

#if !defined(LLAMA_AGENT_SIMPLE_UI)

using namespace ftxui;

struct FTXUI::Impl {
    ScreenInteractive screen{ScreenInteractive::Fullscreen()};
    Component container;

    // --- Messaggi strutturati ---
    enum class MsgType { USER, ASSISTANT, THINKING, TOOL_CALL, TOOL_RESULT, SYSTEM };

    struct ChatMsg {
        MsgType type;
        std::string text;       // contenuto
        std::string extra;      // nome tool, language code block, ...
    };

    std::mutex  text_mutex;
    std::vector<ChatMsg> messages;   // cronologia completa (persistente)
    std::string thinking_text;       // thinking accumulato durante generazione
    std::string response_text;       // response accumulato durante generazione
    std::string footer_text;
    std::string input_text;
    int input_cursor = 0;           // posizione cursore nell'input
    std::string input_placeholder = "Scrivi (Ctrl+N = a capo, Enter = invia)...";

    // Scroll: 1.0 = fondo, 0.0 = inizio
    float scroll_y = 1.0f;
    bool  thinking_collapsed = false;  // Ctrl+T per collassare/espandere thinking

    // Cronologia prompt
    std::vector<std::string> prompt_history;
    int prompt_history_idx = -1;

    // Coda token dal thread di inferenza
    std::mutex  queue_mutex;
    std::queue<std::pair<std::string, TokenType>> token_queue;

    PromptCallback prompt_callback;
    AbortCallback abort_callback;
    std::atomic<bool> running{false};
    std::atomic<bool> generating{false};
    std::chrono::steady_clock::time_point generating_since;

    // Permessi
    std::atomic<bool>       permission_pending{false};
    std::string             permission_tool;
    std::string             permission_resource;
    std::mutex              permission_mutex;
    std::condition_variable permission_cv;
    bool                    permission_allowed = false;

    // --- Helper: aggiunge un messaggio strutturato ---
    void push_msg(MsgType type, const std::string & text, const std::string & extra = "") {
        std::lock_guard<std::mutex> tl(text_mutex);
        messages.push_back({type, text, extra});
    }

    // --- Helper: tronca una stringa a max_len, aggiunge ... ---
    static std::string truncate(const std::string & s, size_t max_len = 60) {
        if (s.size() <= max_len) return s;
        return s.substr(0, max_len) + "...";
    }

    // --- Renderizza un singolo messaggio della cronologia ---
    Element render_msg(const ChatMsg & msg) const {
        switch (msg.type) {
        case MsgType::USER:
            return paragraph(" \u276F " + msg.text)
                | bold | color(Color::GreenLight);

        case MsgType::ASSISTANT: {
            Elements blocks;
            std::istringstream ss(msg.text);
            std::string line;
            std::string block_buf;
            bool in_code = false;
            while (std::getline(ss, line)) {
                while (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.size() >= 3 && line.substr(0, 3) == "```") {
                    if (in_code) {
                        blocks.push_back(
                            paragraph(" " + block_buf)
                                | color(Color::CyanLight) | bgcolor(Color::Grey19));
                        block_buf.clear();
                        in_code = false;
                    } else {
                        if (!block_buf.empty()) {
                            blocks.push_back(paragraph(" " + block_buf) | color(Color::White));
                            block_buf.clear();
                        }
                        in_code = true;
                    }
                } else if (in_code) {
                    block_buf += (block_buf.empty() ? "" : "\n") + line;
                } else if (!line.empty() && line[0] == '#' && line.size() > 1 && line[1] == '#') {
                    blocks.push_back(paragraph(" " + line) | bold | color(Color::YellowLight));
                } else if (!line.empty() && line[0] == '#') {
                    blocks.push_back(paragraph(" " + line) | bold | color(Color::Yellow));
                } else {
                    if (!line.empty()) {
                        block_buf += (block_buf.empty() ? "" : "\n") + line;
                    } else {
                        if (!block_buf.empty()) {
                            blocks.push_back(paragraph(" " + block_buf) | color(Color::White));
                            block_buf.clear();
                        }
                        blocks.push_back(text(""));
                    }
                }
            }
            if (!block_buf.empty()) {
                if (in_code) {
                    blocks.push_back(paragraph(" " + block_buf) | color(Color::CyanLight) | bgcolor(Color::Grey19));
                    blocks.push_back(text(" \u2514\u2500 [code block non chiuso]") | dim | color(Color::GrayDark));
                } else {
                    blocks.push_back(paragraph(" " + block_buf) | color(Color::White));
                }
            } else if (in_code) {
                blocks.push_back(text(" \u250C\u2500 [code block vuoto]") | dim | color(Color::GrayDark));
            }
            return vbox(blocks);
        }

        case MsgType::THINKING: {
            Elements blocks;
            if (thinking_collapsed) {
                blocks.push_back(text(" \u25B6 Thinking  [Ctrl+T/F2 per espandere]") | dim | color(Color::Yellow));
            } else {
                blocks.push_back(text(" \u25BC Thinking  [Ctrl+T/F2 per comprimere]") | dim | color(Color::Yellow));
                blocks.push_back(paragraph("   " + msg.text) | color(Color::YellowLight) | dim);
            }
            return vbox(blocks);
        }

        case MsgType::TOOL_CALL: {
            std::string title = msg.text.empty()
                ? " Tool: " + msg.extra + " "
                : " Tool: " + msg.extra + " (" + truncate(msg.text, 60) + ") ";
            return text(" \u25B8" + title) | bold | color(Color::BlueLight);
        }

        case MsgType::TOOL_RESULT:
            return paragraph("   \u2502 " + msg.text)
                | color(Color::BlueLight) | dim;

        case MsgType::SYSTEM:
            return paragraph(" " + msg.text) | dim | color(Color::White)
                | bgcolor(Color::Grey23);

        default:
            return paragraph(msg.text) | color(Color::White);
        }
    }

    // --- Layout principale ---
    Component build_layout() {
        auto renderer = Renderer([this] {
            // --- Watchdog ---
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
                        case TokenType::TOOL_CALL: {
                            if (!response_text.empty()) {
                                messages.push_back({MsgType::ASSISTANT, response_text});
                                response_text.clear();
                            }
                            // Estrai nome tool: da ">> nome(args)" -> nome
                            std::string tool_text = text;
                            std::string tool_name;
                            size_t gt = tool_text.find(">> ");
                            if (gt != std::string::npos) {
                                size_t start = gt + 3;
                                size_t paren = tool_text.find("(", start);
                                if (paren != std::string::npos)
                                    tool_name = tool_text.substr(start, paren - start);
                                // Prendi solo gli args per il testo
                                std::string args = tool_text.substr(paren + 1);
                                if (!args.empty() && args.back() == ')')
                                    args.pop_back();
                                messages.push_back({MsgType::TOOL_CALL, args, tool_name});
                            } else {
                                messages.push_back({MsgType::TOOL_CALL, tool_text, ""});
                            }
                            break;
                        }
                        case TokenType::SYSTEM:
                            // Output comandi slash: push direttamente come SYSTEM
                            if (!response_text.empty()) {
                                messages.push_back({MsgType::ASSISTANT, response_text});
                                response_text.clear();
                            }
                            messages.push_back({MsgType::SYSTEM, text});
                            break;
                        case TokenType::RESPONSE:
                        case TokenType::UNKNOWN:
                        default: response_text += text; break;
                    }
                }
            }

            std::lock_guard<std::mutex> tl(text_mutex);

            // Spinner
            static int spinner_frame = 0;
            if (generating) spinner_frame++;
            const char * sp = "|/-\\";
            std::string spinner_str;
            if (generating) spinner_str = " " + std::string(1, sp[(spinner_frame / 4) % 4]);

            // --- Costruisci elementi chat ---
            Elements content_elems;

            // Messaggi storici
            for (const auto & msg : messages) {
                content_elems.push_back(render_msg(msg));
                content_elems.push_back(text("")); // spaziatura
            }

            // Thinking in corso
            if (!thinking_text.empty()) {
                if (thinking_collapsed) {
                    content_elems.push_back(
                        text(" \u25B6 Thinking...  [Ctrl+T/F2]") | dim | color(Color::Yellow));
                } else {
                    content_elems.push_back(
                        text(" \u25BC Thinking  [Ctrl+T/F2]") | dim | color(Color::Yellow));
                    content_elems.push_back(
                        paragraph("   " + thinking_text) | color(Color::YellowLight) | dim);
                }
                content_elems.push_back(text(""));
            }

            // Response in corso
            if (!response_text.empty()) {
                Elements resp_blocks;
                std::istringstream ss(response_text);
                std::string line;
                std::string block_buf;
                bool in_code = false;
                while (std::getline(ss, line)) {
                    while (!line.empty() && line.back() == '\r') line.pop_back();
                    if (line.size() >= 3 && line.substr(0, 3) == "```") {
                        if (in_code) {
                            resp_blocks.push_back(
                                paragraph(" " + block_buf) | color(Color::CyanLight) | bgcolor(Color::Grey19));
                            block_buf.clear();
                            in_code = false;
                        } else {
                            if (!block_buf.empty()) {
                                resp_blocks.push_back(paragraph(" " + block_buf) | color(Color::White));
                                block_buf.clear();
                            }
                            in_code = true;
                        }
                    } else if (in_code) {
                        block_buf += (block_buf.empty() ? "" : "\n") + line;
                    } else {
                        if (!line.empty()) {
                            block_buf += (block_buf.empty() ? "" : "\n") + line;
                        } else {
                            if (!block_buf.empty()) {
                                resp_blocks.push_back(paragraph(" " + block_buf) | color(Color::White));
                                block_buf.clear();
                            }
                            resp_blocks.push_back(text(""));
                        }
                    }
                }
                if (!block_buf.empty()) {
                    if (in_code) {
                        resp_blocks.push_back(paragraph(" " + block_buf) | color(Color::CyanLight) | bgcolor(Color::Grey19));
                        resp_blocks.push_back(text(" \u2514\u2500 [code block non chiuso]") | dim | color(Color::GrayDark));
                    } else {
                        resp_blocks.push_back(paragraph(" " + block_buf) | color(Color::White));
                    }
                } else if (in_code) {
                    resp_blocks.push_back(text(" \u250C\u2500 [code block vuoto]") | dim | color(Color::GrayDark));
                }
                content_elems.push_back(vbox(resp_blocks));
                content_elems.push_back(text(""));
            }

            // --- Input con cursore visibile e supporto multilinea ---
            int term_w = std::max(80, screen.dimx());
            int max_input_lines = std::max(5, screen.dimy() / 5);

            Element input_area;
            if (input_text.empty()) {
                input_area = paragraph(" \u276F " + input_placeholder)
                    | color(Color::GrayDark)
                    | size(WIDTH, LESS_THAN, term_w - 2);
            } else {
                // Calcola su quale linea si trova il cursore e in quale colonna
                int cursor_line = 0, col = 0;
                for (int idx = 0; idx < input_cursor && idx < (int)input_text.size(); idx++) {
                    if (input_text[idx] == '\n') { cursor_line++; col = 0; }
                    else col++;
                }

                // Split input in righe
                std::vector<std::string> lines;
                std::istringstream iss(input_text);
                std::string l;
                while (std::getline(iss, l)) lines.push_back(l);
                if (!input_text.empty() && input_text.back() == '\n')
                    lines.push_back(""); // riga vuota finale

                if (lines.empty()) lines.push_back("");

                Elements line_elems;
                for (int li = 0; li < (int)lines.size() && li < max_input_lines; li++) {
                    std::string prefix = li == 0 ? " \u276F " : "   ";
                    if (li == cursor_line) {
                        // Mostra cursore come marker █ nella riga corrente
                        int cc = std::min(col, (int)lines[li].size());
                        std::string marked = lines[li];
                        if (cc < (int)marked.size())
                            marked.insert(cc, "\033[7m \033[27m"); // non funziona in FTXUI
                        // Invece: mostra linea con marker visibile
                        std::string show = prefix + lines[li];
                        if (cc < (int)lines[li].size())
                            show = prefix + lines[li].substr(0, cc) + "\u2588" + lines[li].substr(cc);
                        else
                            show = prefix + lines[li] + "\u2588";
                        line_elems.push_back(
                            paragraph(show) | color(Color::White) | bgcolor(Color::Grey11)
                                | size(WIDTH, LESS_THAN, term_w - 2));
                    } else {
                        line_elems.push_back(
                            paragraph(prefix + lines[li]) | color(Color::White)
                                | size(WIDTH, LESS_THAN, term_w - 2));
                    }
                }
                if ((int)lines.size() > max_input_lines) {
                    line_elems.push_back(
                        text("   ... (+" + std::to_string(lines.size() - max_input_lines) + " righe)")
                            | dim | color(Color::GrayDark));
                }
                input_area = vbox(line_elems)
                    | borderEmpty
                    | size(HEIGHT, LESS_THAN, max_input_lines + 2);
            }

            // --- Hint comandi ---
            Element hint = emptyElement();
            if (!input_text.empty() && input_text[0] == '/' && !generating) {
                hint = hbox(Elements{
                    text(" ") | size(WIDTH, EQUAL, 2),
                    text("/help /clear /regen /compact /model /session /stats /exit") | dim | color(Color::GrayDark),
                });
            }

            // --- Footer con shortcut ---
            Element footer_el = hbox(Elements{
                text(" ") | size(WIDTH, EQUAL, 1),
                text(spinner_str) | color(Color::Green) | bold,
                text(" ") | size(WIDTH, EQUAL, 1),
                text(footer_text) | color(Color::GrayLight) | flex,
                text("\u2190\u2192 cursore  Tab=completa  Esc=stop  PgUp/Dn=scroll") | dim | color(Color::GrayDark),
            }) | bgcolor(Color::Grey15);

            // --- Content area con scroll solo verticale ---
            if (generating) scroll_y = 1.0f;
            Element content = vbox(content_elems)
                | size(WIDTH, LESS_THAN, term_w - 2);
            Element content_area = content
                | focusPositionRelative(0.0f, scroll_y)
                | yframe | flex;

            // --- Layout completo ---
            Element doc = vbox(Elements{
                content_area | flex,
                input_area,
                hint,
                footer_el,
            });

            // --- Overlay permessi ---
            if (permission_pending) {
                std::string t = permission_tool;
                std::string b = permission_resource;
                if (b.size() > 80) b = b.substr(0, 80) + "...";
                Element ov = vbox(Elements{
                    text("   Permesso: " + t + "   ") | bold | hcenter
                        | color(Color::White) | bgcolor(Color::Blue),
                    text(""),
                    paragraph(b) | color(Color::White),
                    text(""),
                    text("[y] Allow  [n] Deny  [a] Always allow")
                        | hcenter | color(Color::Yellow),
                }) | border | bgcolor(Color::Grey19);
                doc = dbox(Elements{doc, ov | center});
            }

            return doc;
        });

        // --- Gestione eventi ---
        auto handler = CatchEvent(renderer, [this](Event event) {
            // Escape: interrompe la generazione in corso
            if (event == Event::Escape && generating) {
                if (abort_callback) abort_callback();
                return true;
            }

            if (permission_pending) {
                if (event.is_character()) {
                    std::string ch = event.character();
                    bool allowed = false;
                    if (ch == "y" || ch == "Y" || ch == "a" || ch == "A") allowed = true;
                    else if (ch == "n" || ch == "N") allowed = false;
                    else return true;
                    {
                        std::lock_guard<std::mutex> lk(permission_mutex);
                        permission_allowed = allowed;
                        permission_pending = false;
                    }
                    permission_cv.notify_one();
                }
                return true;
            }

            // Ctrl+T o F2: toggle thinking collapse
            if ((event == Event::CtrlT || event == Event::F2) && !generating) {
                thinking_collapsed = !thinking_collapsed;
                return true;
            }

            // Enter: invia
            if (event == Event::Return) {
                if (generating) return true;
                std::string prompt;
                {
                    std::lock_guard<std::mutex> tl(text_mutex);
                    prompt = input_text;
                    input_text.clear();
                    input_cursor = 0;
                }
                while (!prompt.empty() && (prompt.back() == '\n' || prompt.back() == '\r'))
                    prompt.pop_back();
                if (prompt.empty()) return true;

                if (prompt_history.empty() || prompt_history.back() != prompt)
                    prompt_history.push_back(prompt);
                prompt_history_idx = -1;

                {
                    std::lock_guard<std::mutex> tl(text_mutex);
                    messages.push_back({MsgType::USER, prompt});
                    thinking_text.clear();
                    response_text.clear();
                    footer_text = "Generazione in corso...";
                }

                if (prompt == "/exit") { screen.Exit(); return true; }

                generating = true;
                generating_since = std::chrono::steady_clock::now();
                if (prompt_callback) prompt_callback(prompt);
                return true;
            }

            // Ctrl+N, Ctrl+J, Ctrl+M: inserisci newline al cursore
            if ((event == Event::CtrlN || event == Event::CtrlJ || event == Event::CtrlM ||
                 (event.is_character() && event.character() == "\n")) && !generating) {
                std::lock_guard<std::mutex> tl(text_mutex);
                input_text.insert(input_cursor, "\n");
                input_cursor++;
                return true;
            }

            // Backspace: cancella a sinistra del cursore
            if (event == Event::Backspace) {
                if (generating) return true;
                std::lock_guard<std::mutex> tl(text_mutex);
                if (input_cursor > 0 && !input_text.empty()) {
                    input_text.erase(input_cursor - 1, 1);
                    input_cursor--;
                }
                return true;
            }

            // Delete: cancella a destra del cursore
            if (event == Event::Delete) {
                if (generating) return true;
                std::lock_guard<std::mutex> tl(text_mutex);
                if (input_cursor < (int)input_text.size()) {
                    input_text.erase(input_cursor, 1);
                }
                return true;
            }

            // Ctrl+A: inizio riga
            if (event == Event::CtrlA) {
                if (generating) return true;
                std::lock_guard<std::mutex> tl(text_mutex);
                input_cursor = 0;
                return true;
            }

            // Ctrl+E: fine riga
            if (event == Event::CtrlE) {
                if (generating) return true;
                std::lock_guard<std::mutex> tl(text_mutex);
                input_cursor = (int)input_text.size();
                return true;
            }

            // Ctrl+W o Alt+Backspace: cancella parola
            if (event == Event::CtrlW) {
                if (generating) return true;
                std::lock_guard<std::mutex> tl(text_mutex);
                int p = input_cursor - 1;
                while (p >= 0 && input_text[p] == ' ') p--;
                while (p >= 0 && input_text[p] != ' ') p--;
                if (p < 0) p = 0;
                int len = input_cursor - p;
                input_text.erase(p, len);
                input_cursor = p;
                return true;
            }

            // Tab: autocompletamento percorso file
            if ((event == Event::Tab || event == Event::TabReverse) && !generating) {
                std::lock_guard<std::mutex> tl(text_mutex);
                if (!input_text.empty() && input_cursor > 0) {
                    int start = input_cursor - 1;
                    while (start >= 0 && input_text[start] != ' ' && input_text[start] != '\n')
                        start--;
                    start++;
                    std::string partial = input_text.substr(start, input_cursor - start);
                    std::string completed = partial;
                    try {
                        std::string dir = ".";
                        std::string prefix = partial;
                        size_t slash = partial.find_last_of('/');
                        if (slash != std::string::npos) {
                            dir = partial.substr(0, slash);
                            if (dir.empty()) dir = "/";
                            prefix = partial.substr(slash + 1);
                        }
                        if (fs::exists(dir) && fs::is_directory(dir)) {
                            for (const auto & e : fs::directory_iterator(dir,
                                     fs::directory_options::skip_permission_denied)) {
                                std::string name = e.path().filename().string();
                                if (prefix.empty() || name.substr(0, prefix.size()) == prefix) {
                                    completed = partial.substr(0, partial.size() - prefix.size()) + name;
                                    if (e.is_directory()) completed += "/";
                                    break;
                                }
                            }
                        }
                    } catch (...) {}
                    if (completed != partial) {
                        input_text.replace(start, input_cursor - start, completed);
                        input_cursor = start + (int)completed.size();
                    }
                }
                return true;
            }

            // Caratteri stampabili: inserisci al cursore
            if (event.is_character() && event.character() != "\n" && event.character() != "\r") {
                std::lock_guard<std::mutex> tl(text_mutex);
                input_text.insert(input_cursor, event.character());
                input_cursor += (int)event.character().size();
                return true;
            }

            // Freccia SINISTRA: muovi cursore (se input non vuoto)
            if (event == Event::ArrowLeft && !generating) {
                if (input_text.empty() || input_cursor <= 0) return true;
                std::lock_guard<std::mutex> tl(text_mutex);
                if (input_cursor > 0) input_cursor--;
                return true;
            }

            // Freccia DESTRA: muovi cursore
            if (event == Event::ArrowRight && !generating) {
                if (input_text.empty() || input_cursor >= (int)input_text.size()) return true;
                std::lock_guard<std::mutex> tl(text_mutex);
                if (input_cursor < (int)input_text.size()) input_cursor++;
                return true;
            }

            // Home: inizio input se testo presente, altrimenti scroll inizio
            if (event == Event::Home && !generating) {
                if (!input_text.empty()) {
                    std::lock_guard<std::mutex> tl(text_mutex);
                    input_cursor = 0;
                } else {
                    scroll_y = 0.0f;
                }
                return true;
            }

            // End: fine input se testo presente, altrimenti scroll fine
            if (event == Event::End && !generating) {
                if (!input_text.empty()) {
                    std::lock_guard<std::mutex> tl(text_mutex);
                    input_cursor = (int)input_text.size();
                } else {
                    scroll_y = 1.0f;
                }
                return true;
            }

            // Mouse: wheel scroll
            if (event.is_mouse()) {
                auto & mouse = event.mouse();
                if (mouse.button == Mouse::WheelUp) {
                    scroll_y = std::max(0.0f, scroll_y - 0.15f);
                    return true;
                }
                if (mouse.button == Mouse::WheelDown) {
                    scroll_y = std::min(1.0f, scroll_y + 0.15f);
                    return true;
                }
            }

            // Freccia SU: cronologia prompt
            if (event == Event::ArrowUp && !generating) {
                std::lock_guard<std::mutex> tl(text_mutex);
                if (!prompt_history.empty()) {
                    if (prompt_history_idx < 0)
                        prompt_history_idx = (int)prompt_history.size() - 1;
                    else if (prompt_history_idx > 0)
                        prompt_history_idx--;
                    input_text = prompt_history[prompt_history_idx];
                    input_cursor = (int)input_text.size();
                }
                return true;
            }

            // Freccia GIU: cronologia prompt
            if (event == Event::ArrowDown && !generating) {
                std::lock_guard<std::mutex> tl(text_mutex);
                if (prompt_history_idx >= 0) {
                    prompt_history_idx++;
                    if (prompt_history_idx >= (int)prompt_history.size()) {
                        prompt_history_idx = -1;
                        input_text.clear();
                    } else {
                        input_text = prompt_history[prompt_history_idx];
                    }
                    input_cursor = (int)input_text.size();
                }
                return true;
            }

            // Scroll: PageUp/PageDown/Home/End
            if (event == Event::PageUp)       { scroll_y = std::max(0.0f, scroll_y - 0.3f); return true; }
            if (event == Event::PageDown)     { scroll_y = std::min(1.0f, scroll_y + 0.3f); return true; }
            if (event == Event::Home)         { scroll_y = 0.0f; return true; }
            if (event == Event::End)          { scroll_y = 1.0f; return true; }

            return false;
        });

        return handler;
    }
};

// ===========================================================================
// Costruttore / Distruttore
// ===========================================================================

FTXUI::FTXUI() : pimpl_(std::make_unique<Impl>()) {}
FTXUI::~FTXUI() { stop(); }

void FTXUI::init(bool) {
    pimpl_->running = true;
    pimpl_->footer_text = "Pronto. Carica modello con -m <path> e scrivi un messaggio.";
    pimpl_->prompt_callback = prompt_callback_;
    pimpl_->container = pimpl_->build_layout();
}

void FTXUI::set_prompt_callback(PromptCallback cb) {
    UI::set_prompt_callback(std::move(cb));
    if (pimpl_) pimpl_->prompt_callback = prompt_callback_;
}

void FTXUI::set_abort_callback(AbortCallback cb) {
    UI::set_abort_callback(std::move(cb));
    if (pimpl_) pimpl_->abort_callback = abort_callback_;
}

void FTXUI::run() {
    if (!initial_prompt_.empty()) {
        std::string p = initial_prompt_;
        initial_prompt_.clear();
        {
            std::lock_guard<std::mutex> tl(pimpl_->text_mutex);
            pimpl_->messages.push_back({FTXUI::Impl::MsgType::USER, p});
            pimpl_->thinking_text.clear();
            pimpl_->response_text.clear();
            pimpl_->footer_text = "Generazione in corso...";
        }
        pimpl_->generating = true;
        pimpl_->generating_since = std::chrono::steady_clock::now();
        if (prompt_callback_) prompt_callback_(p);
    }
    pimpl_->screen.Loop(pimpl_->container);
}

void FTXUI::stop() { pimpl_->running = false; pimpl_->screen.Exit(); }

void FTXUI::stream_token(const std::string & text, TokenType type) {
    {
        std::lock_guard<std::mutex> ql(pimpl_->queue_mutex);
        pimpl_->token_queue.push({text, type});
    }
    pimpl_->screen.RequestAnimationFrame();
}

void FTXUI::show_error(const std::string & msg) {
    {
        std::lock_guard<std::mutex> tl(pimpl_->text_mutex);
        pimpl_->footer_text = "ERRORE: " + msg;
    }
    pimpl_->generating = false;
    pimpl_->screen.RequestAnimationFrame();
}

void FTXUI::show_info(const std::string & msg) {
    {
        std::lock_guard<std::mutex> tl(pimpl_->text_mutex);
        pimpl_->footer_text = msg;
    }
    pimpl_->screen.RequestAnimationFrame();
}

void FTXUI::set_generating(bool gen) {
    {
        std::lock_guard<std::mutex> tl(pimpl_->text_mutex);
        if (!gen && pimpl_->generating) {
            if (!pimpl_->thinking_text.empty()) {
                pimpl_->messages.push_back({
                    FTXUI::Impl::MsgType::THINKING, pimpl_->thinking_text});
                pimpl_->thinking_text.clear();
            }
            if (!pimpl_->response_text.empty()) {
                pimpl_->messages.push_back({
                    FTXUI::Impl::MsgType::ASSISTANT, pimpl_->response_text});
                pimpl_->response_text.clear();
            }
            pimpl_->footer_text = "Pronto. Scrivi o /help per comandi.";
        }
    }
    pimpl_->generating = gen;
    pimpl_->screen.RequestAnimationFrame();
}

bool FTXUI::ask_permission(const std::string & name, const std::string & res) {
    {
        std::lock_guard<std::mutex> lk(pimpl_->permission_mutex);
        pimpl_->permission_tool = name;
        pimpl_->permission_resource = res;
        pimpl_->permission_allowed = false;
        pimpl_->permission_pending = true;
    }
    pimpl_->screen.RequestAnimationFrame();
    {
        std::unique_lock<std::mutex> lk(pimpl_->permission_mutex);
        pimpl_->permission_cv.wait(lk, [this] { return !pimpl_->permission_pending; });
    }
    return pimpl_->permission_allowed;
}

void FTXUI::update_stats(int tokens, float tps, size_t cache_size, int n_ctx, int n_past) {
    std::stringstream ss;
    ss << "Token: " << tokens << "  T/s: " << std::fixed << std::setprecision(1) << tps;
    if (n_ctx > 0) {
        int pct = (int)((float)n_past / n_ctx * 100.0f + 0.5f);
        ss << "  CTX: " << n_past << "/" << n_ctx << " (" << pct << "%)";
    } else if (cache_size > 0) {
        if (cache_size > 1024*1024) ss << "  Cache: " << (cache_size/(1024*1024)) << "MB";
        else if (cache_size > 1024) ss << "  Cache: " << (cache_size/1024) << "KB";
    }
    {
        std::lock_guard<std::mutex> tl(pimpl_->text_mutex);
        pimpl_->footer_text = ss.str();
    }
    pimpl_->screen.RequestAnimationFrame();
}

void FTXUI::show_history(const std::vector<std::pair<std::string, std::string>> & msgs) {
    std::lock_guard<std::mutex> tl(pimpl_->text_mutex);
    pimpl_->messages.clear();
    for (const auto & [role, content] : msgs) {
        if (role == "user")
            pimpl_->messages.push_back({FTXUI::Impl::MsgType::USER, content});
        else if (role == "assistant" || role == "model")
            pimpl_->messages.push_back({FTXUI::Impl::MsgType::ASSISTANT, content});
        else if (role == "system")
            pimpl_->messages.push_back({FTXUI::Impl::MsgType::SYSTEM, content});
        else if (role == "tool")
            pimpl_->messages.push_back({FTXUI::Impl::MsgType::TOOL_RESULT, content});
    }
    pimpl_->screen.RequestAnimationFrame();
}

void FTXUI::show_tool_execution(const std::string & name, const std::string & args) {
    {
        std::lock_guard<std::mutex> tl(pimpl_->text_mutex);
        pimpl_->messages.push_back({FTXUI::Impl::MsgType::TOOL_RESULT, args, name});
    }
    pimpl_->screen.RequestAnimationFrame();
}

void FTXUI::clear_response() {
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
