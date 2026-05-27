#include "agent.h"
#include "ui.h"

#include "arg.h"
#include "common.h"
#include "llama.h"
#include "log.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <chrono>
#include <thread>
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;

/*
 * ============================================================================
 * Agente conversazionale — Core
 *
 * Ciclo di vita:
 *   1. init() — carica modello, sampler, contesto
 *   2. start(UI) — collega callback prompt, carica KVCache + cronologia
 *   3. process_prompt(prompt) — thread separato:
 *        a. tokenizza prompt
 *        b. eval_prompt() — llama_decode del prompt
 *        c. generate() — loop di campionamento token-by-token
 *        d. stream token alla UI via callback
 *        e. rileva tool call e le esegue
 *        f. salva KVCache + cronologia su disco
 *   4. ~Agent() — cleanup
 *
 * Tutti i parametri CLI di llama-cli sono supportati tramite common_params.
 * ============================================================================
 */

// ===========================================================================
// Costruttore
// ===========================================================================

Agent::Agent(common_params & params, bool cache_disabled)
    : params_(params)
{
    // Directory di default per la cache
    std::string cache_dir = params.path_prompt_cache;
    if (cache_dir.empty()) {
        cache_dir = ".cache";
    }

    kvcache_ = std::make_unique<KVCacheManager>(cache_dir);
    if (cache_disabled) {
        kvcache_->set_enabled(false);
    }
    tools_   = std::make_unique<ToolRegistry>();
    permissions_ = std::make_unique<PermissionManager>();
}

// ===========================================================================
// Distruttore
// ===========================================================================

Agent::~Agent()
{
    // Salva stato prima di uscire (guard inserito in on_generation_done
    // previene il doppio salvataggio)
    if (ctx_) {
        on_generation_done();
    }

    // Pulisci risorse llama
    if (sampler_) {
        llama_sampler_free(sampler_);
        sampler_ = nullptr;
    }

    llama_batch_free(batch_);

    llama_init_.reset();  // common_init_result distrugge model e ctx
    llama_backend_free();
}

// ===========================================================================
// init — Inizializza backend, modello, sampler, batch
// ===========================================================================

