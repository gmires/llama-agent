#ifndef LLAMA_AGENT_STREAMING_H
#define LLAMA_AGENT_STREAMING_H

#include "reasoning.h"

#include <string>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>

/**
 * Callback invocata per ogni frammento di token durante lo streaming.
 * Parametri: (testo, tipo_token, indice_turno)
 */
using TokenCallback = std::function<void(const std::string &, TokenType, int)>;

/**
 * Buffer thread-safe per lo streaming dei token.
 *
 * L'inferenza avviene in un thread separato; i token prodotti vengono
 * accodati qui e consumati dal thread della UI. La coda supporta:
 *
 * - Push da thread producer (inferenza)
 * - Pop/blocco da thread consumer (UI)
 * - Notifica immediata via condition_variable
 * - Accumulo del testo completo per ogni turno
 */
class StreamingBuffer {
public:
    StreamingBuffer();

    /**
     * Aggiunge un token alla coda di output.
     * Thread-safe: chiamabile dal thread di inferenza.
     */
    void push(const std::string & text, TokenType type);

    /**
     * Segnala la fine della generazione corrente.
     */
    void finish();

    /**
     * Inizia un nuovo turno di generazione.
     */
    void begin_turn(int turn_index);

    /**
     * Restituisce il testo accumulato per il tipo specificato nel turno corrente.
     */
    std::string get_accumulated(TokenType type) const;

    /**
     * Registra un callback per la notifica di nuovi token.
     */
    void set_callback(TokenCallback cb);

    /**
     * Resetta il buffer per un nuovo turno.
     */
    void reset();

    /**
     * Restituisce true se la generazione è terminata.
     */
    bool is_finished() const { return finished_; }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;

    // Accumulatori di testo per tipo
    std::string thinking_text_;
    std::string response_text_;

    // Stato
    std::atomic<bool> finished_{false};
    int current_turn_ = 0;

    // Callback opzionale
    TokenCallback callback_;
};

#endif // LLAMA_AGENT_STREAMING_H
