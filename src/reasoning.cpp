#include "reasoning.h"

#include <algorithm>
#include <cstring>

/*
 * ============================================================================
 * ReasoningDetector: rilevamento del pensiero interno del modello.
 *
 * Molti modelli LLM moderni (DeepSeek-R1, QwQ, Llama 3.1) producono una fase
 * di "ragionamento interno" prima della risposta finale. I token di questa
 * fase sono spesso delimitati da tag speciali:
 *
 * - DeepSeek: ...
 * - Qwen / QwQ: <thinking> ... </thinking>
 * - Altri: ... , ...
 *
 * Questo modulo analizza il flusso di token in tempo reale e classifica
 * ogni frammento come THINKING o RESPONSE, permettendo alla UI di
 * visualizzarli in pannelli separati.
 * ============================================================================
 */

ReasoningDetector::ReasoningDetector()
{
    // Tag di thinking predefiniti (DeepSeek-style)
    think_start_tags_ = {
        "...",
        "...",
        "<thinking>",
        "[thinking]",
        "Let me think about this",
        "Let me work through",
        "I need to think",
        "I'll reason",
        "Let me reason",
        "Let me analyze",
    };

    think_end_tags_ = {
        "...",
        "...",
        "</thinking>",
        "[/thinking]",
        "...",
        "...",
        "...",
    };
}

TokenType ReasoningDetector::classify(const std::string & token_piece)
{
    // Se forzato a response, ignora ogni logica di thinking
    if (force_response_) {
        has_seen_response_ = true;
        in_thinking_ = false;
        return TokenType::RESPONSE;
    }

    // Accumula il buffer parziale per match multi-token
    partial_buffer_ += token_piece;

    // --- Rilevamento apertura thinking ---
    if (!in_thinking_) {
        for (const auto & tag : think_start_tags_) {
            // Cerca il tag nel buffer accumulato
            if (partial_buffer_.find(tag) != std::string::npos) {
                in_thinking_ = true;
                partial_buffer_.clear();

                // Il tag di apertura e' stato consumato; il resto del buffer
                // (se esiste) fa già parte del pensiero
                return TokenType::THINKING;
            }
        }
    }

    // --- Rilevamento chiusura thinking ---
    if (in_thinking_ && !has_seen_response_) {
        for (const auto & tag : think_end_tags_) {
            if (partial_buffer_.find(tag) != std::string::npos) {
                in_thinking_ = false;
                has_seen_response_ = true;
                partial_buffer_.clear();
                return TokenType::THINKING;  // Il tag di chiusura è ancora thinking
            }
        }

        // Siamo in modalità thinking e non abbiamo trovato un tag di chiusura
        return TokenType::THINKING;
    }

    // --- Rilevamento euristico: se il modello non usa tag espliciti ---
    // Alcuni modelli passano naturalmente da thinking a response senza tag
    // Se abbiamo visto abbastanza token e il tono cambia, rileviamo la transizione

    if (!in_thinking_ && !has_seen_response_) {
        // Euristica semplice: i primi token sono probabilmente thinking
        // se contengono parole chiave di ragionamento
        // (utile per modelli senza tag espliciti)
        // ...
        // Per ora, se non abbiamo visto tag, assumiamo response diretto
        has_seen_response_ = true;
    }

    // Limite del buffer parziale per evitare crescita incontrollata
    if (partial_buffer_.size() > 1024) {
        partial_buffer_.clear();
    }

    return has_seen_response_ ? TokenType::RESPONSE : TokenType::THINKING;
}

void ReasoningDetector::reset()
{
    in_thinking_ = false;
    has_seen_response_ = false;
    partial_buffer_.clear();
}

void ReasoningDetector::set_chat_template(const std::string & tmpl)
{
    // Analizza il template per estrarre i tag think specifici del modello
    // Esempio da chat template: ... ...
    // TODO: parsing strutturato del template Jinja2
    // Per ora, i tag predefiniti coprono la maggior parte dei casi

    if (tmpl.find("...") != std::string::npos) {
        think_start_tags_.push_back("...");
        think_end_tags_.push_back("...");
    }
    if (tmpl.find("<thinking>") != std::string::npos) {
        think_start_tags_.push_back("<thinking>");
        think_end_tags_.push_back("</thinking>");
    }
}