bool Agent::init()
{
    fprintf(stderr, "[Agent] Inizializzazione backend llama.cpp...\n");
    llama_backend_init();
    llama_numa_init(params_.numa);

    fprintf(stderr, "[Agent] Caricamento modello: %s\n",
            params_.model.path.c_str());

    try {
        llama_init_ = common_init_from_params(params_);
    } catch (const std::exception & e) {
        fprintf(stderr, "\033[31m[Agent] Errore caricamento modello: %s\033[0m\n", e.what());
        return false;
    }

    if (!llama_init_) {
        fprintf(stderr, "\033[31m[Agent] common_init_from_params ha restituito nullptr\033[0m\n");
        return false;
    }

    model_ = llama_init_->model();
    ctx_   = llama_init_->context();

    if (!model_ || !ctx_) {
        fprintf(stderr, "\033[31m[Agent] Modello o contesto nullo\033[0m\n");
        return false;
    }

    // --- Inizializzazione sampler ---
    auto sparams = llama_sampler_chain_default_params();
    sampler_ = llama_sampler_chain_init(sparams);
    const auto & s = params_.sampling;

    if (s.penalty_last_n > 0) {
        llama_sampler_chain_add(sampler_,
            llama_sampler_init_penalties(s.penalty_last_n,
                                         s.penalty_repeat,
                                         s.penalty_freq,
                                         s.penalty_present));
    }
    if (s.top_k > 0)
        llama_sampler_chain_add(sampler_, llama_sampler_init_top_k(s.top_k));
    if (s.top_p < 1.0f && s.top_p > 0.0f)
        llama_sampler_chain_add(sampler_, llama_sampler_init_top_p(s.top_p, s.min_keep));
    if (s.min_p > 0.0f)
        llama_sampler_chain_add(sampler_, llama_sampler_init_min_p(s.min_p, s.min_keep));
    if (s.typ_p < 1.0f && s.typ_p > 0.0f)
        llama_sampler_chain_add(sampler_, llama_sampler_init_typical(s.typ_p, s.min_keep));
    if (s.temp > 0.0f)
        llama_sampler_chain_add(sampler_, llama_sampler_init_temp(s.temp));

    // --- Grammar-constrained decoding per tool call JSON ---
    // Usa una lazy grammar: si attiva quando il modello emette {"tool",
    // e forza la sintassi JSON valida per il resto del tool call.
    // Quando non attiva, il modello genera testo libero senza vincoli.
    {
        const char * grammar_str =
            "root ::= \"{\" ws \"\\\"tool\\\"\" ws \":\" ws string ws \",\" ws \"\\\"args\\\"\" ws \":\" ws \"{\" ws args ws \"}\" ws \"}\""
            "\n\n"
            "string ::="
            "\n  \"\\\"\" ("
            "\n    [^\"\\\\\\x7F\\x00-\\x1F] |"
            "\n    \"\\\\\" ([\"\\\\bfnrt] | \"u\" [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F])"
            "\n  )* \"\\\"\" ws"
            "\n\n"
            "args ::= arg (\",\" ws arg)*"
            "\narg ::= \"\\\"\" arg-name \"\\\"\" ws \":\" ws value"
            "\narg-name ::= \"command\" | \"path\" | \"content\" | \"pattern\" | \"url\" | \"format\" | \"timeout\" | \"tool\" | \"function\" | \"arguments\" | \"parameters\" | \"name\" | \"description\""
            "\n\n"
            "value ::= string | number | \"true\" | \"false\" | \"null\""
            "\nnumber ::= \"-\"? ([0-9] | [1-9] [0-9]{0,15}) (\".\" [0-9]+)?"
            "\n\n"
            "ws ::= | \" \" | \"\\n\" [ \\t]{0,20}";

        const char * trigger = "\\{\\\"tool";
        const char * trigger_patterns[] = { trigger };
        const llama_vocab * vocab = llama_model_get_vocab(model_);

        auto * grammar_sampler = llama_sampler_init_grammar_lazy_patterns(
            vocab, grammar_str, "root",
            trigger_patterns, 1,
            nullptr, 0);

        if (grammar_sampler) {
            llama_sampler_chain_add(sampler_, grammar_sampler);
        }
    }

    llama_sampler_chain_add(sampler_, llama_sampler_init_dist(s.seed));

    // Batch per decoding
    batch_ = llama_batch_init(params_.n_batch, 0, 1);

    // --- Carica KVCache persistente ---
    std::string cache_key = params_.model.path + "_ctx" + std::to_string(params_.n_ctx);
    kvcache_->set_cache_key(cache_key);

    std::vector<llama_token> cached_tokens;
    if (kvcache_->load(ctx_, cached_tokens)) {
        fprintf(stderr, "[Agent] KVCache caricata: %zu token da file\n", cached_tokens.size());

        // Ricostruisce la KVCache valutando i token uno per uno.
        // Invece di usare llama_state_load_file (che ha problemi di
        // cell metadata con modelli ricorrenti), salviamo solo i token
        // e ricostruiamo la cache valutandoli in batch.
        fprintf(stdout, "\r\033[K[KVCache] Ricostruzione %zu token...   0%%",
                cached_tokens.size());
        fflush(stdout);
        auto rebuild_start = std::chrono::high_resolution_clock::now();
        n_past_ = 0;
        int n_batch = params_.n_batch;
        int total = (int)cached_tokens.size();
        int bar_width = 30;
        for (int i = 0; i < total; i += n_batch) {
            int n_eval = std::min(total - i, n_batch);
            common_batch_clear(batch_);
            for (int j = 0; j < n_eval; j++) {
                common_batch_add(batch_, cached_tokens[i + j], n_past_, {0}, j == n_eval - 1);
                n_past_++;
            }
            if (llama_decode(ctx_, batch_)) {
                fprintf(stderr, "\n[Agent] Errore ricostruzione KVCache, reset\n");
                n_past_ = 0;
                cached_tokens.clear();
                break;
            }
            int pct = std::min(100, n_past_ * 100 / total);
            int filled = pct * bar_width / 100;
            fprintf(stdout, "\r\033[K[KVCache] \033[34m[");
            for (int b = 0; b < bar_width; b++) {
                fputc(b < filled ? '=' : (b == filled ? '>' : ' '), stdout);
            }
            fprintf(stdout, "\033[0m] %3d%%  (%d/%d token)", pct, n_past_, total);
            fflush(stdout);
        }
        fprintf(stdout, "\n");
        conversation_tokens_ = std::move(cached_tokens);
        fprintf(stderr, "[Agent] KVCache ricostruita: n_past=%d token\n", n_past_);

        // Carica anche la cronologia testuale
        if (load_conversation()) {
            fprintf(stderr, "[Agent] Cronologia conversazione caricata: %zu messaggi\n",
                    history_.size());
        }
    } else {
        fprintf(stderr, "[Agent] Nessuna cache trovata, partenza da zero\n");
    }

    fprintf(stderr, "[Agent] Pronto.\n");
    return true;
}

