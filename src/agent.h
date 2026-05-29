#ifndef LLAMA_AGENT_AGENT_H
#define LLAMA_AGENT_AGENT_H

#include "common.h"
#include "llama.h"

#include "kvcache.h"
#include "tools.h"
#include "permissions.h"
#include "reasoning.h"
#include "streaming.h"

#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <thread>

// Forward declaration
class UI;

/**
 * Agente conversazionale basato su llama.cpp.
 *
 * L'agente gestisce l'intero ciclo di vita di una conversazione:
 * - Caricamento del modello e del contesto
 * - Tokenizzazione / decodifica
 * - Loop di inferenza con streaming
 * - Rilevamento di thinking vs response
 * - Esecuzione di tool chiamati dal LLM
 * - Persistenza della KVCache su disco
 *
 * I parametri CLI sono gestiti tramite common_params (stessi flag di llama-cli).
 */
class Agent {
public:
    /**
     * Costruttore.
     * @param params Parametri common (model path, sampling, ecc.)
     */
    Agent(common_params & params, bool cache_disabled = false);

    /**
     * Distruttore: pulisce modello, contesto e backend.
     */
    ~Agent();

    /**
     * Inizializza il backend, carica il modello e prepara il contesto.
     * @return true se l'inizializzazione è riuscita
     */
    bool init();

    /**
     * Avvia l'agente: carica la KVCache se disponibile e gestisce la conversazione.
     * @param ui Riferimento all'interfaccia utente
     */
    void start(UI & ui);

    /**
     * Elabora un prompt utente e genera una risposta (thread-safe).
     * Pu essere chiamata dal thread della UI: l'inferenza viene spostata
     * su un thread separato per non bloccare l'interfaccia.
     */
    void process_prompt(const std::string & prompt);

    /**
     * Come process_prompt, ma eseguito SUL THREAD CHIAMANTE (sincrono).
     * Usato per single-turn da main: evita race condition su batch_/ctx_.
     */
    void process_prompt_sync(const std::string & prompt);

    /**
     * Rigenera l'ultima risposta.
     */
    void regenerate();

    /**
     * Interrompe la generazione in corso.
     */
    void abort() { interrupted_ = true; }

    /**
     * Pulisce la cronologia della conversazione.
     */
    void clear_history();

    /**
     * Restituisce il numero di token nel contesto.
     */
    int get_n_past() const { return n_past_; }

    /**
     * Restituisce il puntatore al contesto llama (per accesso diretto).
     */
    llama_context * get_context() const { return ctx_; }

    /**
     * Restituisce le statistiche correnti.
     */
    void get_stats(int & out_tokens, float & out_tps, size_t & out_cache_size) const;

    /**
     * Imposta la modalitÃ  di cache (fast/token).
     */
    void set_cache_mode(CacheMode mode) { kvcache_->set_mode(mode); }

    /**
     * Imposta il limite di tool call per turno (0 = illimitato).
     */
    void set_tool_limit(int limit) { tool_limit_ = limit; }

    /**
     * Notifica che la generazione è terminata (chiamato dal thread di inferenza).
     * Salva KVCache + cronologia conversazione su disco.
     */
    void on_generation_done();

private:
    common_params & params_;
    common_init_result_ptr llama_init_;
    llama_model * model_ = nullptr;
    llama_context * ctx_ = nullptr;
    llama_sampler * sampler_ = nullptr;
    llama_batch batch_;

    // Stato della conversazione
    std::vector<llama_token> conversation_tokens_;
    int n_past_ = 0;
    int turn_count_ = 0;

    // Cronologia testuale della conversazione (persistente su file)
    struct Message {
        std::string role;     // "user", "assistant", "system", "tool"
        std::string content;
    };
    std::vector<Message> history_;

    // Moduli
    std::unique_ptr<KVCacheManager> kvcache_;
    std::unique_ptr<ToolRegistry> tools_;
    std::unique_ptr<PermissionManager> permissions_;
    ReasoningDetector reasoning_;
    StreamingBuffer stream_;

    // UI (non owned)
    UI * ui_ = nullptr;

    // Flag per interruzione
    std::atomic<bool> interrupted_{false};

    // Flag per evitare double-save in on_generation_done
    bool done_called_ = false;

    // Limite tool call per turno (0 = illimitato)
    int tool_limit_ = 0;

    // Risultato dell'ultima generazione
    std::string last_response_;

    /**
     * Esegue la generazione token per token, chiamando la UI per ogni token.
     */
    void generate();

    /**
     * Elabora il prompt corrente e popola n_past.
     */
    bool eval_prompt(const std::vector<llama_token> & tokens);

    /**
     * Applica i sampler configurati e campiona il prossimo token.
     */
    llama_token sample_token();

    /**
     * Verifica se un token è di fine generazione.
     */
    bool is_eog(llama_token token) const;

    /**
     * Sistema prompt predefinito che descrive i tool disponibili.
     */
    std::string build_system_prompt() const;

    /**
     * Costruisce il prompt completo per il LLM (system + cronologia + input).
     */
    std::vector<llama_token> build_full_prompt(const std::string & user_input);

    /**
     * Elabora una tool call dalla risposta del LLM.
     */
    void handle_tool_call(const std::string & name,
                          const std::map<std::string, std::string> & args);

    /**
     * Compatta il contesto preservando i tool call e file operations.
     * keep_last: numero messaggi recenti da preservare.
     */
    void compact_context(size_t keep_last);

    // --- Persistenza conversazione ---

    /**
     * Salva la cronologia della conversazione come file JSON in .cache/.
     */
    void save_conversation();

    /**
     * Carica la cronologia della conversazione da file JSON in .cache/.
     * Restituisce true se il file esiste e  stato caricato.
     */
    bool load_conversation();

    /**
     * Restituisce il percorso del file di cronologia (.cache/conversation.json).
     */
    std::string conversation_path() const;
};

#endif // LLAMA_AGENT_AGENT_H
