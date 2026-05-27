#ifndef LLAMA_AGENT_REASONING_H
#define LLAMA_AGENT_REASONING_H

#include <string>
#include <vector>
#include <functional>

/**
 * Classificazione di un token durante la generazione.
 */
enum class TokenType {
    THINKING,   // Token appartenente al ragionamento interno del modello
    RESPONSE,   // Token appartenente alla risposta finale
    TOOL_CALL,  // Indicatore visivo di chiamata tool
    UNKNOWN     // Token non ancora classificato
};

/**
 * Frammento di output classificato: un pezzo di testo con la sua natura.
 */
struct ClassifiedFragment {
    TokenType type;
    std::string text;
};

/**
 * Rilevatore di thinking / response.
 *
 * Molti modelli (DeepSeek, Qwen, Llama 3.1) producono token di ragionamento
 * interno prima della risposta finale. Questo modulo li rileva basandosi su:
 *
 * 1. Token speciali: ... ...
 * 2. Chattemplate: il template del modello GGUF può specificare i tag think
 * 3. Euristiche: pattern nel testo ("Let me think", "I need to", ecc.)
 *
 * I token vengono classificati in tempo reale durante lo streaming,
 * permettendo alla UI di visualizzarli in pannelli separati.
 */
class ReasoningDetector {
public:
    ReasoningDetector();

    /**
     * Analizza un singolo token e lo classifica.
     * Mantiene stato interno per rilevare la transizione thinking → response.
     *
     * @param token_piece Il testo del token decodificato
     * @return TokenType: THINKING, RESPONSE, o UNKNOWN
     */
    TokenType classify(const std::string & token_piece);

    /**
     * Reinizializza lo stato del rilevatore (nuovo turno di generazione).
     */
    void reset();

    /**
     * Imposta il template di chat (per rilevare tag think specifici del modello).
     */
    void set_chat_template(const std::string & tmpl);

    /**
     * Restituisce true se siamo attualmente in modalità thinking.
     */
    bool is_thinking() const { return in_thinking_; }

    /**
     * Forza la modalità (utile per debug o modelli senza thinking).
     */
    void set_force_response(bool force) { force_response_ = force; }

private:
    bool in_thinking_ = false;
    bool has_seen_response_ = false;
    bool force_response_ = false;

    // Pattern di thinking conosciuti (token che aprono la sezione di ragionamento)
    std::vector<std::string> think_start_tags_;
    std::vector<std::string> think_end_tags_;

    // Buffer per match parziale (tag possono essere suddivisi su più token)
    std::string partial_buffer_;

    /**
     * Verifica se il buffer contiene un tag di thinking completo.
     */
    bool check_think_tags(const std::string & text);
};

#endif // LLAMA_AGENT_REASONING_H