// ===========================================================================
// start — Collega UI e avvia il loop
// ===========================================================================

void Agent::start(UI & ui)
{
    ui_ = &ui;

    // Callback: quando l'utente preme Enter nella UI
    ui_->set_prompt_callback([this](const std::string & prompt) {
        process_prompt(prompt);
    });

    // Mostra stato iniziale e cronologia
    std::string status = "Modello: " + params_.model.path;
    if (!history_.empty()) {
        status += " | " + std::to_string(history_.size()) + " messaggi in cronologia";
        // Ricostruisce la cronologia nella UI
        std::vector<std::pair<std::string, std::string>> msgs;
        for (const auto & h : history_)
            msgs.push_back({h.role, h.content});
        ui_->show_history(msgs);
    }
    ui_->show_info(status);
    int n_ctx = ctx_ ? llama_n_ctx(ctx_) : 0;
    ui_->update_stats(n_past_, 0.0f, ctx_ ? llama_state_get_size(ctx_) : 0,
                      n_ctx, n_past_);

    // Se c'è un prompt iniziale da -p e siamo in single-turn,
    // processalo sincronamente (NON in thread separato)
    if (params_.single_turn && !params_.prompt.empty()) {
        process_prompt_sync(params_.prompt);
        params_.prompt.clear();
        on_generation_done();
        ui_->stop();
        return;
    }

    // Altrimenti passa il prompt alla UI per elaborazione asincrona
    if (!params_.prompt.empty()) {
        ui_->set_initial_prompt(params_.prompt);
        params_.prompt.clear();
    }

    // Avvia il loop della UI (bloccante)
    ui_->run();
}

// ===========================================================================
// process_prompt_sync — Elabora il prompt SUL THREAD CHIAMANTE
// ===========================================================================

