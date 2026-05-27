#ifndef LLAMA_AGENT_UI_H
#define LLAMA_AGENT_UI_H

#include "streaming.h"

#include <string>
#include <thread>
#include <atomic>
#include <memory>
#include <functional>

// Forward declaration FTXUI (inclusione nell'implementazione)
// per evitare dipendenze nell'header

/**
 * Callback per quando l'utente invia un prompt.
 * Parametri: (testo_prompt)
 */
using PromptCallback = std::function<void(const std::string &)>;

/**
 * Interfaccia utente principale.
 *
 * Due implementazioni:
 * - UI_MODERNA: FTXUI con split panes THINKING + RESPONSE + input field
 * - UI_SEMPLICE: console ANSI base (stile llama-cli), selezionabile con --simple-ui
 */
class UI {
public:
    UI();
    virtual ~UI();

    /**
     * Inizializza l'interfaccia.
     * @param use_simple Se true, usa la console base invece di FTXUI
     */
    virtual void init(bool use_simple);

    /**
     * Avvia il loop principale dell'interfaccia.
     * Bloccante: ritorna quando l'utente esce.
     */
    virtual void run();

    /**
     * Ferma l'interfaccia e pulisce le risorse.
     */
    virtual void stop();

    /**
     * Mostra un token durante lo streaming.
     * Pu essere chiamata da qualsiasi thread.
     */
    virtual void stream_token(const std::string & text, TokenType type);

    /**
     * Imposta il callback per quando l'utente invia un prompt.
     */
    virtual void set_prompt_callback(PromptCallback cb);

    /**
     * Mostra un messaggio di errore all'utente.
     */
    virtual void show_error(const std::string & message);

    /**
     * Definisce il prompt iniziale (da -p) da inviare automaticamente
     * appena l'interfaccia è pronta. Usato per --single-turn.
     */
    void set_initial_prompt(const std::string & prompt) { initial_prompt_ = prompt; }

    /**
     * Imposta lo stato di generazione (true = in corso, false = completata).
     */
    virtual void set_generating(bool generating) {}

    /**
     * Mostra un messaggio informativo.
     */
    virtual void show_info(const std::string & message);

    /**
     * Mostra una richiesta di permesso all'utente.
     * Restituisce true se l'utente concede il permesso.
     */
    virtual bool ask_permission(const std::string & tool_name,
                                const std::string & resource);

    /**
     * Aggiorna le statistiche mostrate nel footer.
     */
    virtual void update_stats(int tokens_generated, float tokens_per_sec,
                              size_t cache_size, int n_ctx = 0, int n_past = 0);

    /**
     * Mostra la cronologia della conversazione nella UI.
     * Chiamata dopo il caricamento della cronologia persistente.
     */
    virtual void show_history(const std::vector<std::pair<std::string, std::string>> & messages);

    /**
     * Mostra che l'agente sta eseguendo un tool.
     */
    virtual void show_tool_execution(const std::string & tool_name,
                                     const std::string & args);

    /**
     * Pulisce la risposta corrente (per /regen).
     */
    virtual void clear_response();

protected:
    PromptCallback prompt_callback_;
    std::atomic<bool> running_{false};
    bool simple_mode_ = false;
    std::string initial_prompt_;  // prompt da -p (--single-turn)
};

// =============================================================================
// Implementazione FTXUI (UI moderna)
// =============================================================================

// NOTA: L'implementazione FTXUI è in ui_ftxui.cpp
// Entrambe le implementazioni vengono compilate sempre.
// La scelta tra FTXUI e SimpleUI avviene a runtime via --simple-ui.

/**
 * Interfaccia FTXUI: split panes, input field, colori, scroll.
 *
 * Dipende da FTXUI (libreria esterna, inclusa via FetchContent).
 * Se non disponibile, main.cpp usa SimpleUI.
 */
class FTXUI : public UI {
public:
    FTXUI();
    ~FTXUI() override;

    void init(bool use_simple) override;
    void set_prompt_callback(PromptCallback cb) override;
    void run() override;
    void stop() override;
    void stream_token(const std::string & text, TokenType type) override;
    void show_error(const std::string & message) override;
    void show_info(const std::string & message) override;
    void set_generating(bool generating) override;
    bool ask_permission(const std::string & tool_name,
                        const std::string & resource) override;
    void update_stats(int tokens_generated, float tokens_per_sec,
                      size_t cache_size, int n_ctx = 0, int n_past = 0) override;
    void show_history(const std::vector<std::pair<std::string, std::string>> & messages) override;
    void show_tool_execution(const std::string & tool_name,
                             const std::string & args) override;
    void clear_response() override;

private:
    struct Impl;
    std::unique_ptr<Impl> pimpl_;
};

/**
 * Interfaccia console semplice (stile llama-cli).
 *
 * Usa solo ANSI escape codes, nessuna dipendenza esterna.
 * Ideale per pipe, SSH, terminali minimali.
 */
class SimpleUI : public UI {
public:
    SimpleUI();
    ~SimpleUI() override;

    void init(bool use_simple) override;
    void run() override;
    void stop() override;
    void stream_token(const std::string & text, TokenType type) override;
    void show_error(const std::string & message) override;
    void show_info(const std::string & message) override;
    bool ask_permission(const std::string & tool_name,
                        const std::string & resource) override;
    void update_stats(int tokens_generated, float tokens_per_sec,
                      size_t cache_size, int n_ctx = 0, int n_past = 0) override;
    void show_history(const std::vector<std::pair<std::string, std::string>> & messages) override;
    void show_tool_execution(const std::string & tool_name,
                             const std::string & args) override;
    void clear_response() override;

private:
    struct Impl;
    std::unique_ptr<Impl> pimpl_;
};

#endif // LLAMA_AGENT_UI_H