void Agent::process_prompt_sync(const std::string & prompt)
{
    fprintf(stderr, "\033[34m[Agent] process_prompt_sync: prompt=\"%s\" n_past=%d turn=%zu ctx=%p\033[0m\n",
            prompt.c_str(), n_past_, (size_t)turn_count_, (void*)ctx_);

    done_called_ = false;

    // --- Comandi speciali ---
    if (prompt == "/help") {
        std::string help =
            "Comandi disponibili:\n"
            "  /help    — Mostra questo aiuto\n"
            "  /clear   — Cancella la cronologia\n"
            "  /regen   — Rigenera l'ultima risposta\n"
            "  /exit    — Esci\n"
            "\n"
            "Tool disponibili: " + tools_->list_tool_names() + "\n";
        if (ui_) ui_->show_info(help);
        return;
    }
    if (prompt == "/clear") {
        clear_history();
        if (ui_) ui_->show_info("Cronologia cancellata.");
        return;
    }
    if (prompt == "/regen") {
        regenerate();
        return;
    }

    // --- Feedback: preparazione ---
    if (ui_) ui_->show_info("Preparazione prompt in corso...");

    // --- Aggiunge il messaggio utente alla cronologia ---
    history_.push_back({"user", prompt});

    // --- Costruisce il prompt completo ---
    std::string full_text;

    if (n_past_ == 0 && turn_count_ == 0) {
        full_text = build_system_prompt() + "\n\n";
        for (const auto & msg : history_) {
            if (msg.role == "user" && msg.content == prompt) continue;
            full_text += msg.role + ": " + msg.content + "\n\n";
        }
        full_text += "user: " + prompt + "\n\nassistant: ";
    } else {
        // Separa dal turno precedente con un newline
        full_text = "\nuser: " + prompt + "\n\nassistant: ";
    }

    // --- Tokenizza ---
    if (ui_) ui_->show_info("Tokenizzazione...");
    std::vector<llama_token> tokens = common_tokenize(ctx_, full_text, true);

    // Verifica limite contesto
    int n_ctx = llama_n_ctx(ctx_);
    if (n_past_ + (int)tokens.size() > n_ctx - 128) {
        fprintf(stderr, "[Agent] Contesto esaurito, reset...\n");
        llama_memory_clear(llama_get_memory(ctx_), true);
        n_past_ = 0;
        conversation_tokens_.clear();
        full_text = build_system_prompt() + "\n\nuser: " + prompt + "\n\nassistant: ";
        tokens = common_tokenize(ctx_, full_text, true);
    }

    if (tokens.empty()) {
        if (ui_) ui_->show_error("Impossibile tokenizzare");
        return;
    }

    // --- Valuta il prompt in batch ---
    fprintf(stderr, "\033[34m[Agent] Valutazione prompt: %zu token, n_past=%d\033[0m\n",
            tokens.size(), n_past_);

    if (ui_) {
        ui_->show_info("Elaborazione prompt: " +
            std::to_string(tokens.size()) + " token...");
    }

    int n_batch = params_.n_batch;
    auto eval_start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < (int)tokens.size(); i += n_batch) {
        int n_eval = std::min((int)tokens.size() - i, n_batch);
        common_batch_clear(batch_);
        for (int j = 0; j < n_eval; j++) {
            common_batch_add(batch_, tokens[i + j], n_past_, {0}, j == n_eval - 1);
            n_past_++;
        }
        char batch_msg[256];
        snprintf(batch_msg, sizeof(batch_msg),
                 "Batch %d/%d (%d tok, pos=%d)...",
                 i / n_batch + 1, ((int)tokens.size() + n_batch - 1) / n_batch,
                 n_eval, n_past_ - n_eval);
        fprintf(stderr, "\033[34m[Agent] llama_decode %s\033[0m\n", batch_msg);
        if (ui_) ui_->show_info(batch_msg);
        std::atomic<bool> decode_done{false};
        auto decode_start = std::chrono::high_resolution_clock::now();
        std::thread progress_thread([this, &decode_done, &decode_start]() {
            for (int k = 0; k < 120 && !decode_done; k++) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                if (!decode_done) {
                    auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::high_resolution_clock::now() - decode_start).count();
                    char buf[128];
                    snprintf(buf, sizeof(buf), "Decode in corso... (%llds)", (long long)secs);
                    fprintf(stderr, "\033[34m[Agent] %s\033[0m\n", buf);
                    if (ui_) ui_->show_info(buf);
                }
            }
        });
        progress_thread.detach();
        int ret = llama_decode(ctx_, batch_);
        decode_done = true;
        auto decode_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::high_resolution_clock::now() - decode_start).count();
        fprintf(stderr, "\033[34m[Agent] llama_decode batch finito: ret=%d (%lldms)\033[0m\n",
                ret, (long long)decode_ms);
        if (ret) {
            if (ui_) ui_->show_error("Errore llama_decode nel prompt");
            return;
        }
        // Feedback ogni batch: mostra quanti token elaborati
        if (ui_ && (i % (n_batch * 4) == 0)) {
            ui_->show_info("Prompt: " + std::to_string(
                std::min(i + n_batch, (int)tokens.size())) +
                "/" + std::to_string(tokens.size()) + " token...");
        }
    }

    conversation_tokens_.insert(
        conversation_tokens_.end(), tokens.begin(), tokens.end());

    // Salva checkpoint prompt-only per /regen
    kvcache_->save(ctx_, conversation_tokens_, true);

    // --- Genera la risposta ---
    if (ui_) ui_->show_info("Generazione...");
    generate();
}

// ===========================================================================
// process_prompt — Lancia l'inferenza in un thread separato
// ===========================================================================

void Agent::process_prompt(const std::string & prompt)
{
    auto self = this;

    std::thread([self, prompt]() {
        try {
            self->process_prompt_sync(prompt);
            self->on_generation_done();
        } catch (const std::exception & e) {
            fprintf(stderr, "\033[31m[Agent] Eccezione nel thread: %s\033[0m\n", e.what());
            if (self->ui_) {
                self->ui_->show_error(std::string("Errore: ") + e.what());
            }
            self->on_generation_done();
        } catch (...) {
            fprintf(stderr, "\033[31m[Agent] Eccezione sconosciuta nel thread\033[0m\n");
            if (self->ui_) {
                self->ui_->show_error("Errore sconosciuto durante la generazione");
            }
            self->on_generation_done();
        }
    }).detach();
}

// ===========================================================================
// on_generation_done — Salva KVCache + cronologia, notifica UI
// ===========================================================================

void Agent::on_generation_done()
{
    if (done_called_) return;
    done_called_ = true;

    // Salva KVCache
    if (ctx_ && !conversation_tokens_.empty()) {
        kvcache_->save(ctx_, conversation_tokens_);
    }

    // Salva cronologia testuale
    save_conversation();

    // Notifica UI
    if (ui_) {
        ui_->update_stats(n_past_, 0.0f, ctx_ ? llama_state_get_size(ctx_) : 0,
                          llama_n_ctx(ctx_), n_past_);
        ui_->set_generating(false);
        ui_->show_info("Completato. " + std::to_string(history_.size()) +
                       " messaggi, " + std::to_string(n_past_) + " token.");

        // In modalità single-turn, esci dopo la prima risposta
        if (params_.single_turn) {
            ui_->stop();
        }
    }
}

// ===========================================================================
// regenerate — Rigenera l'ultima risposta
// ===========================================================================

void Agent::regenerate()
{
    if (history_.empty()) {
        if (ui_) ui_->show_error("Niente da rigenerare");
        return;
    }

    // Rimuove l'ultima risposta dell'assistant dalla cronologia
    if (!history_.empty() && history_.back().role == "assistant") {
        history_.pop_back();
    }

    // Ricarica la KVCache prompt-only (torna allo stato prima dell'ultima risposta)
    std::vector<llama_token> cached;
    if (kvcache_->load(ctx_, cached, true)) {
        conversation_tokens_ = std::move(cached);
        n_past_ = (int)conversation_tokens_.size();
    } else {
        // Se non c'Ã¨ cache, resetta
        llama_memory_clear(llama_get_memory(ctx_), true);
        n_past_ = 0;
        conversation_tokens_.clear();
    }

    // Se c'Ã¨ un ultimo messaggio utente, riprocessalo
    if (!history_.empty() && history_.back().role == "user") {
        std::string last_prompt = history_.back().content;
        history_.pop_back();  // lo togliamo, process_prompt lo ri-aggiunger
        process_prompt_sync(last_prompt);
    }
}

// ===========================================================================
// clear_history
// ===========================================================================

void Agent::clear_history()
{
    llama_memory_clear(llama_get_memory(ctx_), true);
    conversation_tokens_.clear();
    n_past_ = 0;
    turn_count_ = 0;
    done_called_ = false;
    history_.clear();
    reasoning_.reset();
    stream_.reset();
    kvcache_->clear();

    // Elimina anche il file di cronologia
    std::string path = conversation_path();
    if (!path.empty() && fs::exists(path)) {
        fs::remove(path);
    }

    if (ui_) {
        ui_->clear_response();
        ui_->update_stats(0, 0.0f, 0);
    }
}

// ===========================================================================
// generate — Loop token-by-token (NEL THREAD DI INFERENZA)
// ===========================================================================

void Agent::generate()
{
    fprintf(stderr, "\033[34m[Agent] generate() inizia: n_past=%d turn=%d conv_tokens=%zu\033[0m\n",
            n_past_, turn_count_, conversation_tokens_.size());

    reasoning_.reset();
    stream_.reset();
    stream_.begin_turn(turn_count_);

    // Collega lo streaming alla UI
    if (ui_) {
        stream_.set_callback([this](const std::string & text, TokenType type, int) {
            ui_->stream_token(text, type);
        });
    }

    int max_tokens = params_.n_predict > 0 ? params_.n_predict : 2048;
    auto start_time = std::chrono::high_resolution_clock::now();

    std::string current_response;
    int tool_call_count = 0;
    const int MAX_TOOL_CALLS = 10;

    auto last_stats_time = start_time;

    for (int i = 0; i < max_tokens; i++) {
        if (interrupted_) break;

        // Campiona prossimo token
        llama_token token = llama_sampler_sample(sampler_, ctx_, -1);

        // Fine generazione
        if (llama_vocab_is_eog(llama_model_get_vocab(model_), token)) {
            break;
        }

        // Decodifica token in testo
        std::string piece = common_token_to_piece(ctx_, token);

        // Classifica thinking/response
        TokenType type = reasoning_.classify(piece);

        // Stream alla UI
        stream_.push(piece, type);
        current_response += piece;

        // Prepara batch per il prossimo passo
        common_batch_clear(batch_);
        common_batch_add(batch_, token, n_past_, {0}, true);
        n_past_++;
        conversation_tokens_.push_back(token);  // traccia TUTTI i token

        if (llama_decode(ctx_, batch_)) {
            fprintf(stderr, "[Agent] Errore llama_decode\n");
            if (ui_) {
                ui_->show_error("Errore llama_decode durante generazione");
            }
            break;
        }

        // Rilevamento tool call (ogni token, solo se inizia JSON)
        if (current_response.size() > 8 &&
            (current_response.find("{\"tool\"") != std::string::npos ||
             current_response.find("{\"function\"") != std::string::npos ||
             current_response.find("```json") != std::string::npos))
        {
            std::string tool_name;
            std::map<std::string, std::string> tool_args;
            if (tools_->parse_tool_call(current_response, tool_name, tool_args)) {
                tool_call_count++;
                if (tool_call_count > MAX_TOOL_CALLS) break;

                handle_tool_call(tool_name, tool_args);
                current_response.clear();
                continue;
            }
        }

        // Aggiorna statistiche ogni token (o ogni 5 per performance)
        auto now = std::chrono::high_resolution_clock::now();
        auto elapsed_stats = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_stats_time).count();
        if (elapsed_stats > 200 || i == 0) {  // ogni 200ms
            last_stats_time = now;
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - start_time).count();
            float tps = elapsed > 0 ? (float)(i + 1) / (elapsed / 1000.0f) : 0.0f;
            if (ui_) {
                int n_ctx = llama_n_ctx(ctx_);
                ui_->update_stats(n_past_, tps, llama_state_get_size(ctx_),
                                  llama_n_ctx(ctx_), n_past_);
            }
        }
    }

    stream_.finish();

    // Salva la risposta nella cronologia
    history_.push_back({"assistant", current_response});
    last_response_ = current_response;

    // Se la risposta è vuota (possibile problema KVCache reload), segnala
    if (current_response.empty()) {
        fprintf(stderr, "\033[33m[Agent] ATTENZIONE: Risposta vuota generata (n_past=%d turn=%d, conv_tokens=%zu)\033[0m\n",
                n_past_, turn_count_, conversation_tokens_.size());
    }

    // Calcola statistiche finali
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now() - start_time).count();

    int gen_count = 0;
    for (const auto & msg : history_)
        if (msg.role == "assistant") gen_count++;

    if (elapsed > 0 && ui_) {
        float tps = (float)max_tokens / (elapsed / 1000.0f);
        ui_->update_stats(n_past_, tps, llama_state_get_size(ctx_),
                          llama_n_ctx(ctx_), n_past_);
    }

    turn_count_++;
    fprintf(stderr, "\033[34m[Agent] generate() finito: resp_len=%zu n_past=%d turn=%d\033[0m\n",
            current_response.size(), n_past_, turn_count_);
}

// ===========================================================================
// handle_tool_call
// ===========================================================================

void Agent::handle_tool_call(const std::string & name,
                              const std::map<std::string, std::string> & args)
{
    if (!ui_) return;

    // Prepara stringa args compatta per UI
    std::string args_str;
    for (const auto & [k, v] : args) {
        if (!args_str.empty()) args_str += ", ";
        // Mostra solo i primi 60 caratteri del valore
        std::string val = v.size() > 60 ? v.substr(0, 60) + "..." : v;
        args_str += k + "=\"" + val + "\"";
    }
    std::string tool_msg = "\n  >> " + name + "(" + args_str + ")";
    ui_->stream_token(tool_msg, TokenType::TOOL_CALL);

    // Verifica permessi
    PermissionAction perm = permissions_->check(name, args_str);

    if (perm == PermissionAction::ASK) {
        if (!ui_->ask_permission(name, args_str)) {
            std::string msg = "\n[Tool negato: " + name + "]\n";
            std::vector<llama_token> t = common_tokenize(ctx_, msg, true);
            for (size_t i = 0; i < t.size(); i++) {
                common_batch_clear(batch_);
                common_batch_add(batch_, t[i], n_past_, {0}, i == t.size() - 1);
                n_past_++;
                llama_decode(ctx_, batch_);
            }
            return;
        }
    } else if (perm == PermissionAction::DENY) {
        std::string msg = "\n[Tool bloccato: " + name + "]\n";
        std::vector<llama_token> t = common_tokenize(ctx_, msg, true);
        for (size_t i = 0; i < t.size(); i++) {
            common_batch_clear(batch_);
            common_batch_add(batch_, t[i], n_past_, {0}, i == t.size() - 1);
            n_past_++;
            llama_decode(ctx_, batch_);
        }
        return;
    }

    // Esegue il tool
    ToolResult result = tools_->execute(name, args);

    std::string feedback;
    if (result.success) {
        feedback = "\n  >> " + name + " OK:\n" + result.output + "\n";
    } else {
        feedback = "\n  >> " + name + " ERR: " + result.error + "\n";
    }

    // Inietta il risultato nel contesto
    std::vector<llama_token> ft = common_tokenize(ctx_, feedback, true);
    for (size_t i = 0; i < ft.size(); i++) {
        common_batch_clear(batch_);
        common_batch_add(batch_, ft[i], n_past_, {0}, i == ft.size() - 1);
        n_past_++;
        if (llama_decode(ctx_, batch_)) {
            ui_->show_error("Errore nell'iniezione del risultato del tool");
            break;
        }
    }
}

// ===========================================================================
// build_system_prompt
// ===========================================================================

std::string Agent::build_system_prompt() const
{
    return
        "Sei un assistente AI con accesso ai seguenti strumenti:\n\n"
        + tools_->to_json_schema() + "\n\n"
        "Per usare uno strumento, rispondi con un blocco JSON:\n"
        "```json\n{\"tool\": \"nome\", \"args\": {\"param\": \"valore\"}}\n```\n\n"
        "Dopo il risultato, continua la conversazione.\n"
        "Se non servono strumenti, rispondi normalmente.\n"
        "Pensa passo-passo.\n";
}

// ===========================================================================
// get_stats
// ===========================================================================

void Agent::get_stats(int & out_tokens, float & out_tps, size_t & out_cache_size) const
{
    out_tokens = n_past_;
    out_tps = 0.0f;
    out_cache_size = ctx_ ? llama_state_get_size(ctx_) : 0;
}

// ===========================================================================
// Persistenza conversazione (JSON)
// ===========================================================================

std::string Agent::conversation_path() const
{
    std::string dir = params_.path_prompt_cache;
    if (dir.empty()) dir = ".cache";
    return (fs::path(dir) / "conversation.json").string();
}

void Agent::save_conversation()
{
    if (history_.empty()) return;

    std::string path = conversation_path();
    if (path.empty()) return;

    // Crea la directory se non esiste
    fs::create_directories(fs::path(path).parent_path());

    std::ofstream file(path);
    if (!file.is_open()) {
        fprintf(stderr, "[Agent] Impossibile salvare cronologia: %s\n", path.c_str());
        return;
    }

    // Formato JSON semplice: [{"role":"...","content":"..."}, ...]
    file << "[\n";
    for (size_t i = 0; i < history_.size(); i++) {
        const auto & msg = history_[i];
        file << "  {\n";
        file << "    \"role\": \"" << msg.role << "\",\n";

        // Escape del contenuto (sostituisce " con \")
        std::string escaped = msg.content;
        size_t pos = 0;
        while ((pos = escaped.find('"', pos)) != std::string::npos) {
            escaped.replace(pos, 1, "\\\"");
            pos += 2;
        }
        // Escape newline
        pos = 0;
        while ((pos = escaped.find('\n', pos)) != std::string::npos) {
            escaped.replace(pos, 1, "\\n");
            pos += 2;
        }

        file << "    \"content\": \"" << escaped << "\"\n";
        file << "  }";
        if (i < history_.size() - 1) file << ",";
        file << "\n";
    }
    file << "]\n";
    file.close();

    fprintf(stderr, "[Agent] Cronologia salvata: %zu messaggi in %s\n",
            history_.size(), path.c_str());
}

bool Agent::load_conversation()
{
    std::string path = conversation_path();
    if (path.empty() || !fs::exists(path)) return false;

    std::ifstream file(path);
    if (!file.is_open()) return false;

    // Parsing JSON semplice (senza libreria esterna)
    // Legge riga per riga e cerca "role" e "content"
    std::string line;
    std::string current_role;
    std::string current_content;
    bool in_content = false;

    while (std::getline(file, line)) {
        // Cerca "role": "..."
        auto role_pos = line.find("\"role\"");
        if (role_pos != std::string::npos) {
            auto start = line.find('"', role_pos + 7);
            if (start != std::string::npos) {
                auto end = line.find('"', start + 1);
                if (end != std::string::npos) {
                    current_role = line.substr(start + 1, end - start - 1);
                }
            }
            continue;
        }

        // Cerca "content": "..."
        auto cont_pos = line.find("\"content\"");
        if (cont_pos != std::string::npos) {
            auto start = line.find('"', cont_pos + 9);
            if (start != std::string::npos) {
                in_content = true;
                current_content.clear();
                // Legge tutto fino alla fine del valore
                // (puÃ² essere su piÃ¹ righe se ci sono \\n)
                bool done = false;
                while (!done) {
                    auto end = line.find('"', start + 1);
                    if (end != std::string::npos) {
                        // Verifica che la " non sia escaped
                        if (end > 0 && line[end - 1] == '\\') {
                            current_content += line.substr(start + 1, end - start);
                            start = end + 1;
                        } else {
                            current_content += line.substr(start + 1, end - start - 1);
                            done = true;
                        }
                    } else {
                        current_content += line.substr(start + 1) + "\n";
                        if (!std::getline(file, line)) break;
                        start = 0;
                    }
                }

                // De-escape
                size_t p = 0;
                while ((p = current_content.find("\\n", p)) != std::string::npos) {
                    current_content.replace(p, 2, "\n");
                    p++;
                }
                p = 0;
                while ((p = current_content.find("\\\"", p)) != std::string::npos) {
                    current_content.replace(p, 2, "\"");
                    p++;
                }

                in_content = false;

                if (!current_role.empty()) {
                    history_.push_back({current_role, current_content});
                    current_role.clear();
                }
            }
        }
    }

    file.close();

    if (!history_.empty()) {
        fprintf(stderr, "[Agent] Cronologia caricata: %zu messaggi\n", history_.size());
        return true;
    }

    return false;
}
